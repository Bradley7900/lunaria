/* Every eglMakeCurrent in the process, recorded in a ring without printing.
 *
 * A context or surface current on one host thread makes eglMakeCurrent on
 * any other host thread fail with EGL_BAD_ACCESS, and the failure names no
 * culprit. The 4-core Unity startup hang (16-11) is exactly that: Unity's
 * first bind is refused, and printing each bind to find the holder shifted
 * the timing enough to hide it (6/6 clean traced, 3/8 hung untraced). So the
 * binds are only recorded here, and the ring is printed when a guest bind
 * fails: the last successful bind on another host thread that was never
 * undone is the holder.
 *
 * Include after <EGL/egl.h>. The function-like macro routes every call
 * through the recorder; `(eglMakeCurrent)(...)` reaches the real one. */
#ifndef LUNA_EGL_MC_H
#define LUNA_EGL_MC_H

#include <EGL/egl.h>

#ifdef __cplusplus
extern "C" {
#endif
EGLBoolean luna_egl_make_current_rec(EGLDisplay dpy, EGLSurface draw,
                                     EGLSurface read, EGLContext ctx,
                                     const char *file, int line);
void luna_egl_mc_dump(const char *why);
#ifdef __cplusplus
}
#endif

#define eglMakeCurrent(d, a, b, c) \
    luna_egl_make_current_rec((d), (a), (b), (c), __FILE__, __LINE__)

#endif
