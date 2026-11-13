/* MPL-2.0.  java.util.stream, eagerly.
 *
 * Nothing modelled Stream, so List.stream() answered null and the first
 * .filter() threw a NullPointerException.  Kefir's CheaterChecker
 * (GetActivities, IsCheaterAppInstalled) runs exactly that chain while Last
 * Day on Earth logs in, and threw instead of answering (Aloa, 14-11).
 *
 * A stream here is a snapshot: every intermediate operation runs at once
 * and hands back a new stream over an ArrayList of the results.  Laziness
 * is only observable through side effects inside lambdas (ordering against
 * an infinite source, short-circuiting), and none of the callers seen so far
 * depend on it.  Included from dvm_runtime.c, after list_init/list_add. */

static const struct rt_field rt_list_stream_fields[] = {
   { "items", "Ljava/util/ArrayList;" },
   F_END
};
static const struct rt_field rt_simple_collector_fields[] = {
   { "kind", "I" },
   { "sep", "Ljava/lang/String;" },
   { "prefix", "Ljava/lang/String;" },
   { "suffix", "Ljava/lang/String;" },
   F_END
};
static const char *const rt_list_stream_ifaces[] = {
   "Ljava/util/stream/Stream;", "Ljava/util/stream/BaseStream;",
   "Ljava/lang/AutoCloseable;", NULL
};
static const char *const rt_stream_iface_base[] = {
   "Ljava/util/stream/BaseStream;", NULL
};
static const char *const rt_simple_collector_ifaces[] = {
   "Ljava/util/stream/Collector;", NULL
};

enum { STREAM_TO_LIST = 0, STREAM_TO_SET = 1, STREAM_JOINING = 2 };

static dvm_ref stream_new_list(struct dvm *vm)
{
   struct dvm_class *c = dvm__class_by_desc(vm, "Ljava/util/ArrayList;");
   dvm_ref l = c ? dvm_new_object(vm, c) : 0;
   union dvm_value ignored = { 0 };
   if (l) (void)list_init(vm, l, NULL, 0, &ignored);
   return l;
}
static void stream_list_add(struct dvm *vm, dvm_ref list, dvm_ref item)
{
   union dvm_value a = { .l = item }, ignored = { 0 };
   (void)list_add(vm, list, &a, 1, &ignored);
}
static dvm_ref stream_wrap(struct dvm *vm, dvm_ref list)
{
   struct dvm_class *c = dvm__class_by_desc(vm, "Llunaria/util/ListStream;");
   dvm_ref s = c ? dvm_new_object(vm, c) : 0;
   if (s) {
      union dvm_value v = { .l = list };
      (void)dvm_set_field(vm, s, "items", "Ljava/util/ArrayList;", v);
      dvm_pin(vm, s);
   }
   return s;
}
static struct rt_list *stream_items(struct dvm *vm, dvm_ref self)
{
   union dvm_value v = { 0 };
   if (!dvm_get_field(vm, self, "items", "Ljava/util/ArrayList;", &v) || !v.l)
      return NULL;
   return list_of(vm, v.l);
}
static bool stream_is_ours(struct dvm *vm, dvm_ref obj)
{
   struct dvm_class *c = obj ? dvm_object_class(vm, obj) : NULL;
   return c && c->name && !strcmp(c->name, "lunaria/util/ListStream");
}
/* Append every element an Iterable (or one of our streams) yields. */
static bool stream_drain(struct dvm *vm, dvm_ref src, dvm_ref list)
{
   if (!src) return true;
   if (stream_is_ours(vm, src)) {
      struct rt_list *l = stream_items(vm, src);
      for (uint32_t i = 0; l && i < l->size; ++i) stream_list_add(vm, list, l->items[i]);
      return true;
   }
   union dvm_value it = { 0 }, more = { 0 }, item = { 0 };
   if (!am_vcall(vm, src, "iterator", "()Ljava/util/Iterator;", NULL, 0, &it)) return false;
   for (;;) {
      if (!am_vcall(vm, it.l, "hasNext", "()Z", NULL, 0, &more)) return false;
      if (!more.i) return true;
      if (!am_vcall(vm, it.l, "next", "()Ljava/lang/Object;", NULL, 0, &item)) return false;
      stream_list_add(vm, list, item.l);
   }
}
static dvm_ref stream_from_array(struct dvm *vm, dvm_ref array, int32_t from, int32_t to)
{
   dvm_ref list = stream_new_list(vm);
   if (!list) return 0;
   uint32_t n = array ? dvm_array_length(vm, array) : 0;
   dvm_ref *data = array ? dvm_array_data(vm, array) : NULL;
   if (to < 0 || (uint32_t)to > n) to = (int32_t)n;
   for (int32_t i = from < 0 ? 0 : from; data && i < to; ++i) stream_list_add(vm, list, data[i]);
   return stream_wrap(vm, list);
}

