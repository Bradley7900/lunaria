/*
 * Copyright © 2026 Yuichiro Nakada / Project Lunaria
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See luna_compositor.h for what this is and why it exists.
 */

#include "luna_compositor.h"
#include "luna_boot.h"
#include "luna_ime.h"
#include "luna_overlay.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <EGL/eglext_angle.h>
#endif

#define SLOTS 3

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLConfig  g_cfg;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLSurface g_win = EGL_NO_SURFACE;
static EGLNativeWindowType g_native;

/* Windows: ANGLE gives every context on one EGLDisplay the same D3D11 device,
 * and its D3D11 state cache is per device, not per context.  With the
 * compositor drawing on its own thread while the guest draws on another,
 * the guest's draws went to whatever render target the compositor had bound
 * last: clears (which name their target) still landed, every draw vanished,
 * and Unity titles showed only their clear colour.  So on Windows the
 * compositor runs on a display of its own (a separate device) and frames
 * cross over as pixels, the path Vulkan frames already use.
 * LUNARIA_COMP_SHARED_DISPLAY=1 restores the shared display. */
static bool g_own_display;
static EGLDisplay g_guest_dpy = EGL_NO_DISPLAY;
/* Frames between the two devices without a CPU round trip: each slot is a
 * pbuffer on the guest's display whose D3D11 texture is opened on the
 * compositor's through its share handle (EGL_ANGLE_surface_d3d_texture_2d_
 * share_handle / EGL_ANGLE_d3d_share_handle_client_buffer).  Anything missing
 * or failing drops back to pixels.  LUNARIA_COMP_SHARE=0 forces pixels. */
static int   g_share_state;            /* 0 untried, 1 on, -1 off */
static void *g_slot_handle[SLOTS];     /* under g_mu */

static atomic_bool g_active;
static atomic_ullong g_frames;
static pthread_t g_thread;

/* Frame slots.  A slot is free, queued (the newest submitted frame, not yet
 * taken) or shown (what the compositor samples).
 *
 * A guest context lives in the share group its creator chose — Android's
 * rule — which is usually not the compositor's.  So a frame crosses over as
 * an EGLImage, which belongs to the display rather than to a share group: the
 * guest side keeps a texture per slot in its own group and an image of it,
 * and the compositor binds that image to a texture of its own.  The fences
 * are EGL syncs for the same reason. */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv_comp = PTHREAD_COND_INITIALIZER;   /* compositor waits */
static pthread_cond_t  g_cv_guest = PTHREAD_COND_INITIALIZER;  /* submit waits */
static EGLImage g_slot_img[SLOTS];     /* what the slot holds now */
static EGLSync  g_ready_fence[SLOTS];  /* the copy into the slot is done */
static EGLSync  g_release_fence[SLOTS];/* the compositor is done reading it */
static int      g_queued = -1, g_shown = -1;
static int      g_frame_w[SLOTS], g_frame_h[SLOTS];
static atomic_uint g_comp_frames;     /* distinct guest frames the compositor got */
static bool     g_wake;

/* Guest side: per guest share group (keyed by the presenting context), the
 * slot textures and their images. */
#define SOURCES 8
struct comp_source {
   EGLContext ctx;
   GLuint tex[SLOTS];
   EGLImage img[SLOTS];
   int w, h;
   unsigned long long used;
};
static struct comp_source g_src[SOURCES];
static unsigned long long g_src_clock;

/* Images a resize replaced, destroyed by the compositor once no slot of its
 * own still samples them. */
static EGLImage g_retired[SOURCES * SLOTS];
static int g_nretired;

static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;

/* Frames that arrive as pixels (a Vulkan swapchain read back to host memory)
 * rather than in a GL context.  One pending buffer, under g_mu; the
 * compositor uploads it to a texture of its own and shows that until the next
 * frame of either kind arrives. */
static uint8_t *g_px_buf;          /* pending frame, RGBA, bottom row first */
static size_t   g_px_cap;
static int      g_px_w, g_px_h;
static bool     g_px_pending;

static uint64_t now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- guest side ---------------------------------------------------------- */

/* The source for the current context, (re)allocated to w x h. */
static struct comp_source *source_for(EGLContext ctx, int w, int h)
{
   struct comp_source *src = NULL, *lru = &g_src[0];
   for (int i = 0; i < SOURCES; ++i) {
      if (g_src[i].ctx == ctx) { src = &g_src[i]; break; }
      if (g_src[i].used < lru->used) lru = &g_src[i];
   }
   if (!src) {
      /* A context not seen before takes the least recently used entry.  Its
       * textures belonged to another context and are simply forgotten; the
       * images are retired like a resize's. */
      src = lru;
      pthread_mutex_lock(&g_mu);
      for (int i = 0; i < SLOTS; ++i)
         if (src->img[i] && g_nretired < (int)(sizeof g_retired / sizeof g_retired[0]))
            g_retired[g_nretired++] = src->img[i];
      pthread_mutex_unlock(&g_mu);
      memset(src, 0, sizeof *src);
      src->ctx = ctx;
   }
   src->used = ++g_src_clock;
   if (src->w == w && src->h == h && src->img[0]) return src;

