/*
 * EGL for switch-sdl2's video driver, which the Vulkan build never starts:
 * SDL is only its audio output, and the display belongs to nv2a_vk. Linking
 * switch-mesa's real EGL would bring a second copy of Mesa next to NVK's.
 * Every entry fails, so SDL reports "no EGL" if anything ever asks.
 */
#include <stddef.h>
#include <stdint.h>

#define EGL_FAIL 0
#define EGL_NOT_INITIALIZED 0x3001

void *eglGetDisplay(void *d) { (void)d; return NULL; }
void *eglGetPlatformDisplay(unsigned p, void *d, const intptr_t *a) { (void)p; (void)d; (void)a; return NULL; }
unsigned eglInitialize(void *d, int *maj, int *min) { (void)d; (void)maj; (void)min; return EGL_FAIL; }
unsigned eglTerminate(void *d) { (void)d; return EGL_FAIL; }
int eglGetError(void) { return EGL_NOT_INITIALIZED; }
const char *eglQueryString(void *d, int n) { (void)d; (void)n; return NULL; }
void *eglGetProcAddress(const char *n) { (void)n; return NULL; }
unsigned eglBindAPI(unsigned api) { (void)api; return EGL_FAIL; }
unsigned eglQueryAPI(void) { return 0; }
unsigned eglChooseConfig(void *d, const int *a, void **c, int n, int *num)
{ (void)d; (void)a; (void)c; (void)n; if (num) *num = 0; return EGL_FAIL; }
unsigned eglGetConfigAttrib(void *d, void *c, int a, int *v) { (void)d; (void)c; (void)a; (void)v; return EGL_FAIL; }
void *eglCreateContext(void *d, void *c, void *s, const int *a) { (void)d; (void)c; (void)s; (void)a; return NULL; }
unsigned eglDestroyContext(void *d, void *c) { (void)d; (void)c; return EGL_FAIL; }
void *eglCreateWindowSurface(void *d, void *c, void *w, const int *a) { (void)d; (void)c; (void)w; (void)a; return NULL; }
void *eglCreatePbufferSurface(void *d, void *c, const int *a) { (void)d; (void)c; (void)a; return NULL; }
unsigned eglDestroySurface(void *d, void *s) { (void)d; (void)s; return EGL_FAIL; }
unsigned eglMakeCurrent(void *d, void *dr, void *rd, void *c) { (void)d; (void)dr; (void)rd; (void)c; return EGL_FAIL; }
unsigned eglSwapBuffers(void *d, void *s) { (void)d; (void)s; return EGL_FAIL; }
unsigned eglSwapInterval(void *d, int i) { (void)d; (void)i; return EGL_FAIL; }
unsigned eglWaitGL(void) { return EGL_FAIL; }
unsigned eglWaitNative(int e) { (void)e; return EGL_FAIL; }