/* Collection.stream() / parallelStream(). */
static bool collection_stream(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                              int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   dvm_ref list = stream_new_list(vm);
   if (!list || !stream_drain(vm, self, list)) return false;
   RETL(stream_wrap(vm, list));
}

/* filter / map / flatMap / peek. */
static bool stream_transform(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                             int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   bool filter = !strcmp(name, "filter"), flat = !strcmp(name, "flatMap"),
        peek = !strcmp(name, "peek");
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "%s", name); return false; }
   struct rt_list *l = stream_items(vm, self);
   dvm_ref list = stream_new_list(vm);
   if (!list) return false;
   for (uint32_t i = 0; l && i < l->size; ++i) {
      union dvm_value item = { .l = l->items[i] }, r = { 0 };
      if (filter || peek) {
         if (!am_vcall(vm, ARG(0).l, filter ? "test" : "accept",
                       filter ? "(Ljava/lang/Object;)Z" : "(Ljava/lang/Object;)V",
                       &item, 1, &r)) return false;
         if (peek || r.i) stream_list_add(vm, list, item.l);
      } else {
         if (!am_vcall(vm, ARG(0).l, "apply", "(Ljava/lang/Object;)Ljava/lang/Object;",
                       &item, 1, &r)) return false;
         if (!flat) stream_list_add(vm, list, r.l);
         else if (!stream_drain(vm, r.l, list)) return false;
      }
   }
   RETL(stream_wrap(vm, list));
}

/* distinct / limit / skip. */
static bool stream_slice(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   struct rt_list *l = stream_items(vm, self);
   dvm_ref list = stream_new_list(vm);
   if (!list) return false;
   uint32_t n = l ? l->size : 0, from = 0, to = n;
   if (!strcmp(name, "limit") && ARG(0).j >= 0 && (uint64_t)ARG(0).j < n) to = (uint32_t)ARG(0).j;
   if (!strcmp(name, "skip") && ARG(0).j > 0) from = (uint64_t)ARG(0).j < n ? (uint32_t)ARG(0).j : n;
   bool distinct = !strcmp(name, "distinct");
   for (uint32_t i = from; i < to; ++i) {
      if (distinct) {
         union dvm_value probe = { .l = l->items[i] }, has = { 0 };
         if (!am_vcall(vm, list, "contains", "(Ljava/lang/Object;)Z", &probe, 1, &has)) return false;
         if (has.i) continue;
      }
      stream_list_add(vm, list, l->items[i]);
   }
   RETL(stream_wrap(vm, list));
}

/* sorted() / sorted(Comparator): through the list's own sort. */
static bool stream_sorted(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   dvm_ref list = stream_new_list(vm);
   union dvm_value cmp = { .l = nargs > 0 ? ARG(0).l : 0 }, ignored = { 0 };
   if (!list || !stream_drain(vm, self, list)) return false;
   struct dvm_method *sort = dvm_lookup(vm, "java/util/Collections", "sort",
                                        "(Ljava/util/List;Ljava/util/Comparator;)V");
   union dvm_value call[2] = { { .l = list }, cmp };
   if (sort && !dvm_call(vm, sort, 0, call, 2, &ignored)) return false;
   RETL(stream_wrap(vm, list));
}

static bool stream_for_each(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)nargs; (void)out;
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "forEach"); return false; }
   struct rt_list *l = stream_items(vm, self);
   for (uint32_t i = 0; l && i < l->size; ++i) {
      union dvm_value item = { .l = l->items[i] }, r = { 0 };
      if (!am_vcall(vm, ARG(0).l, "accept", "(Ljava/lang/Object;)V", &item, 1, &r)) return false;
   }
   RETV();
}