   pthread_mutex_lock(&g_mu);
   for (int i = 0; i < SLOTS; ++i)
      if (src->img[i] && g_nretired < (int)(sizeof g_retired / sizeof g_retired[0]))
         g_retired[g_nretired++] = src->img[i];
   pthread_mutex_unlock(&g_mu);
   for (int i = 0; i < SLOTS; ++i) {
      if (!src->tex[i]) glGenTextures(1, &src->tex[i]);
      glBindTexture(GL_TEXTURE_2D, src->tex[i]);
      /* The compositor samples only RGB.  Copying a default framebuffer to
       * RGBA8 is invalid for some EGL surface formats (ANGLE returns 0x502),
       * while RGB8 matches their colour data and keeps EGLImage sharing. */
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB,
                   GL_UNSIGNED_BYTE, NULL);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      const EGLAttrib attrs[] = { EGL_GL_TEXTURE_LEVEL, 0, EGL_NONE };
      src->img[i] = eglCreateImage(g_dpy, ctx, EGL_GL_TEXTURE_2D,
                                   (EGLClientBuffer)(uintptr_t)src->tex[i], attrs);
      if (src->img[i] == EGL_NO_IMAGE) {
         static int warned;
         if (warned++ < 4)
            fprintf(stderr, "[compositor] eglCreateImage failed (0x%x)\n",
                    eglGetError());
         return NULL;
      }
   }
   src->w = w;
   src->h = h;
   return src;
}

static void submit_pixels(int w, int h, const void *pixels, int stride,
                          bool bgra, int swap_interval, bool bottom_up);

#ifdef _WIN32
static struct {
   EGLContext ctx;
   int w, h;
   EGLSurface pb[SLOTS];
   void *handle[SLOTS];
   GLuint tex[SLOTS], fbo;
   int pending;      /* slot copied but not yet handed over, or -1 */
   EGLSync fence;    /* signals when that copy is done */
} g_gs;

static void share_off(const char *why)
{
   g_share_state = -1;
   fprintf(stderr, "[compositor] shared texture off (%s); frames go as pixels\n", why);
}

/* Guest thread, guest context current.  False: use the pixel path. */
static bool share_submit(EGLContext ctx, int w, int h, int swap_interval)
{
   static PFNEGLQUERYSURFACEPOINTERANGLEPROC query_ptr;
   if (g_share_state < 0) return false;
   if (g_share_state == 0) {
      const char *env = getenv("LUNARIA_COMP_SHARE");
      const char *e1 = eglQueryString(g_guest_dpy, EGL_EXTENSIONS);
      const char *e2 = eglQueryString(g_dpy, EGL_EXTENSIONS);
      query_ptr = (PFNEGLQUERYSURFACEPOINTERANGLEPROC)
         eglGetProcAddress("eglQuerySurfacePointerANGLE");
      if (env && env[0] == '0') { share_off("LUNARIA_COMP_SHARE=0"); return false; }
      if (!e1 || !strstr(e1, "EGL_ANGLE_surface_d3d_texture_2d_share_handle") ||
          !e2 || !strstr(e2, "EGL_ANGLE_d3d_share_handle_client_buffer") || !query_ptr) {
         share_off("ANGLE share-handle extensions missing");
         return false;
      }
      g_share_state = 1;
   }
   if (g_gs.ctx != ctx || g_gs.w != w || g_gs.h != h) {
      for (int i = 0; i < SLOTS; ++i) {
         if (g_gs.pb[i] != EGL_NO_SURFACE && g_gs.pb[i]) {
            if (g_gs.ctx == ctx) eglReleaseTexImage(g_guest_dpy, g_gs.pb[i], EGL_BACK_BUFFER);
            eglDestroySurface(g_guest_dpy, g_gs.pb[i]);
         }
         if (g_gs.ctx == ctx && g_gs.tex[i]) glDeleteTextures(1, &g_gs.tex[i]);
      }
      if (g_gs.ctx == ctx && g_gs.fbo) glDeleteFramebuffers(1, &g_gs.fbo);
      if (g_gs.fence != EGL_NO_SYNC) eglDestroySync(g_guest_dpy, g_gs.fence);
      memset(&g_gs, 0, sizeof g_gs);
      g_gs.pending = -1;
      const EGLint ca[] = {
         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
         EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_BIND_TO_TEXTURE_RGBA, EGL_TRUE,
         EGL_NONE
      };
      EGLConfig cfg;
      EGLint n = 0;
      if (!eglChooseConfig(g_guest_dpy, ca, &cfg, 1, &n) || n < 1) {
         share_off("no bindable pbuffer config");
         return false;
      }
      GLint prev_tex = 0;
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
      const EGLint pa[] = {
         EGL_WIDTH, w, EGL_HEIGHT, h,
         EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA, EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
         EGL_NONE
      };
      for (int i = 0; i < SLOTS; ++i) {
         g_gs.pb[i] = eglCreatePbufferSurface(g_guest_dpy, cfg, pa);
         if (g_gs.pb[i] == EGL_NO_SURFACE ||
             !query_ptr(g_guest_dpy, g_gs.pb[i], EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE,
                        &g_gs.handle[i]) || !g_gs.handle[i]) {
            glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
            share_off("pbuffer or share handle failed");
            return false;
         }
         glGenTextures(1, &g_gs.tex[i]);
         glBindTexture(GL_TEXTURE_2D, g_gs.tex[i]);
         if (!eglBindTexImage(g_guest_dpy, g_gs.pb[i], EGL_BACK_BUFFER)) {
            glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
            share_off("eglBindTexImage on the guest side failed");
            return false;
         }
      }
      glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
      glGenFramebuffers(1, &g_gs.fbo);
      g_gs.ctx = ctx;
      g_gs.w = w;
      g_gs.h = h;
      fprintf(stderr, "[compositor] shared D3D textures %dx%d x%d (frames stay on the GPU)\n",
              w, h, SLOTS);
   }

   /* The previous frame was copied and fenced, not waited for: by now the
    * GPU has normally finished it, so this wait is free and the guest never
    * stalls on its own frame.  It reaches the compositor one submit late. */
   if (g_gs.pending >= 0) {
      eglClientWaitSync(g_guest_dpy, g_gs.fence, EGL_SYNC_FLUSH_COMMANDS_BIT, EGL_FOREVER);
      eglDestroySync(g_guest_dpy, g_gs.fence);
      g_gs.fence = EGL_NO_SYNC;
   }

   pthread_mutex_lock(&g_mu);
   if (swap_interval > 0 && g_queued >= 0) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_nsec += 100 * 1000 * 1000;
      if (until.tv_nsec >= 1000000000) { until.tv_sec += 1; until.tv_nsec -= 1000000000; }
      while (g_queued >= 0 &&
             pthread_cond_timedwait(&g_cv_guest, &g_mu, &until) != ETIMEDOUT) {
      }
   }
   if (g_gs.pending >= 0) {
      const int p = g_gs.pending;
      g_slot_handle[p] = g_gs.handle[p];
      g_frame_w[p] = g_gs.w;
      g_frame_h[p] = g_gs.h;
      g_queued = p;
      g_gs.pending = -1;
      pthread_cond_signal(&g_cv_comp);
   }
   int slot = 0;
   while (slot == g_shown || slot == g_queued) ++slot;
   if (slot >= SLOTS) slot = g_queued;
   if (slot == g_queued) g_queued = -1;
   pthread_mutex_unlock(&g_mu);

   GLint prev_read = 0, prev_draw = 0;
   GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
   glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
   glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
   while (glGetError() != GL_NO_ERROR) {}
   if (scissor) glDisable(GL_SCISSOR_TEST);   /* a blit is scissored */
   glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gs.fbo);
   glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          g_gs.tex[slot], 0);
   glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
   glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
   glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
   if (scissor) glEnable(GL_SCISSOR_TEST);
   const GLenum err = glGetError();
   if (err != GL_NO_ERROR) {
      char why[64];
      snprintf(why, sizeof why, "blit error 0x%x", err);
      share_off(why);
      return false;
   }
   /* Another device reads the texture, so the copy must be finished before
    * the compositor is told about it.  Fence it and publish next time. */
   g_gs.fence = eglCreateSync(g_guest_dpy, EGL_SYNC_FENCE, NULL);
   if (g_gs.fence != EGL_NO_SYNC) {
      glFlush();
      g_gs.pending = slot;
      return true;
   }
   glFinish();

   pthread_mutex_lock(&g_mu);
   g_slot_handle[slot] = g_gs.handle[slot];
   g_frame_w[slot] = w;
   g_frame_h[slot] = h;
   g_queued = slot;
   pthread_cond_signal(&g_cv_comp);
   pthread_mutex_unlock(&g_mu);
   return true;
}

