/* MPL-2.0. Process isolation uses exec, never fork of a live threaded VM. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lunaria_os.h"
#include "luna_google.h"
#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#define QUEUE_MAX (8u * LUNA_PROCESS_MESSAGE_MAX)
#define PID_STRIDE 4096u
#define PID_LIMIT 4194304u
#define NAME_MAX_BYTES 511u
struct packet { struct packet *next; size_t size,offset; unsigned char bytes[]; };
struct luna_process_channel {
   pthread_mutex_t lock;
   int fd,error;
   struct packet *head,*tail;
   size_t queued;
   unsigned char header[8];
   size_t header_size,received;
   struct packet *incoming;
   struct packet *ready_head,*ready_tail;
};
struct child {
   struct child *next;
   char *name;
   pid_t host_pid;
   uint32_t guest_pid;
   struct luna_process_channel *channel;
   bool dead,drained;
   int status;
};
struct exit_notice { struct exit_notice *next; char *name; uint32_t pid; int status; };
struct luna_process_pool {
   pthread_mutex_t lock;
   char *executable,*mode,*host_apk;
   uint32_t parent_pid,next_pid,guest_uid;
   struct child *children;
   struct exit_notice *exits,*exits_tail;
   struct reply *ready_head,*ready_tail;
   int error;
};
static void put32(unsigned char *p,uint32_t v)
{ for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(i*8)); }
static uint32_t get32(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void packets_free(struct packet *p)
{ while(p){struct packet *next=p->next;free(p);p=next;} }
struct luna_process_channel *luna_process_channel_open(int fd)
{
   if(fd<0){errno=EBADF;return NULL;}
   int flags=fcntl(fd,F_GETFL),cloexec=fcntl(fd,F_GETFD);
   if(flags<0 || cloexec<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0 ||
      fcntl(fd,F_SETFD,cloexec|FD_CLOEXEC)<0){int e=errno;close(fd);errno=e;return NULL;}
   struct luna_process_channel *c=calloc(1,sizeof *c);
   if(!c){close(fd);return NULL;}
   int e=pthread_mutex_init(&c->lock,NULL);
   if(e){close(fd);free(c);errno=e;return NULL;}
   c->fd=fd;return c;
}
static int flush(struct luna_process_channel *c)
{
   while(c->head){
      struct packet *p=c->head;
      ssize_t n=send(c->fd,p->bytes+p->offset,p->size-p->offset,MSG_NOSIGNAL);
      if(n<0 && errno==EINTR)continue;
      if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))return 0;
      if(n<=0)return -(n<0 ? errno : EPIPE);
      p->offset+=(size_t)n;c->queued-=(size_t)n;
      if(p->offset==p->size){c->head=p->next;if(!c->head)c->tail=NULL;free(p);}
   }
   return 0;
}
int luna_process_channel_send(struct luna_process_channel *c,const void *data,size_t size)
{
   if(!c || (!data && size))return -EINVAL;
   if(size>LUNA_PROCESS_MESSAGE_MAX)return -EMSGSIZE;
   pthread_mutex_lock(&c->lock);
   int rc=c->error ? -c->error : 0;
   if(!rc && c->queued+size+8>QUEUE_MAX)rc=-ENOBUFS;
   struct packet *p=rc ? NULL : malloc(sizeof *p+size+8);
   if(!rc && !p)rc=-ENOMEM;
   if(!rc){
      p->next=NULL;p->offset=0;p->size=size+8;
      memcpy(p->bytes,"LSP1",4);put32(p->bytes+4,(uint32_t)size);
      if(size)memcpy(p->bytes+8,data,size);
      if(c->tail)c->tail->next=p;else c->head=p;c->tail=p;c->queued+=p->size;
      rc=flush(c);if(rc)c->error=-rc;
   }
   pthread_mutex_unlock(&c->lock);return rc;
}
int luna_process_channel_tick(struct luna_process_channel *c,luna_process_message_fn callback,void *user)
{
   if(!c)return -EINVAL;
   struct packet *head=NULL,*tail=NULL;int count=0;
   pthread_mutex_lock(&c->lock);
   int rc=c->error ? -c->error : flush(c);
   while(!rc && count<32){
      unsigned char *to;size_t left;
      if(c->header_size<8){to=c->header+c->header_size;left=8-c->header_size;}
      else{
         if(!c->incoming){
            uint32_t size=get32(c->header+4);
            if(memcmp(c->header,"LSP1",4) || size>LUNA_PROCESS_MESSAGE_MAX){rc=-EPROTO;break;}
            c->incoming=malloc(sizeof *c->incoming+size);
            if(!c->incoming){rc=-ENOMEM;break;}
            c->incoming->next=NULL;c->incoming->size=size;c->incoming->offset=0;c->received=0;
         }
         to=c->incoming->bytes+c->received;left=c->incoming->size-c->received;
         if(!left){
            if(tail)tail->next=c->incoming;else head=c->incoming;
            tail=c->incoming;c->incoming=NULL;c->header_size=0;++count;continue;
         }
      }
      ssize_t n=recv(c->fd,to,left,0);
      if(n<0 && errno==EINTR)continue;
      if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))break;
      if(n<=0){rc=n<0 ? -errno : (c->header_size || c->incoming ? -EPROTO : -EPIPE);break;}
      if(c->header_size<8)c->header_size+=(size_t)n;else c->received+=(size_t)n;
   }
   if(rc)c->error=-rc;
   /* Keep unread callbacks on the channel, not on this stack. A callback
    * may pump this channel again while waiting for a synchronous Binder reply. */
   if(head){
      if(c->ready_tail)c->ready_tail->next=head;else c->ready_head=head;
      c->ready_tail=tail;
   }
   pthread_mutex_unlock(&c->lock);
   for(unsigned delivered=0;delivered<32;++delivered){
      pthread_mutex_lock(&c->lock);
      struct packet *p=c->ready_head;
      if(p){c->ready_head=p->next;if(!c->ready_head)c->ready_tail=NULL;}
      pthread_mutex_unlock(&c->lock);
      if(!p)break;
      if(callback)callback(user,p->bytes,p->size);
      free(p);
   }
   return rc ? rc : count;
}
void luna_process_channel_close(struct luna_process_channel *c)
{
   if(!c)return;
   close(c->fd);packets_free(c->head);packets_free(c->ready_head);free(c->incoming);
   pthread_mutex_destroy(&c->lock);free(c);
}
static bool env_key(const char *entry,const char *name)
{ size_t n=strlen(name);return !strncmp(entry,name,n) && entry[n]=='='; }
static char **child_environment(const char *name,uint32_t pid,uint32_t parent,uint32_t uid,bool host)
{
   size_t n=0;while(environ[n])++n;
   char **result=calloc(n+5,sizeof *result);if(!result)return NULL;
   size_t at=0;
   for(size_t i=0;i<n;++i){
      if(host && (env_key(environ[i],"LUNARIA_LAUNCH_ACTIVITY") || env_key(environ[i],"LUNARIA_TOUCH_TEST") ||
                  env_key(environ[i],"LUNARIA_TOUCH_FIFO") || env_key(environ[i],"LUNARIA_PLAY")))continue;
      if(env_key(environ[i],"ANDROID_PROCESS_NAME") || env_key(environ[i],"LUNARIA_GUEST_TID_BASE") ||
         env_key(environ[i],"LUNARIA_GUEST_PPID") || env_key(environ[i],"LUNARIA_GUEST_UID"))continue;
      result[at]=strdup(environ[i]);if(!result[at++])goto fail;
   }
   size_t len=strlen(name)+sizeof "ANDROID_PROCESS_NAME=";
   result[at]=malloc(len);if(!result[at])goto fail;
   snprintf(result[at++],len,"ANDROID_PROCESS_NAME=%s",name);
   char text[64];snprintf(text,sizeof text,"LUNARIA_GUEST_TID_BASE=%u",pid);
   result[at]=strdup(text);if(!result[at++])goto fail;
   snprintf(text,sizeof text,"LUNARIA_GUEST_PPID=%u",parent);
   result[at]=strdup(text);if(!result[at++])goto fail;
   if(uid){
      snprintf(text,sizeof text,"LUNARIA_GUEST_UID=%u",uid);
      result[at]=strdup(text);if(!result[at++])goto fail;
   }
   return result;
fail:
   for(size_t i=0;i<at;++i)free(result[i]);
   free(result);return NULL;
}
static void environment_free(char **env)
{ if(env){for(size_t i=0;env[i];++i)free(env[i]);free(env);} }
static int spawn_child(struct luna_process_pool *pool,const char *name,struct child **out)
{
   if(pool->next_pid>PID_LIMIT-PID_STRIDE)return -EAGAIN;
   struct child *child=calloc(1,sizeof *child);if(!child)return -ENOMEM;
   child->name=strdup(name);if(!child->name){free(child);return -ENOMEM;}
   int fds[2];
   if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,fds)<0){int e=errno;free(child->name);free(child);return -e;}
   /* dup2's source must survive replacement of the protocol's fixed fd 3. */
   int source=fcntl(fds[1],F_DUPFD_CLOEXEC,4);
   if(source<0){int e=errno;close(fds[0]);close(fds[1]);free(child->name);free(child);return -e;}
   posix_spawn_file_actions_t actions;int rc=posix_spawn_file_actions_init(&actions);
   bool initialized=rc==0;
   if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,source,3);
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2,34)
#define LUNA_HAVE_SPAWN_CLOSEFROM 1
#endif
#endif
#ifdef LUNA_HAVE_SPAWN_CLOSEFROM
   if(!rc)rc=posix_spawn_file_actions_addclosefrom_np(&actions,4);