/* anyMatch / allMatch / noneMatch. */
static bool stream_match(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)nargs;
   const char *name = builtin_name();
   bool any = !strcmp(name, "anyMatch"), all = !strcmp(name, "allMatch");
   if (!ARG(0).l) { dvm__throw(vm, "java/lang/NullPointerException", "%s", name); return false; }
   struct rt_list *l = stream_items(vm, self);
   for (uint32_t i = 0; l && i < l->size; ++i) {
      union dvm_value item = { .l = l->items[i] }, r = { 0 };
      if (!am_vcall(vm, ARG(0).l, "test", "(Ljava/lang/Object;)Z", &item, 1, &r)) return false;
      if (any && r.i) RETI(1);
      if (all && !r.i) RETI(0);
      if (!any && !all && r.i) RETI(0);
   }
   RETI(any ? 0 : 1);
}

static bool stream_count(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                         int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   struct rt_list *l = stream_items(vm, self);
   out->j = l ? (int64_t)l->size : 0;
   return true;
}

/* toArray() / toArray(IntFunction<A[]>). */
static bool stream_to_array(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   struct rt_list *l = stream_items(vm, self);
   uint32_t n = l ? l->size : 0;
   dvm_ref a = 0;
   if (nargs > 0 && ARG(0).l) {
      union dvm_value len = { .i = (int32_t)n }, r = { 0 };
      if (!am_vcall(vm, ARG(0).l, "apply", "(I)Ljava/lang/Object;", &len, 1, &r)) return false;
      a = r.l;
   } else {
      a = dvm_new_array(vm, 'L', "Ljava/lang/Object;", n);
   }
   dvm_ref *data = a ? dvm_array_data(vm, a) : NULL;
   uint32_t cap = a ? dvm_array_length(vm, a) : 0;
   for (uint32_t i = 0; data && i < n && i < cap; ++i) data[i] = l->items[i];
   RETL(a);
}

static bool stream_iterator(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)args; (void)nargs;
   union dvm_value v = { 0 };
   if (!dvm_get_field(vm, self, "items", "Ljava/util/ArrayList;", &v) || !v.l) RETL(0);
   return am_vcall(vm, v.l, "iterator", "()Ljava/util/Iterator;", NULL, 0, out);
}

/* sequential / parallel / unordered / onClose: same stream. */
static bool stream_self(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                        int nargs, union dvm_value *out)
{
   (void)vm; (void)args; (void)nargs;
   RETL(self);
}

static dvm_ref collector_new(struct dvm *vm, int kind, dvm_ref sep, dvm_ref prefix, dvm_ref suffix)
{
   struct dvm_class *c = dvm__class_by_desc(vm, "Llunaria/util/SimpleCollector;");
   dvm_ref o = c ? dvm_new_object(vm, c) : 0;
   if (!o) return 0;
   union dvm_value k = { .i = kind }, s = { .l = sep }, p = { .l = prefix }, x = { .l = suffix };
   (void)dvm_set_field(vm, o, "kind", "I", k);
   (void)dvm_set_field(vm, o, "sep", "Ljava/lang/String;", s);
   (void)dvm_set_field(vm, o, "prefix", "Ljava/lang/String;", p);
   (void)dvm_set_field(vm, o, "suffix", "Ljava/lang/String;", x);
   dvm_pin(vm, o);
   return o;
}
/* A CharSequence argument as a String (StringBuilder, String, ...). */
static dvm_ref stream_as_string(struct dvm *vm, dvm_ref cs)
{
   if (!cs) return 0;
   union dvm_value r = { 0 };
   return am_vcall(vm, cs, "toString", "()Ljava/lang/String;", NULL, 0, &r) ? r.l : 0;
}
/* Collectors.toList() / toSet() / joining(...). */
static bool collectors_make(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                            int nargs, union dvm_value *out)
{
   (void)self;
   const char *name = builtin_name();
   int kind = !strcmp(name, "joining") ? STREAM_JOINING
            : !strcmp(name, "toSet") ? STREAM_TO_SET : STREAM_TO_LIST;
   dvm_ref sep = nargs > 0 ? stream_as_string(vm, ARG(0).l) : 0;
   dvm_ref prefix = nargs > 2 ? stream_as_string(vm, ARG(1).l) : 0;
   dvm_ref suffix = nargs > 2 ? stream_as_string(vm, ARG(2).l) : 0;
   RETL(collector_new(vm, kind, sep, prefix, suffix));
}