/* Hand over a fenced frame now: the pixel path is about to take over. */
static void share_flush_pending(void)
{
   if (g_gs.pending < 0) return;
   eglClientWaitSync(g_guest_dpy, g_gs.fence, EGL_SYNC_FLUSH_COMMANDS_BIT, EGL_FOREVER);
   eglDestroySync(g_guest_dpy, g_gs.fence);
   g_gs.fence = EGL_NO_SYNC;
   pthread_mutex_lock(&g_mu);
   const int p = g_gs.pending;
   g_slot_handle[p] = g_gs.handle[p];
   g_frame_w[p] = g_gs.w;
   g_frame_h[p] = g_gs.h;
   g_queued = p;
   g_gs.pending = -1;
   pthread_cond_signal(&g_cv_comp);
   pthread_mutex_unlock(&g_mu);
}

/* Which path is faster depends on the machine: the shared texture saves a
 * read-back and an upload, but a software device (WARP on a GPU-less box)
 * or a driver that copies shared resources can make it the slower one.  So
 * measure instead of guessing, keep the better one (the shared texture on a
 * near tie, since it spends less CPU), and look again when it may have
 * changed.  LUNARIA_COMP_SHARE=1/0 pins it.
 *
 * Measured on GitHub's runners the first version picked wrong in four runs
 * out of five, costing ~15 fps for two minutes at a time, because:
 *  - it timed shared, then pixels: a scene that got heavier between the two
 *    windows (menu into gameplay) was charged to the pixel path.  Now the
 *    windows run shared, pixels, pixels, shared, so a steady change in the
 *    scene lands on both paths equally;
 *  - in a menu both paths reach the frame cap, a tie that said nothing about
 *    the game, and it was then kept for 120 s.  A tie now looks again in
 *    20 s;
 *  - nothing noticed a heavier scene after the pick.  Now a frame rate that
 *    falls below 85% of what the pick measured looks again at once. */