#else
   /* This path must not leak the live VM's files/sockets into another VM. */
   if(!rc)rc=ENOTSUP;
#endif
   char **env=child_environment(name,pool->next_pid,pool->parent_pid,pool->guest_uid,pool->host_apk!=NULL);
   if(!env && !rc)rc=ENOMEM;
   const char *owner=getenv("ANDROID_PACKAGE_NAME"),*profile="default";
   /* A peer system package owns its own app data, rather than another game's
    * account profile. This package's own secondary processes still share it. */
   size_t owner_length=owner ? strlen(owner) : 0;
   if(owner_length && !strncmp(name,owner,owner_length) && (!name[owner_length] || name[owner_length]==':')){
      const char *current=getenv("LUNARIA_PROFILE");if(current && *current)profile=current;
   } else if(!strcmp(name,"com.google.android.gms") || !strcmp(name,"com.android.vending") || !strcmp(name,"com.google.android.gsf"))
      profile=luna_google_profile_current();
   char *module_args[]={pool->executable,pool->mode,"3",NULL};
   char *host_args[]={pool->executable,"--service-host","--profile",(char *)profile,pool->host_apk,NULL};
   char **args=pool->host_apk ? host_args : module_args;
   if(!rc)rc=posix_spawn(&child->host_pid,pool->executable,&actions,NULL,args,env);
   if(initialized)posix_spawn_file_actions_destroy(&actions);
   environment_free(env);close(source);close(fds[1]);
   if(rc){close(fds[0]);free(child->name);free(child);return -rc;}
   child->guest_pid=pool->next_pid;pool->next_pid+=PID_STRIDE;
   child->channel=luna_process_channel_open(fds[0]);
   if(!child->channel){
      int e=errno;kill(child->host_pid,SIGKILL);while(waitpid(child->host_pid,NULL,0)<0 && errno==EINTR){}
      free(child->name);free(child);return -e;
   }
   child->next=pool->children;pool->children=child;*out=child;return 0;
}
struct luna_process_pool *luna_process_pool_open(const char *exe,const char *mode,uint32_t parent,uint32_t first)
{
   if(!exe || !*exe || !mode || !*mode || !parent || parent>PID_LIMIT-PID_STRIDE || first<parent+PID_STRIDE || first>PID_LIMIT-PID_STRIDE){errno=EINVAL;return NULL;}
   struct luna_process_pool *pool=calloc(1,sizeof *pool);if(!pool)return NULL;
   pool->executable=strdup(exe);pool->mode=strdup(mode);pool->parent_pid=parent;pool->next_pid=first;
   int rc=pthread_mutex_init(&pool->lock,NULL);
   if(!pool->executable || !pool->mode || rc){if(!rc)pthread_mutex_destroy(&pool->lock);free(pool->executable);free(pool->mode);free(pool);errno=rc ? rc : ENOMEM;return NULL;}
   return pool;
}
struct luna_process_pool *luna_process_pool_open_host(const char *exe,const char *apk,uint32_t uid,uint32_t parent,uint32_t first)
{
   if(!apk || !*apk){errno=EINVAL;return NULL;}
   struct luna_process_pool *pool=luna_process_pool_open(exe,"--service-host",parent,first);
   if(!pool)return NULL;
   pool->guest_uid=uid;
   pool->host_apk=strdup(apk);
   if(!pool->host_apk){int e=errno;luna_process_pool_close(pool);errno=e;return NULL;}
   return pool;
}
static void child_poll(struct child *child)
{
   if(child->dead)return;
   int status=0;pid_t result;
   do{result=waitpid(child->host_pid,&status,WNOHANG);}while(result<0 && errno==EINTR);
   if(result==child->host_pid || (result<0 && errno==ECHILD)){
      child->dead=true;child->status=result>0 ? status : -ECHILD;
   }
}
static void reap(struct luna_process_pool *pool)
{
   struct child **link=&pool->children;
   while(*link){
      struct child *child=*link;child_poll(child);
      /* An exited VM can still have final replies in its socket. Keep its
       * old incarnation until those replies and EOF have been consumed. */
      if(child->dead && child->drained){
         struct exit_notice *notice=malloc(sizeof *notice);
         if(notice){
            notice->next=NULL;notice->name=child->name;child->name=NULL;
            notice->pid=child->guest_pid;notice->status=child->status;
            if(pool->exits_tail)pool->exits_tail->next=notice;else pool->exits=notice;
            pool->exits_tail=notice;
         }else pool->error=ENOMEM;
         *link=child->next;luna_process_channel_close(child->channel);free(child->name);free(child);continue;
      }
      link=&child->next;
   }
}
int luna_process_pool_send(struct luna_process_pool *pool,const char *name,const void *data,size_t size,uint32_t *pid)
{
   if(!pool || !name || !*name || strlen(name)>NAME_MAX_BYTES || (!data && size))return -EINVAL;
   if(size>LUNA_PROCESS_MESSAGE_MAX)return -EMSGSIZE;
   pthread_mutex_lock(&pool->lock);
   struct child *child=pool->children;
   while(child){child_poll(child);if(!child->dead && !strcmp(child->name,name))break;child=child->next;}
   int rc=child ? 0 : spawn_child(pool,name,&child);
   if(!rc){rc=luna_process_channel_send(child->channel,data,size);if(!rc && pid)*pid=child->guest_pid;}
   pthread_mutex_unlock(&pool->lock);return rc;
}
int luna_process_pool_send_to_host(struct luna_process_pool *pool,uint32_t host_pid,const void *data,size_t size)
{
   if(!pool || (!data && size))return -EINVAL;
   if(size>LUNA_PROCESS_MESSAGE_MAX)return -EMSGSIZE;
   pthread_mutex_lock(&pool->lock);
   int rc=-ESRCH;
   for(struct child *child=pool->children;child;child=child->next){
      if((uint32_t)child->host_pid!=host_pid)continue;
      child_poll(child);
      if(!child->dead)rc=luna_process_channel_send(child->channel,data,size);
      break;
   }
   pthread_mutex_unlock(&pool->lock);
   return rc;
}
int luna_process_pool_running(struct luna_process_pool *pool,const char *name)
{
   if(!pool || !name)return 0;
   pthread_mutex_lock(&pool->lock);
   int running=0;
   for(struct child *child=pool->children;child && !running;child=child->next){
      child_poll(child);running=!child->dead && !strcmp(child->name,name);
   }
   pthread_mutex_unlock(&pool->lock);return running;
}
struct reply { struct reply *next; char *name; uint32_t pid; size_t size; unsigned char data[]; };
struct replies { struct reply *head,*tail; struct child *child; int error; };
static void collect_reply(void *user,const void *data,size_t size)
{
   struct replies *r=user;struct reply *p=malloc(sizeof *p+size);
   if(!p){r->error=ENOMEM;return;}
   p->name=strdup(r->child->name);if(!p->name){free(p);r->error=ENOMEM;return;}
   p->next=NULL;p->pid=r->child->guest_pid;p->size=size;if(size)memcpy(p->data,data,size);
   if(r->tail)r->tail->next=p;else r->head=p;r->tail=p;
}
int luna_process_pool_tick(struct luna_process_pool *pool,luna_process_reply_fn callback,void *user)
{
   if(!pool)return -EINVAL;
   struct replies replies={0};int count=0,error=0;
   pthread_mutex_lock(&pool->lock);
   for(struct child *child=pool->children;child;child=child->next){
      replies.child=child;int n=luna_process_channel_tick(child->channel,collect_reply,&replies);
      if(n>0)count+=n;
      if(n<0){child->drained=true;if(!error)error=-n;}
   }
   reap(pool);if(pool->error && !error)error=pool->error;pool->error=0;
   /* The pool also batches channel callbacks; expose its pending replies to
    * a nested pool tick before invoking any potentially blocking callback. */
   if(replies.head){
      if(pool->ready_tail)pool->ready_tail->next=replies.head;else pool->ready_head=replies.head;
      pool->ready_tail=replies.tail;
   }
   pthread_mutex_unlock(&pool->lock);
   for(;;){
      pthread_mutex_lock(&pool->lock);
      struct reply *p=pool->ready_head;
      if(p){pool->ready_head=p->next;if(!pool->ready_head)pool->ready_tail=NULL;}
      pthread_mutex_unlock(&pool->lock);
      if(!p)break;
      if(callback)callback(user,p->name,p->pid,p->data,p->size);
      free(p->name);free(p);
   }
   return replies.error ? -replies.error : error ? -error : count;
}
int luna_process_pool_next_exit(struct luna_process_pool *pool,char *name,size_t capacity,uint32_t *pid,int *status)
{
   if(!pool || !name || !capacity)return -EINVAL;
   pthread_mutex_lock(&pool->lock);struct exit_notice *n=pool->exits;int rc=0;
   if(n){
      if(strlen(n->name)>=capacity)rc=-ENOSPC;
      else{
         memcpy(name,n->name,strlen(n->name)+1);if(pid)*pid=n->pid;if(status)*status=n->status;
         pool->exits=n->next;if(!pool->exits)pool->exits_tail=NULL;free(n->name);free(n);rc=1;
      }
   }
   pthread_mutex_unlock(&pool->lock);return rc;
}
void luna_process_pool_close(struct luna_process_pool *pool)
{
   if(!pool)return;
   while(pool->ready_head){
      struct reply *p=pool->ready_head;pool->ready_head=p->next;free(p->name);free(p);
   }
   for(struct child *c=pool->children;c;c=c->next){luna_process_channel_close(c->channel);c->channel=NULL;c->drained=true;}
   struct timespec delay={0,10000000};
   for(unsigned i=0;i<20 && pool->children;++i){reap(pool);if(pool->children)nanosleep(&delay,NULL);}
   for(struct child *c=pool->children;c;c=c->next)kill(c->host_pid,SIGTERM);
   for(unsigned i=0;i<20 && pool->children;++i){reap(pool);if(pool->children)nanosleep(&delay,NULL);}
   while(pool->children){
      struct child *c=pool->children;pool->children=c->next;
      kill(c->host_pid,SIGKILL);while(waitpid(c->host_pid,NULL,0)<0 && errno==EINTR){}
      free(c->name);free(c);
   }
   while(pool->exits){struct exit_notice *n=pool->exits;pool->exits=n->next;free(n->name);free(n);}
   pthread_mutex_destroy(&pool->lock);free(pool->host_apk);free(pool->mode);free(pool->executable);free(pool);
}