struct stream_text { char *p; size_t n, cap; };
static void stream_text_add(struct stream_text *t, const char *s)
{
   size_t k = s ? strlen(s) : 0;
   if (t->n + k + 1 > t->cap) {
      size_t cap = (t->cap ? t->cap * 2 : 256) + k;
      char *p = realloc(t->p, cap);
      if (!p) return;
      t->p = p; t->cap = cap;
   }
   if (k) memcpy(t->p + t->n, s, k);
   t->n += k;
   t->p[t->n] = '\0';
}

static bool stream_collect(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                           int nargs, union dvm_value *out)
{
   (void)nargs;
   dvm_ref c = ARG(0).l;
   struct dvm_class *cc = c ? dvm_object_class(vm, c) : NULL;
   if (!cc || !cc->name || strcmp(cc->name, "lunaria/util/SimpleCollector")) {
      dvm__throw(vm, "java/lang/UnsupportedOperationException",
                 "Stream.collect with %s", cc && cc->name ? cc->name : "null");
      return false;
   }
   union dvm_value kind = { 0 };
   (void)dvm_get_field(vm, c, "kind", "I", &kind);
   struct rt_list *l = stream_items(vm, self);
   if (kind.i == STREAM_JOINING) {
      union dvm_value sep = { 0 }, prefix = { 0 }, suffix = { 0 };
      (void)dvm_get_field(vm, c, "sep", "Ljava/lang/String;", &sep);
      (void)dvm_get_field(vm, c, "prefix", "Ljava/lang/String;", &prefix);
      (void)dvm_get_field(vm, c, "suffix", "Ljava/lang/String;", &suffix);
      struct stream_text t = { 0 };
      stream_text_add(&t, prefix.l ? dvm_string_utf8(vm, prefix.l) : "");
      for (uint32_t i = 0; l && i < l->size; ++i) {
         if (i && sep.l) stream_text_add(&t, dvm_string_utf8(vm, sep.l));
         dvm_ref s = l->items[i] ? stream_as_string(vm, l->items[i]) : 0;
         stream_text_add(&t, s ? dvm_string_utf8(vm, s) : "null");
      }
      stream_text_add(&t, suffix.l ? dvm_string_utf8(vm, suffix.l) : "");
      dvm_ref r = dvm_new_string(vm, t.p ? t.p : "");
      free(t.p);
      RETL(r);
   }
   if (kind.i == STREAM_TO_SET) {
      struct dvm_class *hs = dvm__class_by_desc(vm, "Ljava/util/HashSet;");
      struct dvm_method *init = hs ? dvm_find_method(vm, hs, "<init>", "()V") : NULL;
      dvm_ref set = hs ? dvm_new_object(vm, hs) : 0;
      union dvm_value ignored = { 0 };
      if (!set || (init && !dvm_call(vm, init, set, NULL, 0, &ignored))) return false;
      for (uint32_t i = 0; l && i < l->size; ++i) {
         union dvm_value item = { .l = l->items[i] };
         if (!am_vcall(vm, set, "add", "(Ljava/lang/Object;)Z", &item, 1, &ignored)) return false;
      }
      dvm_pin(vm, set);
      RETL(set);
   }
   dvm_ref list = stream_new_list(vm);
   for (uint32_t i = 0; list && l && i < l->size; ++i) stream_list_add(vm, list, l->items[i]);
   dvm_pin(vm, list);
   RETL(list);
}