#define PICK_WARMUP_NS   (20ull * 1000000000ull)
#define PICK_WINDOW_NS   (2ull * 1000000000ull)
#define PICK_RECHECK_NS  (120ull * 1000000000ull)
#define PICK_TIE_NS      (20ull * 1000000000ull)
#define PICK_LOADING_NS  (15ull * 1000000000ull)
#define PICK_WATCH_NS    (3ull * 1000000000ull)
#define PICK_MIN_GAP_NS  (15ull * 1000000000ull)
static bool share_choose(void)
{
   static int forced = -2;          /* -1 auto, 0 pixels, 1 shared */
   static int phase;                /* 0 using the pick, 1..4 timing windows */
   static bool pick = true;
   static uint64_t t0, next_check, last_pick, watch_t0;
   static unsigned f0, watch_f0;
   static uint64_t ns[2];           /* [0] pixels, [1] shared */
   static unsigned fr[2];
   static double pick_fps;
   static const bool order[5] = { false, true, false, false, true };   /* S P P S */
   if (forced == -2) {
      const char *env = getenv("LUNARIA_COMP_SHARE");
      forced = !env || !*env ? -1 : env[0] == '0' ? 0 : 1;
      if (forced >= 0)
         fprintf(stderr, "[compositor] frame path pinned to %s (LUNARIA_COMP_SHARE)\n",
                 forced ? "shared textures" : "pixels");
   }
   if (forced >= 0) return forced;
   if (g_share_state < 0) return false;
   const uint64_t now = now_ns();
   const unsigned frames = atomic_load(&g_comp_frames);
   if (!next_check) next_check = now + PICK_WARMUP_NS;

   if (phase == 0) {
      /* Watch the frame rate the pick is giving. */
      if (pick_fps > 0.0) {
         if (!watch_t0) { watch_t0 = now; watch_f0 = frames; }
         else if (now - watch_t0 >= PICK_WATCH_NS) {
            const double fps = (frames - watch_f0) * 1e9 / (double)(now - watch_t0);
            watch_t0 = now; watch_f0 = frames;
            if (fps >= 10.0 && fps < pick_fps * 0.85 && now - last_pick >= PICK_MIN_GAP_NS &&
                next_check > now) {
               fprintf(stderr, "[compositor] frame rate fell to %.1f fps from %.1f, timing both paths again\n",
                       fps, pick_fps);
               next_check = now;
            }
         }
      }
      if (now < next_check) return pick;
      phase = 1; t0 = now; f0 = frames;
      ns[0] = ns[1] = 0; fr[0] = fr[1] = 0;
      return order[1];
   }
   if (now - t0 < PICK_WINDOW_NS) return order[phase];
   /* Close this window on the path it timed. */
   ns[order[phase]] += now - t0;
   fr[order[phase]] += frames - f0;
   if (phase < 4) {
      ++phase; t0 = now; f0 = frames;
      return order[phase];
   }
   phase = 0;
   watch_t0 = 0;
   const double shared_fps = fr[1] * 1e9 / (double)ns[1];
   const double pixel_fps = fr[0] * 1e9 / (double)ns[0];
   /* A window that caught loading compares a loading screen against
    * gameplay, not two paths.  Keep the current pick and look again soon. */
   if (shared_fps < 10.0 || pixel_fps < 10.0) {
      next_check = now + PICK_LOADING_NS;
      return pick;
   }
   const bool was = pick;
   pick = shared_fps >= pixel_fps * 0.95;
   const bool tie = shared_fps < pixel_fps * 1.05 && pixel_fps < shared_fps * 1.05;
   pick_fps = pick ? shared_fps : pixel_fps;
   last_pick = now;
   static bool logged;
   if (pick != was || !logged) {
      fprintf(stderr, "[compositor] frame path: %s (shared %.1f fps, pixels %.1f fps)\n",
              pick ? "shared textures" : "pixels", shared_fps, pixel_fps);
      logged = true;
   }
   next_check = now + (tie ? PICK_TIE_NS : PICK_RECHECK_NS);
   return pick;
}
#endif

void luna_comp_submit(int w, int h, int swap_interval)
{
   if (!atomic_load_explicit(&g_active, memory_order_acquire) || w <= 0 || h <= 0)
      return;
   EGLContext ctx = eglGetCurrentContext();
   if (ctx == EGL_NO_CONTEXT) return;

#ifdef _WIN32
   if (g_own_display) {
      if (share_choose()) {
         if (share_submit(ctx, w, h, swap_interval)) return;
      } else {
         share_flush_pending();
      }
   }
#endif
   if (g_own_display) {
      /* The compositor is on another display: read the default framebuffer
       * back and hand it over as pixels (already in texture row order). */
      static _Thread_local uint8_t *buf;
      static _Thread_local size_t cap;
      const size_t need = (size_t)w * 4u * (size_t)h;
      if (cap < need) {
         uint8_t *nb = realloc(buf, need);
         if (!nb) return;
         buf = nb;
         cap = need;
      }
      GLint prev_read = 0, pack = 4;
      glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
      glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      glPixelStorei(GL_PACK_ALIGNMENT, 4);
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
      glPixelStorei(GL_PACK_ALIGNMENT, pack);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
      submit_pixels(w, h, buf, w * 4, false, swap_interval, true);
      return;
   }

   pthread_mutex_lock(&g_mu);
   /* A device's BufferQueue: with vsync on, a frame that is still queued
    * holds the producer until the consumer takes it.  Bounded, so a
    * compositor that has stopped cannot hang the guest. */
   if (swap_interval > 0 && g_queued >= 0) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_nsec += 100 * 1000 * 1000;
      if (until.tv_nsec >= 1000000000) { until.tv_sec += 1; until.tv_nsec -= 1000000000; }
      while (g_queued >= 0 &&
             pthread_cond_timedwait(&g_cv_guest, &g_mu, &until) != ETIMEDOUT) {
      }
   }
   int slot = 0;
   while (slot == g_shown || slot == g_queued) ++slot;
   if (slot >= SLOTS) slot = g_queued;   /* interval 0 and both taken: replace */
   EGLSync release = g_release_fence[slot];
   g_release_fence[slot] = EGL_NO_SYNC;
   EGLSync stale = EGL_NO_SYNC;
   if (slot == g_queued) {
      stale = g_ready_fence[slot];
      g_ready_fence[slot] = EGL_NO_SYNC;
      g_queued = -1;
   }
   pthread_mutex_unlock(&g_mu);

   if (stale != EGL_NO_SYNC) eglDestroySync(g_dpy, stale);
   if (release != EGL_NO_SYNC) {
      eglWaitSync(g_dpy, release, 0);
      eglDestroySync(g_dpy, release);
   }

   GLint prev_tex = 0, prev_read = 0;
   glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
   glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);

   struct comp_source *src = source_for(ctx, w, h);
   EGLSync ready = EGL_NO_SYNC;
   if (src) {
      /* Default framebuffer -> slot.  glCopyTexSubImage2D needs no
       * framebuffer object (those are per context) and, unlike a blit, is
       * not clipped by the guest's scissor. */
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      glBindTexture(GL_TEXTURE_2D, src->tex[slot]);
      glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
      ready = eglCreateSync(g_dpy, EGL_SYNC_FENCE, NULL);
      glFlush();
   }
   glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
   glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
   if (!src) return;

   pthread_mutex_lock(&g_mu);
   g_ready_fence[slot] = ready;
   g_slot_img[slot] = src->img[slot];
   g_frame_w[slot] = w;
   g_frame_h[slot] = h;
   g_queued = slot;
   pthread_cond_signal(&g_cv_comp);
   pthread_mutex_unlock(&g_mu);
}