#elif defined(_WIN32)
/* Windows: the same protocol and pool as Linux, over a local stream socket.
 *
 * Windows has no socketpair() and no fd inheritance by number, so the parent
 * listens on its own AF_UNIX socket (127.0.0.1 TCP where AF_UNIX is missing)
 * and the child connects back: LUNARIA_IPC_PATH or LUNARIA_IPC_PORT say
 * where, LUNARIA_IPC_TOKEN proves it is the child this parent started.  The
 * child still gets "3" on its command line and calls
 * luna_process_channel_open(3), which does the connecting.  The parent
 * accepts lazily from channel_tick/flush, so a slow child (Play services
 * unpacking on its first start) never blocks the game; messages queue until
 * it arrives.  Before this, every bindService into Google Play services
 * failed with "Function not implemented" (Aloa, 14-11). */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <wchar.h>
/* stdlib.h only declares it under _CRT_RAND_S, and lunaria_os.h got there first. */
errno_t __cdecl rand_s(unsigned int *);
#define QUEUE_MAX (8u * LUNA_PROCESS_MESSAGE_MAX)
#define PID_STRIDE 4096u
#define PID_LIMIT 4194304u
#define NAME_MAX_BYTES 511u
#define TOKEN_CHARS 32
struct packet { struct packet *next; size_t size,offset; unsigned char bytes[]; };
struct luna_process_channel {
   pthread_mutex_t lock;
   SOCKET fd,listener;
   HANDLE process;            /* parent side: the child, to notice it died before connecting */
   char token[TOKEN_CHARS+1];
   wchar_t path[MAX_PATH];    /* AF_UNIX socket file to remove on close */
   int error;
   struct packet *head,*tail;
   size_t queued;
   unsigned char header[8];
   size_t header_size,received;
   struct packet *incoming;
   struct packet *ready_head,*ready_tail;
};
struct child {
   struct child *next;
   char *name;
   DWORD host_pid;
   HANDLE process;
   uint32_t guest_pid;
   struct luna_process_channel *channel;
   bool dead,drained;
   int status;
};
struct exit_notice { struct exit_notice *next; char *name; uint32_t pid; int status; };
struct luna_process_pool {
   pthread_mutex_t lock;
   char *executable,*mode,*host_apk;
   uint32_t parent_pid,next_pid,guest_uid;
   struct child *children;
   struct exit_notice *exits,*exits_tail;
   struct reply *ready_head,*ready_tail;
   int error;
};
static void put32(unsigned char *p,uint32_t v)
{ for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(i*8)); }
static uint32_t get32(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void packets_free(struct packet *p)
{ while(p){struct packet *next=p->next;free(p);p=next;} }
static void ipc_wsa(void)
{
   static int started;
   if(!started){WSADATA d;if(!WSAStartup(MAKEWORD(2,2),&d))started=1;}
}
static int ipc_nonblock(SOCKET s,int on)
{ u_long v=(u_long)on;return ioctlsocket(s,FIONBIO,&v)==0 ? 0 : -1; }
static int wsa_errno(void)
{
   int e=WSAGetLastError();
   return e==WSAEWOULDBLOCK ? EAGAIN : e==WSAEINTR ? EINTR : e==WSAEMSGSIZE ? EMSGSIZE : EPIPE;
}
static struct luna_process_channel *channel_new(SOCKET fd)
{
   struct luna_process_channel *c=calloc(1,sizeof *c);
   if(!c)return NULL;
   int e=pthread_mutex_init(&c->lock,NULL);
   if(e){free(c);errno=e;return NULL;}
   c->fd=fd;c->listener=INVALID_SOCKET;
   return c;
}
/* Parent side: take the child's connection once it is there.  0 = connected
 * (now or before), -EAGAIN = not yet, other negative = gone. */
static int channel_accept(struct luna_process_channel *c)
{
   if(c->fd!=INVALID_SOCKET)return 0;
   if(c->listener==INVALID_SOCKET)return -EPIPE;
   SOCKET s=accept(c->listener,NULL,NULL);
   if(s==INVALID_SOCKET){
      int e=wsa_errno();
      if(e!=EAGAIN)return -e;
      if(c->process && WaitForSingleObject(c->process,0)==WAIT_OBJECT_0)return -EPIPE;
      return -EAGAIN;
   }
   /* The child writes its token straight after connect(); wait briefly for it. */
   ipc_nonblock(s,0);
   DWORD timeout=5000;
   setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char *)&timeout,sizeof timeout);
   char got[TOKEN_CHARS];size_t have=0;
   while(have<TOKEN_CHARS){
      int n=recv(s,got+have,(int)(TOKEN_CHARS-have),0);
      if(n<=0)break;
      have+=(size_t)n;
   }
   if(have!=TOKEN_CHARS || memcmp(got,c->token,TOKEN_CHARS)){
      fprintf(stderr,"[service-broker] refused a connection without this child's token\n");
      closesocket(s);return -EAGAIN;
   }
   timeout=0;
   setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char *)&timeout,sizeof timeout);
   if(ipc_nonblock(s,1)){closesocket(s);return -EPIPE;}
   c->fd=s;
   closesocket(c->listener);c->listener=INVALID_SOCKET;
   if(c->path[0]){DeleteFileW(c->path);c->path[0]=0;}
   return 0;
}
/* Child side: fd 3 means "the parent's socket named in the environment". */
struct luna_process_channel *luna_process_channel_open(int fd)
{
   if(fd!=3){errno=EBADF;return NULL;}
   ipc_wsa();
   const char *path=getenv("LUNARIA_IPC_PATH"),*port=getenv("LUNARIA_IPC_PORT"),*token=getenv("LUNARIA_IPC_TOKEN");
   if(!token || strlen(token)!=TOKEN_CHARS || ((!path || !*path) && (!port || !*port))){errno=EBADF;return NULL;}
   SOCKET s=INVALID_SOCKET;
   if(path && *path){
      struct sockaddr_un a;memset(&a,0,sizeof a);a.sun_family=AF_UNIX;
      if(strlen(path)>=sizeof a.sun_path){errno=ENAMETOOLONG;return NULL;}
      memcpy(a.sun_path,path,strlen(path)+1);
      s=socket(AF_UNIX,SOCK_STREAM,0);
      if(s!=INVALID_SOCKET && connect(s,(struct sockaddr *)&a,sizeof a)){closesocket(s);s=INVALID_SOCKET;}
   } else {
      struct sockaddr_in a;memset(&a,0,sizeof a);a.sin_family=AF_INET;
      a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons((u_short)atoi(port));
      s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
      if(s!=INVALID_SOCKET && connect(s,(struct sockaddr *)&a,sizeof a)){closesocket(s);s=INVALID_SOCKET;}
   }
   if(s==INVALID_SOCKET){errno=ECONNREFUSED;return NULL;}
   size_t sent=0;
   while(sent<TOKEN_CHARS){
      int n=send(s,token+sent,(int)(TOKEN_CHARS-sent),0);
      if(n<=0){closesocket(s);errno=EPIPE;return NULL;}
      sent+=(size_t)n;
   }
   /* Not for this process's own children. */
   (void)luna_os_setenv("LUNARIA_IPC_PATH","",1);
   (void)luna_os_setenv("LUNARIA_IPC_PORT","",1);
   (void)luna_os_setenv("LUNARIA_IPC_TOKEN","",1);
   if(ipc_nonblock(s,1)){closesocket(s);errno=EIO;return NULL;}
   struct luna_process_channel *c=channel_new(s);
   if(!c)closesocket(s);
   return c;
}
static int flush(struct luna_process_channel *c)
{
   int a=channel_accept(c);
   if(a==-EAGAIN)return 0;
   if(a)return a;
   while(c->head){
      struct packet *p=c->head;
      int n=send(c->fd,(const char *)p->bytes+p->offset,(int)(p->size-p->offset),0);
      if(n<0){int e=wsa_errno();if(e==EINTR)continue;if(e==EAGAIN)return 0;return -e;}
      if(n==0)return -EPIPE;
      p->offset+=(size_t)n;c->queued-=(size_t)n;
      if(p->offset==p->size){c->head=p->next;if(!c->head)c->tail=NULL;free(p);}
   }
   return 0;
}
int luna_process_channel_send(struct luna_process_channel *c,const void *data,size_t size)
{
   if(!c || (!data && size))return -EINVAL;
   if(size>LUNA_PROCESS_MESSAGE_MAX)return -EMSGSIZE;
   pthread_mutex_lock(&c->lock);
   int rc=c->error ? -c->error : 0;
   if(!rc && c->queued+size+8>QUEUE_MAX)rc=-ENOBUFS;
   struct packet *p=rc ? NULL : malloc(sizeof *p+size+8);
   if(!rc && !p)rc=-ENOMEM;
   if(!rc){
      p->next=NULL;p->offset=0;p->size=size+8;
      memcpy(p->bytes,"LSP1",4);put32(p->bytes+4,(uint32_t)size);
      if(size)memcpy(p->bytes+8,data,size);
      if(c->tail)c->tail->next=p;else c->head=p;c->tail=p;c->queued+=p->size;
      rc=flush(c);if(rc)c->error=-rc;
   }
   pthread_mutex_unlock(&c->lock);return rc;
}
int luna_process_channel_tick(struct luna_process_channel *c,luna_process_message_fn callback,void *user)
{
   if(!c)return -EINVAL;
   struct packet *head=NULL,*tail=NULL;int count=0;
   pthread_mutex_lock(&c->lock);
   int rc=c->error ? -c->error : flush(c);
   bool connected=c->fd!=INVALID_SOCKET;
   while(!rc && connected && count<32){
      unsigned char *to;size_t left;
      if(c->header_size<8){to=c->header+c->header_size;left=8-c->header_size;}
      else{
         if(!c->incoming){
            uint32_t size=get32(c->header+4);
            if(memcmp(c->header,"LSP1",4) || size>LUNA_PROCESS_MESSAGE_MAX){rc=-EPROTO;break;}
            c->incoming=malloc(sizeof *c->incoming+size);
            if(!c->incoming){rc=-ENOMEM;break;}
            c->incoming->next=NULL;c->incoming->size=size;c->incoming->offset=0;c->received=0;
         }
         to=c->incoming->bytes+c->received;left=c->incoming->size-c->received;
         if(!left){
            if(tail)tail->next=c->incoming;else head=c->incoming;
            tail=c->incoming;c->incoming=NULL;c->header_size=0;++count;continue;
         }
      }
      int n=recv(c->fd,(char *)to,(int)left,0);
      if(n<0){int e=wsa_errno();if(e==EINTR)continue;if(e==EAGAIN)break;rc=-e;break;}
      if(n==0){rc=c->header_size || c->incoming ? -EPROTO : -EPIPE;break;}
      if(c->header_size<8)c->header_size+=(size_t)n;else c->received+=(size_t)n;
   }
   if(rc)c->error=-rc;
   if(head){
      if(c->ready_tail)c->ready_tail->next=head;else c->ready_head=head;
      c->ready_tail=tail;
   }
   pthread_mutex_unlock(&c->lock);
   for(unsigned delivered=0;delivered<32;++delivered){
      pthread_mutex_lock(&c->lock);
      struct packet *p=c->ready_head;
      if(p){c->ready_head=p->next;if(!c->ready_head)c->ready_tail=NULL;}
      pthread_mutex_unlock(&c->lock);
      if(!p)break;
      if(callback)callback(user,p->bytes,p->size);
      free(p);
   }
   return rc ? rc : count;
}
void luna_process_channel_close(struct luna_process_channel *c)
{
   if(!c)return;
   if(c->fd!=INVALID_SOCKET)closesocket(c->fd);
   if(c->listener!=INVALID_SOCKET)closesocket(c->listener);
   if(c->path[0])DeleteFileW(c->path);
   packets_free(c->head);packets_free(c->ready_head);free(c->incoming);
   pthread_mutex_destroy(&c->lock);free(c);
}
/* Parent side: a listening channel the child will connect to. */
static struct luna_process_channel *channel_listen(char *path_out,size_t path_cap,int *port_out)
{
   ipc_wsa();
   struct luna_process_channel *c=channel_new(INVALID_SOCKET);
   if(!c)return NULL;
   static volatile LONG serial;
   unsigned char raw[TOKEN_CHARS/2];
   for(size_t i=0;i<sizeof raw;i+=sizeof(unsigned)){
      unsigned v=0;
      if(rand_s(&v)){luna_process_channel_close(c);errno=EIO;return NULL;}
      memcpy(raw+i,&v,sizeof raw-i<sizeof v ? sizeof raw-i : sizeof v);
   }
   for(size_t i=0;i<sizeof raw;++i)snprintf(c->token+2*i,3,"%02x",raw[i]);
   path_out[0]=0;*port_out=0;
   /* AF_UNIX: a socket file in %TEMP%, no network stack and no firewall. */
   wchar_t dir[MAX_PATH];
   DWORD n=GetTempPathW(MAX_PATH,dir);
   SOCKET s=socket(AF_UNIX,SOCK_STREAM,0);
   if(s!=INVALID_SOCKET && n && n<MAX_PATH-48){
      swprintf(c->path,MAX_PATH,L"%lslunaria-ipc-%lu-%ld.sock",dir,(unsigned long)GetCurrentProcessId(),
               (long)InterlockedIncrement(&serial));
      DeleteFileW(c->path);
      struct sockaddr_un a;memset(&a,0,sizeof a);a.sun_family=AF_UNIX;
      int len=WideCharToMultiByte(CP_UTF8,0,c->path,-1,a.sun_path,(int)sizeof a.sun_path,NULL,NULL);
      if(len>0 && (size_t)len<=path_cap && !bind(s,(struct sockaddr *)&a,sizeof a) && !listen(s,1)){
         memcpy(path_out,a.sun_path,(size_t)len);
      } else {
         closesocket(s);s=INVALID_SOCKET;DeleteFileW(c->path);
      }
   } else if(s!=INVALID_SOCKET){closesocket(s);s=INVALID_SOCKET;}
   if(s==INVALID_SOCKET){
      c->path[0]=0;
      struct sockaddr_in a;memset(&a,0,sizeof a);a.sin_family=AF_INET;
      a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;
      int len=sizeof a;
      s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
      if(s==INVALID_SOCKET || bind(s,(struct sockaddr *)&a,sizeof a) || listen(s,1) ||
         getsockname(s,(struct sockaddr *)&a,&len)){
         if(s!=INVALID_SOCKET)closesocket(s);
         luna_process_channel_close(c);errno=EADDRNOTAVAIL;return NULL;
      }
      *port_out=ntohs(a.sin_port);
   }
   if(ipc_nonblock(s,1)){closesocket(s);luna_process_channel_close(c);errno=EIO;return NULL;}
   c->listener=s;
   return c;
}
/* Wide environment block for the child: ours minus the per-process keys,
 * plus the child's own.  Built from _wenviron so non-ASCII values survive. */