/* Stream.of(T) / of(T...) / empty() / concat(a, b); Arrays.stream(T[]...). */
static bool stream_static(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                          int nargs, union dvm_value *out)
{
   (void)self;
   const char *name = builtin_name();
   if (!strcmp(name, "concat")) {
      dvm_ref list = stream_new_list(vm);
      if (!list || !stream_drain(vm, ARG(0).l, list) || !stream_drain(vm, ARG(1).l, list))
         return false;
      RETL(stream_wrap(vm, list));
   }
   if (!strcmp(name, "empty")) RETL(stream_wrap(vm, stream_new_list(vm)));
   struct dvm_object *a = nargs > 0 && ARG(0).l ? dvm__obj(vm, ARG(0).l) : NULL;
   bool array = a && a->kind == DVM_OBJ_ARRAY && builtin_sig() && strstr(builtin_sig(), "([");
   if (array)
      RETL(stream_from_array(vm, ARG(0).l, nargs > 2 ? ARG(1).i : 0, nargs > 2 ? ARG(2).i : -1));
   dvm_ref list = stream_new_list(vm);
   if (list && nargs > 0) stream_list_add(vm, list, ARG(0).l);
   RETL(stream_wrap(vm, list));
}

static bool stream_is_parallel(struct dvm *vm, dvm_ref self, const union dvm_value *args,
                               int nargs, union dvm_value *out)
{
   (void)vm; (void)self; (void)args; (void)nargs;
   RETI(0);
}

static const struct rt_method rt_list_stream[] = {
   M("filter", "(Ljava/util/function/Predicate;)Ljava/util/stream/Stream;", stream_transform),
   M("map", "(Ljava/util/function/Function;)Ljava/util/stream/Stream;", stream_transform),
   M("flatMap", "(Ljava/util/function/Function;)Ljava/util/stream/Stream;", stream_transform),
   M("peek", "(Ljava/util/function/Consumer;)Ljava/util/stream/Stream;", stream_transform),
   M("distinct", "()Ljava/util/stream/Stream;", stream_slice),
   M("limit", "(J)Ljava/util/stream/Stream;", stream_slice),
   M("skip", "(J)Ljava/util/stream/Stream;", stream_slice),
   M("sorted", "()Ljava/util/stream/Stream;", stream_sorted),
   M("sorted", "(Ljava/util/Comparator;)Ljava/util/stream/Stream;", stream_sorted),
   M("forEach", "(Ljava/util/function/Consumer;)V", stream_for_each),
   M("forEachOrdered", "(Ljava/util/function/Consumer;)V", stream_for_each),
   M("anyMatch", "(Ljava/util/function/Predicate;)Z", stream_match),
   M("allMatch", "(Ljava/util/function/Predicate;)Z", stream_match),
   M("noneMatch", "(Ljava/util/function/Predicate;)Z", stream_match),
   M("count", "()J", stream_count),
   M("toArray", "()[Ljava/lang/Object;", stream_to_array),
   M("toArray", "(Ljava/util/function/IntFunction;)[Ljava/lang/Object;", stream_to_array),
   M("collect", "(Ljava/util/stream/Collector;)Ljava/lang/Object;", stream_collect),
   M("iterator", "()Ljava/util/Iterator;", stream_iterator),
   M("sequential", "()Ljava/util/stream/BaseStream;", stream_self),
   M("parallel", "()Ljava/util/stream/BaseStream;", stream_self),
   M("unordered", "()Ljava/util/stream/BaseStream;", stream_self),
   M("onClose", "(Ljava/lang/Runnable;)Ljava/util/stream/BaseStream;", stream_self),
   M("isParallel", "()Z", stream_is_parallel),
   M("close", "()V", empty_void),
   M_END
};
static const struct rt_method rt_stream_statics[] = {
   SM("of", "(Ljava/lang/Object;)Ljava/util/stream/Stream;", stream_static),
   SM("of", "([Ljava/lang/Object;)Ljava/util/stream/Stream;", stream_static),
   SM("empty", "()Ljava/util/stream/Stream;", stream_static),
   SM("concat", "(Ljava/util/stream/Stream;Ljava/util/stream/Stream;)Ljava/util/stream/Stream;",
      stream_static),
   M_END
};
static const struct rt_method rt_collectors[] = {
   SM("toList", "()Ljava/util/stream/Collector;", collectors_make),
   SM("toSet", "()Ljava/util/stream/Collector;", collectors_make),
   SM("joining", "()Ljava/util/stream/Collector;", collectors_make),
   SM("joining", "(Ljava/lang/CharSequence;)Ljava/util/stream/Collector;", collectors_make),
   SM("joining", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;Ljava/lang/CharSequence;)"
      "Ljava/util/stream/Collector;", collectors_make),
   M_END
};