static void submit_pixels(int w, int h, const void *pixels, int stride,
                          bool bgra, int swap_interval, bool bottom_up)
{
   if (!atomic_load_explicit(&g_active, memory_order_acquire) || w <= 0 ||
       h <= 0 || !pixels)
      return;
   const size_t row = (size_t)w * 4u, need = row * (size_t)h;
   pthread_mutex_lock(&g_mu);
   if (swap_interval > 0 && g_px_pending) {
      struct timespec until;
      clock_gettime(CLOCK_REALTIME, &until);
      until.tv_nsec += 100 * 1000 * 1000;
      if (until.tv_nsec >= 1000000000) { until.tv_sec += 1; until.tv_nsec -= 1000000000; }
      while (g_px_pending &&
             pthread_cond_timedwait(&g_cv_guest, &g_mu, &until) != ETIMEDOUT) {
      }
   }
   if (g_px_cap < need) {
      uint8_t *nb = realloc(g_px_buf, need);
      if (!nb) { pthread_mutex_unlock(&g_mu); return; }
      g_px_buf = nb;
      g_px_cap = need;
   }
   /* GL's texture rows run bottom-up; a top-down frame is flipped on the way
    * in, a glReadPixels result is already in texture order. */
   if (bottom_up && !bgra && (size_t)stride == row) {
      memcpy(g_px_buf, pixels, need);
   } else for (int y = 0; y < h; ++y) {
      const uint8_t *src = (const uint8_t *)pixels + (size_t)y * (size_t)stride;
      uint8_t *dst = g_px_buf + (size_t)(bottom_up ? y : h - 1 - y) * row;
      if (!bgra) {
         memcpy(dst, src, row);
         continue;
      }
      for (int x = 0; x < w; ++x) {
         dst[4 * x + 0] = src[4 * x + 2];
         dst[4 * x + 1] = src[4 * x + 1];
         dst[4 * x + 2] = src[4 * x + 0];
         dst[4 * x + 3] = src[4 * x + 3];
      }
   }
   g_px_w = w;
   g_px_h = h;
   g_px_pending = true;
   pthread_cond_signal(&g_cv_comp);
   pthread_mutex_unlock(&g_mu);
}

void luna_comp_submit_pixels(int w, int h, const void *pixels, int stride,
                             bool bgra, int swap_interval)
{
   submit_pixels(w, h, pixels, stride, bgra, swap_interval, false);
}

void luna_comp_submit_pixels_bottom_up(int w, int h, const void *pixels,
                                       int stride, bool bgra, int swap_interval)
{
   submit_pixels(w, h, pixels, stride, bgra, swap_interval, true);
}

void luna_comp_wake(void)
{
   if (!atomic_load_explicit(&g_active, memory_order_acquire)) return;
   pthread_mutex_lock(&g_mu);
   g_wake = true;
   pthread_cond_signal(&g_cv_comp);
   pthread_mutex_unlock(&g_mu);
}

bool luna_comp_active(void)
{
   return atomic_load_explicit(&g_active, memory_order_acquire);
}

unsigned long long luna_comp_frames(void)
{
   return atomic_load_explicit(&g_frames, memory_order_relaxed);
}

/* ---- compositor thread ---------------------------------------------------- */

static GLuint g_prog, g_vbo, g_vao;
static GLint  g_u_tex;
static GLuint g_comp_tex[SLOTS];      /* the compositor's view of each slot */
static GLuint g_px_tex;               /* the last frame submitted as pixels */
static int g_px_tex_w, g_px_tex_h;
static bool g_px_shown;               /* g_px_tex is what is on screen */
static EGLImage g_comp_img[SLOTS];    /* the image bound to it */

#ifdef _WIN32
static void      *g_comp_handle[SLOTS];  /* share handle bound to g_comp_tex */
static EGLSurface g_comp_pb[SLOTS];