static bool wenv_key(const wchar_t *entry,const wchar_t *name)
{ size_t n=wcslen(name);return !_wcsnicmp(entry,name,n) && entry[n]==L'='; }
static int wenv_add(wchar_t **block,size_t *used,size_t *cap,const wchar_t *entry)
{
   size_t n=wcslen(entry)+1;
   if(*used+n+1>*cap){
      size_t nc=(*cap ? *cap*2 : 65536)+n;
      wchar_t *p=realloc(*block,nc*sizeof *p);
      if(!p)return -1;
      *block=p;*cap=nc;
   }
   memcpy(*block+*used,entry,n*sizeof(wchar_t));*used+=n;
   return 0;
}
static int wenv_add_utf8(wchar_t **block,size_t *used,size_t *cap,const char *key,const char *value)
{
   char *text=malloc(strlen(key)+strlen(value)+2);
   if(!text)return -1;
   sprintf(text,"%s=%s",key,value);
   int n=MultiByteToWideChar(CP_UTF8,0,text,-1,NULL,0);
   wchar_t *w=n>0 ? malloc((size_t)n*sizeof *w) : NULL;
   int rc=-1;
   if(w && MultiByteToWideChar(CP_UTF8,0,text,-1,w,n)>0)rc=wenv_add(block,used,cap,w);
   free(w);free(text);return rc;
}
static wchar_t *child_environment(const char *name,uint32_t pid,uint32_t parent,uint32_t uid,bool host,
                                  const char *path,int port,const char *token)
{
   static const wchar_t *const drop_always[]={L"ANDROID_PROCESS_NAME",L"LUNARIA_GUEST_TID_BASE",L"LUNARIA_GUEST_PPID",
      L"LUNARIA_GUEST_UID",L"LUNARIA_IPC_PATH",L"LUNARIA_IPC_PORT",L"LUNARIA_IPC_TOKEN",L"LUNARIA_NO_LOG_FILE"};
   static const wchar_t *const drop_host[]={L"LUNARIA_LAUNCH_ACTIVITY",L"LUNARIA_TOUCH_TEST",L"LUNARIA_TOUCH_FIFO",L"LUNARIA_PLAY"};
   wchar_t *block=NULL;size_t used=0,cap=0;
   for(wchar_t **e=_wenviron;e && *e;++e){
      bool skip=(*e)[0]==L'=';
      for(size_t i=0;!skip && i<sizeof drop_always/sizeof *drop_always;++i)skip=wenv_key(*e,drop_always[i]);
      for(size_t i=0;!skip && host && i<sizeof drop_host/sizeof *drop_host;++i)skip=wenv_key(*e,drop_host[i]);
      if(!skip && wenv_add(&block,&used,&cap,*e))goto fail;
   }
   char text[32];
   if(wenv_add_utf8(&block,&used,&cap,"ANDROID_PROCESS_NAME",name))goto fail;
   snprintf(text,sizeof text,"%u",pid);
   if(wenv_add_utf8(&block,&used,&cap,"LUNARIA_GUEST_TID_BASE",text))goto fail;
   snprintf(text,sizeof text,"%u",parent);
   if(wenv_add_utf8(&block,&used,&cap,"LUNARIA_GUEST_PPID",text))goto fail;
   if(uid){
      snprintf(text,sizeof text,"%u",uid);
      if(wenv_add_utf8(&block,&used,&cap,"LUNARIA_GUEST_UID",text))goto fail;
   }
   /* Its stderr is ours: its log lines land in our lunaria-last-run.log, and
    * it must not truncate that file with a log of its own. */
   if(wenv_add_utf8(&block,&used,&cap,"LUNARIA_NO_LOG_FILE","1"))goto fail;
   if(path && *path){if(wenv_add_utf8(&block,&used,&cap,"LUNARIA_IPC_PATH",path))goto fail;}
   else{
      snprintf(text,sizeof text,"%d",port);
      if(wenv_add_utf8(&block,&used,&cap,"LUNARIA_IPC_PORT",text))goto fail;
   }
   if(wenv_add_utf8(&block,&used,&cap,"LUNARIA_IPC_TOKEN",token))goto fail;
   if(wenv_add(&block,&used,&cap,L""))goto fail;
   return block;
fail:
   free(block);return NULL;
}
/* Command line quoting, as CommandLineToArgvW reads it back. */
static int cmd_add(wchar_t **cmd,size_t *used,size_t *cap,const char *arg)
{
   int n=MultiByteToWideChar(CP_UTF8,0,arg,-1,NULL,0);
   if(n<=0)return -1;
   size_t need=*used+(size_t)n*2+4;
   if(need>*cap){
      size_t nc=need*2;wchar_t *p=realloc(*cmd,nc*sizeof *p);
      if(!p)return -1;
      *cmd=p;*cap=nc;
   }
   wchar_t *w=malloc((size_t)n*sizeof *w);
   if(!w)return -1;
   MultiByteToWideChar(CP_UTF8,0,arg,-1,w,n);
   wchar_t *o=*cmd+*used;
   if(*used)*o++=L' ';
   *o++=L'"';
   size_t slashes=0;
   for(const wchar_t *p=w;*p;++p){
      if(*p==L'\\'){++slashes;*o++=*p;continue;}
      if(*p==L'"'){for(size_t i=0;i<slashes+1;++i)*o++=L'\\';}
      slashes=0;*o++=*p;
   }
   for(size_t i=0;i<slashes;++i)*o++=L'\\';
   *o++=L'"';*o=0;
   *used=(size_t)(o-*cmd);
   free(w);return 0;
}
static int spawn_child(struct luna_process_pool *pool,const char *name,struct child **out)
{
   if(pool->next_pid>PID_LIMIT-PID_STRIDE)return -EAGAIN;
   struct child *child=calloc(1,sizeof *child);if(!child)return -ENOMEM;
   child->name=strdup(name);if(!child->name){free(child);return -ENOMEM;}
   char path[108];int port=0;
   struct luna_process_channel *channel=channel_listen(path,sizeof path,&port);
   if(!channel){int e=errno;free(child->name);free(child);return -e;}
   const char *owner=getenv("ANDROID_PACKAGE_NAME"),*profile="default";
   size_t owner_length=owner ? strlen(owner) : 0;
   if(owner_length && !strncmp(name,owner,owner_length) && (!name[owner_length] || name[owner_length]==':')){
      const char *current=getenv("LUNARIA_PROFILE");if(current && *current)profile=current;
   } else if(!strcmp(name,"com.google.android.gms") || !strcmp(name,"com.android.vending") || !strcmp(name,"com.google.android.gsf"))
      profile=luna_google_profile_current();
   const char *module_args[]={pool->executable,pool->mode,"3",NULL};
   const char *host_args[]={pool->executable,"--service-host","--profile",profile,pool->host_apk,NULL};
   const char *const *args=pool->host_apk ? host_args : module_args;
   wchar_t *cmd=NULL;size_t used=0,cap=0;int rc=0;
   for(size_t i=0;args[i] && !rc;++i)if(cmd_add(&cmd,&used,&cap,args[i]))rc=-ENOMEM;
   wchar_t *env=rc ? NULL : child_environment(name,pool->next_pid,pool->parent_pid,pool->guest_uid,
                                                pool->host_apk!=NULL,path,port,channel->token);
   if(!rc && !env)rc=-ENOMEM;
   int wn=rc ? 0 : MultiByteToWideChar(CP_UTF8,0,pool->executable,-1,NULL,0);
   wchar_t *exe=wn>0 ? malloc((size_t)wn*sizeof *exe) : NULL;
   if(!rc && (!exe || !MultiByteToWideChar(CP_UTF8,0,pool->executable,-1,exe,wn)))rc=-ENOMEM;
   /* Inherit stdout/stderr only, so the child's log reaches ours and no
    * other handle of this VM leaks into it. */
   HANDLE std_out=NULL,std_err=NULL,list[2];DWORD nlist=0;
   HANDLE self=GetCurrentProcess();
   HANDLE ho=GetStdHandle(STD_OUTPUT_HANDLE),he=GetStdHandle(STD_ERROR_HANDLE);
   if(!rc && ho && ho!=INVALID_HANDLE_VALUE && DuplicateHandle(self,ho,self,&std_out,0,TRUE,DUPLICATE_SAME_ACCESS))list[nlist++]=std_out;
   if(!rc && he && he!=INVALID_HANDLE_VALUE && DuplicateHandle(self,he,self,&std_err,0,TRUE,DUPLICATE_SAME_ACCESS))list[nlist++]=std_err;
   STARTUPINFOEXW si;memset(&si,0,sizeof si);si.StartupInfo.cb=sizeof si;
   SIZE_T attr_size=0;LPPROC_THREAD_ATTRIBUTE_LIST attrs=NULL;
   if(!rc && nlist){
      InitializeProcThreadAttributeList(NULL,1,0,&attr_size);
      attrs=malloc(attr_size);
      if(attrs && InitializeProcThreadAttributeList(attrs,1,0,&attr_size) &&
         UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,list,nlist*sizeof *list,NULL,NULL)){
         si.lpAttributeList=attrs;
         si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
         si.StartupInfo.hStdInput=NULL;
         si.StartupInfo.hStdOutput=std_out ? std_out : std_err;
         si.StartupInfo.hStdError=std_err ? std_err : std_out;
      } else {
         if(attrs){free(attrs);attrs=NULL;}
         nlist=0;
      }
   }
   PROCESS_INFORMATION pi;memset(&pi,0,sizeof pi);
   if(!rc && !CreateProcessW(exe,cmd,NULL,NULL,nlist ? TRUE : FALSE,
                             CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW|(attrs ? EXTENDED_STARTUPINFO_PRESENT : 0),
                             env,NULL,&si.StartupInfo,&pi)){
      fprintf(stderr,"[service-broker] cannot start %s: error %lu\n",name,(unsigned long)GetLastError());
      rc=-ECHILD;
   }
   if(attrs){DeleteProcThreadAttributeList(attrs);free(attrs);}
   if(std_out)CloseHandle(std_out);
   if(std_err)CloseHandle(std_err);
   free(exe);free(env);free(cmd);
   if(rc){luna_process_channel_close(channel);free(child->name);free(child);return rc;}
   CloseHandle(pi.hThread);
   child->process=pi.hProcess;child->host_pid=pi.dwProcessId;
   channel->process=pi.hProcess;
   child->channel=channel;
   child->guest_pid=pool->next_pid;pool->next_pid+=PID_STRIDE;
   fprintf(stderr,"[service-broker] started %s (pid %lu, %s)\n",name,(unsigned long)pi.dwProcessId,
           path[0] ? "unix socket" : "loopback");
   child->next=pool->children;pool->children=child;*out=child;return 0;
}
struct luna_process_pool *luna_process_pool_open(const char *exe,const char *mode,uint32_t parent,uint32_t first)
{
   if(!exe || !*exe || !mode || !*mode || !parent || parent>PID_LIMIT-PID_STRIDE || first<parent+PID_STRIDE || first>PID_LIMIT-PID_STRIDE){errno=EINVAL;return NULL;}
   struct luna_process_pool *pool=calloc(1,sizeof *pool);if(!pool)return NULL;
   pool->executable=strdup(exe);pool->mode=strdup(mode);pool->parent_pid=parent;pool->next_pid=first;
   int rc=pthread_mutex_init(&pool->lock,NULL);
   if(!pool->executable || !pool->mode || rc){if(!rc)pthread_mutex_destroy(&pool->lock);free(pool->executable);free(pool->mode);free(pool);errno=rc ? rc : ENOMEM;return NULL;}
   return pool;
}
struct luna_process_pool *luna_process_pool_open_host(const char *exe,const char *apk,uint32_t uid,uint32_t parent,uint32_t first)
{
   if(!apk || !*apk){errno=EINVAL;return NULL;}
   struct luna_process_pool *pool=luna_process_pool_open(exe,"--service-host",parent,first);
   if(!pool)return NULL;
   pool->guest_uid=uid;
   pool->host_apk=strdup(apk);
   if(!pool->host_apk){int e=errno;luna_process_pool_close(pool);errno=e;return NULL;}
   return pool;
}
static void child_poll(struct child *child)
{
   if(child->dead || !child->process)return;
   if(WaitForSingleObject(child->process,0)==WAIT_OBJECT_0){
      DWORD code=0;GetExitCodeProcess(child->process,&code);
      child->dead=true;child->status=(int)code;
   }
}
static void child_free(struct child *child)
{
   luna_process_channel_close(child->channel);
   if(child->process)CloseHandle(child->process);
   free(child->name);free(child);
}
static void reap(struct luna_process_pool *pool)
{
   struct child **link=&pool->children;
   while(*link){
      struct child *child=*link;child_poll(child);
      if(child->dead && child->drained){
         struct exit_notice *notice=malloc(sizeof *notice);
         if(notice){
            notice->next=NULL;notice->name=child->name;child->name=NULL;
            notice->pid=child->guest_pid;notice->status=child->status;
            if(pool->exits_tail)pool->exits_tail->next=notice;else pool->exits=notice;
            pool->exits_tail=notice;
         }else pool->error=ENOMEM;
         *link=child->next;child_free(child);continue;
      }
      link=&child->next;
   }
}
int luna_process_pool_send(struct luna_process_pool *pool,const char *name,const void *data,size_t size,uint32_t *pid)
{
   if(!pool || !name || !*name || strlen(name)>NAME_MAX_BYTES || (!data && size))return -EINVAL;
   if(size>LUNA_PROCESS_MESSAGE_MAX)return -EMSGSIZE;
   pthread_mutex_lock(&pool->lock);
   struct child *child=pool->children;
   while(child){child_poll(child);if(!child->dead && !strcmp(child->name,name))break;child=child->next;}
   int rc=child ? 0 : spawn_child(pool,name,&child);
   if(!rc){rc=luna_process_channel_send(child->channel,data,size);if(!rc && pid)*pid=child->guest_pid;}
   pthread_mutex_unlock(&pool->lock);return rc;
}
int luna_process_pool_send_to_host(struct luna_process_pool *pool,uint32_t host_pid,const void *data,size_t size)
{
   if(!pool || (!data && size))return -EINVAL;
   if(size>LUNA_PROCESS_MESSAGE_MAX)return -EMSGSIZE;
   pthread_mutex_lock(&pool->lock);
   int rc=-ESRCH;
   for(struct child *child=pool->children;child;child=child->next){
      if((uint32_t)child->host_pid!=host_pid)continue;
      child_poll(child);
      if(!child->dead)rc=luna_process_channel_send(child->channel,data,size);
      break;
   }
   pthread_mutex_unlock(&pool->lock);
   return rc;
}
int luna_process_pool_running(struct luna_process_pool *pool,const char *name)
{
   if(!pool || !name)return 0;
   pthread_mutex_lock(&pool->lock);
   int running=0;
   for(struct child *child=pool->children;child && !running;child=child->next){
      child_poll(child);running=!child->dead && !strcmp(child->name,name);
   }
   pthread_mutex_unlock(&pool->lock);return running;
}
struct reply { struct reply *next; char *name; uint32_t pid; size_t size; unsigned char data[]; };
struct replies { struct reply *head,*tail; struct child *child; int error; };
static void collect_reply(void *user,const void *data,size_t size)
{
   struct replies *r=user;struct reply *p=malloc(sizeof *p+size);
   if(!p){r->error=ENOMEM;return;}
   p->name=strdup(r->child->name);if(!p->name){free(p);r->error=ENOMEM;return;}
   p->next=NULL;p->pid=r->child->guest_pid;p->size=size;if(size)memcpy(p->data,data,size);
   if(r->tail)r->tail->next=p;else r->head=p;r->tail=p;
}
int luna_process_pool_tick(struct luna_process_pool *pool,luna_process_reply_fn callback,void *user)
{
   if(!pool)return -EINVAL;
   struct replies replies={0};int count=0,error=0;
   pthread_mutex_lock(&pool->lock);
   for(struct child *child=pool->children;child;child=child->next){
      replies.child=child;int n=luna_process_channel_tick(child->channel,collect_reply,&replies);
      if(n>0)count+=n;
      if(n<0){child->drained=true;if(!error)error=-n;}
   }
   reap(pool);if(pool->error && !error)error=pool->error;pool->error=0;
   if(replies.head){
      if(pool->ready_tail)pool->ready_tail->next=replies.head;else pool->ready_head=replies.head;
      pool->ready_tail=replies.tail;
   }
   pthread_mutex_unlock(&pool->lock);
   for(;;){
      pthread_mutex_lock(&pool->lock);
      struct reply *p=pool->ready_head;
      if(p){pool->ready_head=p->next;if(!pool->ready_head)pool->ready_tail=NULL;}
      pthread_mutex_unlock(&pool->lock);
      if(!p)break;
      if(callback)callback(user,p->name,p->pid,p->data,p->size);
      free(p->name);free(p);
   }
   return replies.error ? -replies.error : error ? -error : count;
}
int luna_process_pool_next_exit(struct luna_process_pool *pool,char *name,size_t capacity,uint32_t *pid,int *status)
{
   if(!pool || !name || !capacity)return -EINVAL;
   pthread_mutex_lock(&pool->lock);struct exit_notice *n=pool->exits;int rc=0;
   if(n){
      if(strlen(n->name)>=capacity)rc=-ENOSPC;
      else{
         memcpy(name,n->name,strlen(n->name)+1);if(pid)*pid=n->pid;if(status)*status=n->status;
         pool->exits=n->next;if(!pool->exits)pool->exits_tail=NULL;free(n->name);free(n);rc=1;
      }
   }
   pthread_mutex_unlock(&pool->lock);return rc;
}
void luna_process_pool_close(struct luna_process_pool *pool)
{
   if(!pool)return;
   while(pool->ready_head){
      struct reply *p=pool->ready_head;pool->ready_head=p->next;free(p->name);free(p);
   }
   /* Closing the socket is the child's signal to finish; give it a moment. */
   for(struct child *c=pool->children;c;c=c->next){luna_process_channel_close(c->channel);c->channel=NULL;c->drained=true;}
   for(unsigned i=0;i<40 && pool->children;++i){reap(pool);if(pool->children)Sleep(10);}
   while(pool->children){
      struct child *c=pool->children;pool->children=c->next;
      if(c->process){TerminateProcess(c->process,1);WaitForSingleObject(c->process,2000);}
      c->channel=NULL;child_free(c);
   }
   while(pool->exits){struct exit_notice *n=pool->exits;pool->exits=n->next;free(n->name);free(n);}
   pthread_mutex_destroy(&pool->lock);free(pool->host_apk);free(pool->mode);free(pool->executable);free(pool);
}