/* Compositor thread: open the guest's D3D texture for this slot. */
static void comp_bind_shared(int slot, void *handle)
{
   static EGLConfig pb_cfg;
   static int have_cfg;
   if (!handle || handle == g_comp_handle[slot]) return;
   if (!have_cfg) {
      const EGLint ca[] = {
         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
         EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_BIND_TO_TEXTURE_RGBA, EGL_TRUE,
         EGL_NONE
      };
      EGLint n = 0;
      have_cfg = eglChooseConfig(g_dpy, ca, &pb_cfg, 1, &n) && n == 1 ? 1 : -1;
   }
   if (g_comp_pb[slot]) {
      glBindTexture(GL_TEXTURE_2D, g_comp_tex[slot]);
      eglReleaseTexImage(g_dpy, g_comp_pb[slot], EGL_BACK_BUFFER);
      eglDestroySurface(g_dpy, g_comp_pb[slot]);
      g_comp_pb[slot] = EGL_NO_SURFACE;
      g_comp_handle[slot] = NULL;
   }
   EGLSurface s = EGL_NO_SURFACE;
   if (have_cfg > 0) {
      const EGLint a[] = {
         EGL_WIDTH, g_frame_w[slot], EGL_HEIGHT, g_frame_h[slot],
         EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA, EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
         EGL_NONE
      };
      s = eglCreatePbufferFromClientBuffer(g_dpy, EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE,
                                           (EGLClientBuffer)handle, pb_cfg, a);
   }
   glBindTexture(GL_TEXTURE_2D, g_comp_tex[slot]);
   if (s == EGL_NO_SURFACE || !eglBindTexImage(g_dpy, s, EGL_BACK_BUFFER)) {
      static int warned;
      if (warned++ < 4)
         fprintf(stderr, "[compositor] opening a shared texture failed (0x%x)\n",
                 eglGetError());
      if (s != EGL_NO_SURFACE) eglDestroySurface(g_dpy, s);
      return;
   }
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   g_comp_pb[slot] = s;
   g_comp_handle[slot] = handle;
}
#endif

static GLuint compile(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   GLint ok = 0;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[512];
      glGetShaderInfoLog(s, sizeof log, NULL, log);
      fprintf(stderr, "[compositor] shader: %s\n", log);
   }
   return s;
}

static bool gl_setup(void)
{
   static const char vs[] =
      "attribute vec2 p;\n"
      "varying vec2 uv;\n"
      "void main(){ uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
   static const char fs[] =
      "precision mediump float;\n"
      "varying vec2 uv;\n"
      "uniform sampler2D t;\n"
      "void main(){ gl_FragColor = vec4(texture2D(t, uv).rgb, 1.0); }\n";
   g_prog = glCreateProgram();
   glAttachShader(g_prog, compile(GL_VERTEX_SHADER, vs));
   glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation(g_prog, 0, "p");
   glLinkProgram(g_prog);
   GLint ok = 0;
   glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
   if (!ok) return false;
   g_u_tex = glGetUniformLocation(g_prog, "t");
   static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
   glGenTextures(SLOTS, g_comp_tex);
   glGenTextures(1, &g_px_tex);
   p_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
      eglGetProcAddress("glEGLImageTargetTexture2DOES");
   if (!p_glEGLImageTargetTexture2DOES) return false;
   /* A vertex array of the compositor's own: luna-ui draws in this context
    * too, and attribute state set without one lands in whichever VAO luna-ui
    * last bound. */
   glGenVertexArrays(1, &g_vao);
   glBindVertexArray(g_vao);
   glGenBuffers(1, &g_vbo);
   glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
   glEnableVertexAttribArray(0);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
   glBindVertexArray(0);
   glBindBuffer(GL_ARRAY_BUFFER, 0);
   return true;
}

static void draw_texture(GLuint tex, int fw, int fh, int win_w, int win_h)
{
   /* Letterboxed to the frame's aspect ratio: a window resized by the user
    * shows the guest's picture undistorted. */
   int vw = win_w, vh = win_h, vx = 0, vy = 0;
   if (fw > 0 && fh > 0) {
      if ((long long)win_w * fh > (long long)win_h * fw) {
         vw = (int)((long long)win_h * fw / fh);
         vx = (win_w - vw) / 2;
      } else {
         vh = (int)((long long)win_w * fh / fw);
         vy = (win_h - vh) / 2;
      }
   }
   glViewport(vx, vy, vw, vh);
   glUseProgram(g_prog);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, tex);
   glUniform1i(g_u_tex, 0);
   glBindVertexArray(g_vao);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   glBindVertexArray(0);
   glViewport(0, 0, win_w, win_h);
}

/* The state the compositor's own draw expects, reset every frame: luna-ui
 * changes blending, scissor and program freely. */
static void reset_state(int w, int h)
{
   glBindFramebuffer(GL_FRAMEBUFFER, 0);
   glViewport(0, 0, w, h);
   glDisable(GL_BLEND);
   glDisable(GL_SCISSOR_TEST);
   glDisable(GL_DEPTH_TEST);
   glDisable(GL_CULL_FACE);
   glDisable(GL_STENCIL_TEST);
   glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

/* LUNARIA_COMP_SHOT_S=<seconds>: every that many seconds, write what the
 * compositor is about to put on screen — the guest frame with the emulator's
 * own UI over it — to $LUNARIA_DUMP_DIR (default /tmp) as comp_NNNN.ppm.
 * The guest-side LUNARIA_SCREENSHOT_EVERY sees only the guest's frame; this
 * is the only picture of the boot card, dialogs and the input method as the
 * user sees them. */
static void comp_maybe_dump(int w, int h)
{
   static int probed;
   static uint64_t every_ns, next_ns;
   static unsigned seq;
   if (!probed) {
      probed = 1;
      const char *e = getenv("LUNARIA_COMP_SHOT_S");
      const double s = e ? atof(e) : 0.0;
      every_ns = s > 0.0 ? (uint64_t)(s * 1e9) : 0u;
      next_ns = now_ns() + every_ns;
   }
   if (!every_ns || w <= 0 || h <= 0) return;
   const uint64_t t = now_ns();
   if (t < next_ns) return;
   next_ns = t + every_ns;
   const size_t row = (size_t)w * 4u;
   uint8_t *px = malloc(row * (size_t)h);
   if (!px) return;
   glPixelStorei(GL_PACK_ALIGNMENT, 1);
   glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
   const char *dir = getenv("LUNARIA_DUMP_DIR");
   char path[1024];
   snprintf(path, sizeof path, "%s/comp_%04u.ppm", dir && *dir ? dir : "/tmp",
            seq++);
   FILE *f = fopen(path, "wb");
   if (f) {
      fprintf(f, "P6\n%d %d\n255\n", w, h);
      for (int y = h - 1; y >= 0; --y)
         for (int x = 0; x < w; ++x)
            fwrite(px + (size_t)y * row + (size_t)x * 4u, 1, 3, f);
      fclose(f);
      fprintf(stderr, "[compositor] shot -> %s\n", path);
   }
   free(px);
}

static void *comp_main(void *arg)
{
   (void)arg;
   if (!eglMakeCurrent(g_dpy, g_win, g_win, g_ctx)) {
      fprintf(stderr, "[compositor] eglMakeCurrent failed (0x%x)\n", eglGetError());
      atomic_store(&g_active, false);
      luna_overlay_set_hosted(false);
      return NULL;
   }
   eglSwapInterval(g_dpy, 1);
   if (!gl_setup()) {
      fprintf(stderr, "[compositor] shader setup failed\n");
      atomic_store(&g_active, false);
      luna_overlay_set_hosted(false);
      return NULL;
   }
   luna_overlay_bind_host_thread();
   fprintf(stderr, "[compositor] running (window owned by its own thread)\n");

   bool have_frame = false;
   uint64_t last_draw = 0;
   for (;;) {
      /* How long the picture may stay as it is: the boot card and the input
       * method's caret animate; a static screen needs nothing until the guest
       * or the UI changes it. */
      const bool boot = luna_boot_active() && !have_frame && !luna_overlay_guest_window_up();
      const bool ime = luna_ime_active();
      const uint64_t period = boot ? 16000000ull : ime ? 33000000ull
                             : luna_overlay_active() ? 100000000ull
                             : 1000000000ull;

      pthread_mutex_lock(&g_mu);
      if (g_queued < 0 && !g_wake && !g_px_pending) {
         const uint64_t due = last_draw + period;
         const uint64_t t = now_ns();
         if (due > t) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            uint64_t ns = (uint64_t)until.tv_nsec + (due - t);
            until.tv_sec += (time_t)(ns / 1000000000ull);
            until.tv_nsec = (long)(ns % 1000000000ull);
            (void)pthread_cond_timedwait(&g_cv_comp, &g_mu, &until);
         }
      }
      g_wake = false;
      int take = g_queued;
      EGLSync ready = EGL_NO_SYNC;
      EGLImage take_img = EGL_NO_IMAGE;
      void *take_handle = NULL;
      int prev_shown = g_shown;
      if (take >= 0) {
         /* Retire the old shown slot before publishing a different one.
          * All draws sampling the old slot were submitted on this context
          * in earlier iterations. The producer may immediately reuse it
          * after g_shown changes, so its release fence must already exist. */
         take_handle = g_slot_handle[take];
         if (prev_shown >= 0 && prev_shown != take && !g_own_display) {
            EGLSync done = eglCreateSync(g_dpy, EGL_SYNC_FENCE, NULL);
            glFlush();
            if (g_release_fence[prev_shown] != EGL_NO_SYNC)
               eglDestroySync(g_dpy, g_release_fence[prev_shown]);
            g_release_fence[prev_shown] = done;
         }
         ready = g_ready_fence[take];
         take_img = g_slot_img[take];
         g_ready_fence[take] = EGL_NO_SYNC;
         g_queued = -1;
         g_shown = take;
         pthread_cond_signal(&g_cv_guest);
      }
      const int shown = g_shown;
      if (g_px_pending) {
         /* Uploaded under the lock: the buffer is the submitter's to refill
          * as soon as it is released. */
         glBindTexture(GL_TEXTURE_2D, g_px_tex);
         if (g_px_tex_w != g_px_w || g_px_tex_h != g_px_h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_px_w, g_px_h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, g_px_buf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g_px_tex_w = g_px_w;
            g_px_tex_h = g_px_h;
         } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_px_w, g_px_h, GL_RGBA,
                            GL_UNSIGNED_BYTE, g_px_buf);
         }
         g_px_pending = false;
         atomic_fetch_add(&g_comp_frames, 1);
         g_px_shown = true;
         have_frame = true;
         pthread_cond_signal(&g_cv_guest);
      } else if (take >= 0) {
         g_px_shown = false;
      }
      pthread_mutex_unlock(&g_mu);

      if (take >= 0) {
         if (ready != EGL_NO_SYNC) {
            eglWaitSync(g_dpy, ready, 0);
            eglDestroySync(g_dpy, ready);
         }
#ifdef _WIN32
         if (g_own_display) {
            comp_bind_shared(take, take_handle);
         } else
#endif
         if (take_img != g_comp_img[take]) {
            glBindTexture(GL_TEXTURE_2D, g_comp_tex[take]);
            p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)take_img);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g_comp_img[take] = take_img;
         }
         atomic_fetch_add(&g_comp_frames, 1);
         have_frame = true;
      }
      /* Images a resize or an evicted context left behind, once none of
       * this side's slots is bound to them. */
      pthread_mutex_lock(&g_mu);
      for (int i = 0; i < g_nretired; ) {
         bool bound = false;
         for (int k = 0; k < SLOTS; ++k)
            bound |= g_comp_img[k] == g_retired[i] || g_slot_img[k] == g_retired[i];
         if (bound) { ++i; continue; }
         eglDestroyImage(g_dpy, g_retired[i]);
         g_retired[i] = g_retired[--g_nretired];
      }
      pthread_mutex_unlock(&g_mu);

      EGLint w = 0, h = 0;
      eglQuerySurface(g_dpy, g_win, EGL_WIDTH, &w);
      eglQuerySurface(g_dpy, g_win, EGL_HEIGHT, &h);
      if (w <= 0 || h <= 0) { last_draw = now_ns(); continue; }

      reset_state(w, h);
      glClearColor(0.f, 0.f, 0.f, 1.f);
      glClear(GL_COLOR_BUFFER_BIT);
      if (have_frame && g_px_shown)
         draw_texture(g_px_tex, g_px_tex_w, g_px_tex_h, w, h);
      else if (have_frame && shown >= 0)
         draw_texture(g_comp_tex[shown], g_frame_w[shown], g_frame_h[shown], w, h);

      const bool draw_boot=luna_boot_active() && !have_frame && !luna_overlay_guest_window_up();
      if (!luna_overlay_present_checked(w,h,draw_boot)) {
         /* The front buffer is still the last complete frame. Font shaping
          * can hold the overlay lock while the guest builds a new document;
          * publishing just the cleared backdrop would flash black. */
         last_draw=now_ns();
         continue;
      }

      comp_maybe_dump(w, h);
      eglSwapBuffers(g_dpy, g_win);
      atomic_fetch_add_explicit(&g_frames, 1, memory_order_relaxed);
      last_draw = now_ns();
   }
   return NULL;
}