#else
/* No fork-only or shared-address-space substitute on unsupported hosts. */
#include <errno.h>
#ifdef _WIN32
#include <io.h>
#define ipc_close _close
#else
#include <unistd.h>
#define ipc_close close
#endif
struct luna_process_channel *luna_process_channel_open(int fd)
{ if(fd>=0)ipc_close(fd);errno=ENOSYS;return NULL; }
int luna_process_channel_send(struct luna_process_channel *c,const void *p,size_t n)
{ (void)c;(void)p;(void)n;return -ENOSYS; }
int luna_process_channel_tick(struct luna_process_channel *c,luna_process_message_fn f,void *u)
{ (void)c;(void)f;(void)u;return -ENOSYS; }
void luna_process_channel_close(struct luna_process_channel *c){(void)c;}
struct luna_process_pool *luna_process_pool_open_host(const char *e,const char *a,uint32_t u,uint32_t p,uint32_t f)
{ (void)e;(void)a;(void)u;(void)p;(void)f;errno=ENOSYS;return NULL; }
struct luna_process_pool *luna_process_pool_open(const char *e,const char *m,uint32_t p,uint32_t f)
{ (void)e;(void)m;(void)p;(void)f;errno=ENOSYS;return NULL; }
int luna_process_pool_running(struct luna_process_pool *p,const char *n)
{ (void)p;(void)n;return 0; }
int luna_process_pool_send_to_host(struct luna_process_pool *p,uint32_t h,const void *d,size_t s)
{ (void)p;(void)h;(void)d;(void)s;return -ENOSYS; }
int luna_process_pool_send(struct luna_process_pool *p,const char *n,const void *d,size_t s,uint32_t *g)
{ (void)p;(void)n;(void)d;(void)s;(void)g;return -ENOSYS; }
int luna_process_pool_tick(struct luna_process_pool *p,luna_process_reply_fn f,void *u)
{ (void)p;(void)f;(void)u;return -ENOSYS; }
int luna_process_pool_next_exit(struct luna_process_pool *p,char *n,size_t c,uint32_t *g,int *s)
{ (void)p;(void)n;(void)c;(void)g;(void)s;return -ENOSYS; }
void luna_process_pool_close(struct luna_process_pool *p){(void)p;}
#endif