#ifdef _WIN32
/* A second ANGLE display, on the same backend as the guest's, keyed by the
 * window's DC so ANGLE does not hand back the guest's display. */
static EGLDisplay comp_own_display(EGLNativeWindowType win, EGLDisplay guest,
                                   EGLConfig *cfg)
{
   const char *shared = getenv("LUNARIA_COMP_SHARED_DISPLAY");
   if (shared && shared[0] && shared[0] != '0') return EGL_NO_DISPLAY;
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
      eglGetProcAddress("eglGetPlatformDisplayEXT");
   HDC dc = GetDC((HWND)win);
   if (!gpd || !dc) return EGL_NO_DISPLAY;
   const char *be = getenv("LUNARIA_ANGLE_BACKEND");
   const EGLint types[2] = {
      be && !strcmp(be, "vulkan") ? EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE
                                  : EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
      EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE,
   };
   for (int i = 0; i < 2; ++i) {
      const EGLint attrs[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, types[i], EGL_NONE };
      EGLDisplay d = gpd(EGL_PLATFORM_ANGLE_ANGLE, (void *)dc, attrs);
      EGLint major = 0, minor = 0;
      if (d == EGL_NO_DISPLAY || d == guest || !eglInitialize(d, &major, &minor))
         continue;
      const EGLint ca[] = {
         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
         EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
         EGL_NONE
      };
      EGLint n = 0;
      if (eglChooseConfig(d, ca, cfg, 1, &n) && n == 1) return d;
      eglTerminate(d);
   }
   return EGL_NO_DISPLAY;
}
#endif

bool luna_comp_start(void *egl_display, void *egl_config, void *native_window)
{
   if (atomic_load(&g_active)) return true;
   g_dpy = (EGLDisplay)egl_display;
   g_guest_dpy = g_dpy;
   g_cfg = (EGLConfig)egl_config;
   g_native = (EGLNativeWindowType)(uintptr_t)native_window;

#ifdef _WIN32
   {
      EGLConfig own_cfg;
      EGLDisplay own = comp_own_display(g_native, g_dpy, &own_cfg);
      if (own != EGL_NO_DISPLAY) {
         EGLSurface s = eglCreateWindowSurface(own, own_cfg, g_native, NULL);
         if (s != EGL_NO_SURFACE) {
            g_dpy = own;
            g_cfg = own_cfg;
            g_win = s;
            g_own_display = true;
            fprintf(stderr, "[compositor] own ANGLE display (separate device); "
                    "guest frames arrive as pixels\n");
         } else {
            fprintf(stderr, "[compositor] own display: window surface failed (0x%x), "
                    "sharing the guest's display\n", eglGetError());
         }
      }
   }
#endif
   if (g_win == EGL_NO_SURFACE)
      g_win = eglCreateWindowSurface(g_dpy, g_cfg, g_native, NULL);
   if (g_win == EGL_NO_SURFACE) {
      fprintf(stderr, "[compositor] eglCreateWindowSurface failed (0x%x)\n",
              eglGetError());
      return false;
   }
   static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
   /* A share group of its own.  Frames arrive as EGLImages, which belong
    * to the display, so nothing here needs the guest's objects — and the
    * guest must not reach the emulator's: GLES lets an application bind a
    * buffer name it never generated, and Genshin's writes landed in luna-ui's
    * quad buffer, so every dialog and the input method drew nothing once the
    * game was rendering. */
   g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attr);
   if (g_ctx == EGL_NO_CONTEXT) {
      fprintf(stderr, "[compositor] eglCreateContext failed (0x%x)\n", eglGetError());
      eglDestroySurface(g_dpy, g_win);
      g_win = EGL_NO_SURFACE;
      return false;
   }
   luna_overlay_set_wake(luna_comp_wake);
   /* Before the thread starts: from here on only it draws the overlay. */
   luna_overlay_set_hosted(true);
   atomic_store(&g_active, true);
   if (pthread_create(&g_thread, NULL, comp_main, NULL) != 0) {
      atomic_store(&g_active, false);
      luna_overlay_set_hosted(false);
      return false;
   }
   pthread_detach(g_thread);
   return true;
}
