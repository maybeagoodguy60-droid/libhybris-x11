/*
 * glxshim - a client-side GLX implementation for lorie/termux-x11.
 *
 * lorie advertises a GLX extension that cannot reach the GPU (it registers
 * __glXDRISWRastProvider with a hardcoded config table; there is no real GLX
 * support). This shim is LD_PRELOADed as libGL.so.1 and answers the whole GLX
 * ABI in-process, backing every context with a real GLES 3.2 context on the
 * GPU via libhybris' EGL x11 platform (which drives DRI3 + Present).
 *
 * The app never talks to lorie's GLX. eglCreateWindowSurface(x11_window_xid)
 * makes libEGL call x11ws_CreateWindow, which builds the X11NativeWindow, so
 * presentation is handled entirely by the existing platform code.
 *
 * Stage 1 (M1): GLX ABI + core GL state. Desktop-GL calls are forwarded to
 * GLES where the ABI matches; the translation grows from here.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <link.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

/*
 * The chroot has no GL/gl.h or GL/glx.h (no mesa dev headers), so the GLX ABI
 * is declared here. All values below are frozen by the GLX 1.4 specification.
 */
typedef double GLdouble;   /* desktop-only type, absent from GLES headers */
typedef struct __GLXconfigRec  *GLXFBConfig;
typedef struct __GLXcontextRec *GLXContext;
typedef struct __GLXFBConfigRec *GLXFBConfigRec;
typedef XID  GLXDrawable;
typedef XID  GLXPixmap;
typedef XID  GLXPbuffer;
typedef struct __GLXPbufferConfigRec *GLXPbufferConfig;
typedef void (*__GLXextFuncPtr)(void);

#define GLX_USE_GL                       1
#define GLX_BUFFER_SIZE                  2
#define GLX_LEVEL                        3
#define GLX_RGBA                         4
#define GLX_DOUBLEBUFFER                 5
#define GLX_STEREO                       6
#define GLX_AUX_BUFFERS                  7
#define GLX_RED_SIZE                     8
#define GLX_GREEN_SIZE                   9
#define GLX_BLUE_SIZE                    10
#define GLX_ALPHA_SIZE                   11
#define GLX_DEPTH_SIZE                   12
#define GLX_STENCIL_SIZE                 13
#define GLX_CONFIG_CAVEAT                0x20
#define GLX_X_VISUAL_TYPE                0x22
#define GLX_TRANSPARENT_TYPE             0x23
#define GLX_TRANSPARENT_INDEX            0x24
#define GLX_TRANSPARENT_RED_VALUE        0x25
#define GLX_TRANSPARENT_GREEN_VALUE      0x26
#define GLX_TRANSPARENT_BLUE_VALUE       0x27
#define GLX_TRANSPARENT_ALPHA_VALUE      0x28
#define GLX_EXTENSIONS                   0x305F
#define GLX_VENDOR                       0x1F00
#define GLX_VERSION                      0x1F01
#define GLX_CLIENT_VENDOR                0x1F02
#define GLX_CLIENT_VERSION               0x1F03
#define GLX_SERVER_VENDOR                0x1F04
#define GLX_SERVER_VERSION               0x1F05
#define GLX_DRAWABLE_TYPE                0x8010
#define GLX_RENDER_TYPE                  0x8011
#define GLX_X_RENDERABLE                 0x8012
#define GLX_FBCONFIG_ID                  0x8013
#define GLX_RGBA_TYPE                    0x8014
#define GLX_WINDOW_TYPE                  0x8015
#define GLX_PBUFFER_TYPE                 0x8016
#define GLX_MAX_PBUFFER_WIDTH            0x8017
#define GLX_MAX_PBUFFER_HEIGHT           0x8018
#define GLX_MAX_PBUFFER_PIXELS           0x8019
#define GLX_OPTIMAL_PBUFFER_WIDTH        0x801A
#define GLX_OPTIMAL_PBUFFER_HEIGHT       0x801B
#define GLX_X_VISUAL_ID                  0x21
#define GLX_VISUAL_ID                    0x800B
#define GLX_SWAP_INTERVAL_EXT            0x20F1
#define GLX_SWAP_METHOD                  0x80F0
#define GLX_RGBA_BIT                     0x00000001
#define GLX_WINDOW_BIT                   0x00000001
#define GLX_PIXMAP_BIT                   0x00000002
#define GLX_PBUFFER_BIT                  0x00000004
#define GLX_TRUE_COLOR                   0x8002
#define GLX_DIRECT_COLOR                 0x8003
#define GLX_NONE                         0x8000
#define GLX_DONT_CARE                    0x8000
#define GLX_SWAP_COPY                    0x00000000
#define GLX_SAMPLES                      0x186A1
#define GLX_SAMPLE_BUFFERS               0x186A2
#define GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB 0x20B2
#define GLX_CONTEXT_PROFILE_MASK         0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT     0x00000001
#define GLX_BAD_SCREEN                   1
#define GLX_BAD_ATTRIBUTE                2
#define GLX_BAD_VISUAL                   4
#define GL_CONTEXT_PROFILE_MASK         0x9126
#define GL_CONTEXT_CORE_PROFILE_BIT     0x00000001

#ifndef EGL_NO_CONFIG_KHR
#define EGL_NO_CONFIG_KHR ((EGLConfig)0)
#endif

/* ------------------------------------------------------------------ */
/* dynamic loading: we must not link libEGL/libGLESv2 at build time,   */
/* because we are pretending to BE libGL.                              */
/* ------------------------------------------------------------------ */

static void *egl_so, *x11_so, *gles_so;

static void *sym(void *h, const char *n) { return dlsym(h, n); }

/* var is p_eglFoo; the real EGL symbol is eglFoo, so skip the 2-char prefix */
#define LOADSYM(h, var) do { *(void **)(&var) = sym(h, #var + 2); } while (0)

/* GLES entry points (resolved via eglGetProcAddress) */
typedef void *(*PFNGLEGPROC)(const char *);
static PFNGLEGPROC                    eglGetProcAddress_f;
static EGLDisplay (*p_eglGetDisplay)(void *);
static EGLBoolean (*p_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLBoolean (*p_eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
static EGLSurface  (*p_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
static EGLSurface  (*p_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
static EGLContext  (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLBoolean  (*p_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean  (*p_eglDestroyContext)(EGLDisplay, EGLContext);
static EGLBoolean  (*p_eglDestroySurface)(EGLDisplay, EGLSurface);
static EGLBoolean  (*p_eglSwapBuffers)(EGLDisplay, EGLSurface);
static EGLBoolean  (*p_eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
static EGLint      (*p_eglGetError)(void);
static EGLSurface  (*p_eglGetCurrentSurface)(EGLint);

/* GLES entry points reached via eglGetProcAddress. The exported desktop-GL
 * wrappers call these; where the desktop and GLES ABIs differ the wrapper
 * adapts (notably glClearDepth: GLdouble in desktop GL, GLfloat in GLES). */
#define GLES_FNS \
    FV(GLenum, pfGetError,            glGetError,                (void)) \
    FV(GLuint, pfCreateShader,          glCreateShader,            (GLenum)) \
    FV(GLuint, pfCreateProgram,         glCreateProgram,           (void)) \
    FV(GLint, pfGetAttribLocation,     glGetAttribLocation,       (GLuint, const GLchar *)) \
    FV(GLint, pfGetUniformLocation,    glGetUniformLocation,      (GLuint, const GLchar *)) \
    FV(void,  pfClear,                 glClear,                   (GLbitfield)) \
    FV(void,  pfClearColor,            glClearColor,              (GLfloat, GLfloat, GLfloat, GLfloat)) \
    FV(void,  pfClearDepth,            glClearDepth,              (GLfloat)) \
    FV(void,  pfViewport,              glViewport,                (GLint, GLint, GLsizei, GLsizei)) \
    FV(void,  pfScissor,               glScissor,                 (GLint, GLint, GLsizei, GLsizei)) \
    FV(void,  pfEnable,                glEnable,                  (GLenum)) \
    FV(void,  pfDisable,               glDisable,                 (GLenum)) \
    FV(void,  pfBlendFunc,             glBlendFunc,               (GLenum, GLenum)) \
    FV(void,  pfDepthFunc,             glDepthFunc,               (GLenum)) \
    FV(void,  pfDepthMask,             glDepthMask,               (GLboolean)) \
    FV(void,  pfColorMask,             glColorMask,               (GLboolean, GLboolean, GLboolean, GLboolean)) \
    FV(void,  pfCullFace,              glCullFace,                (GLenum)) \
    FV(void,  pfFinish,                glFinish,                  (void)) \
    FV(void,  pfFlush,                 glFlush,                   (void)) \
    FV(void,  pfDrawArrays,            glDrawArrays,              (GLenum, GLint, GLsizei)) \
    FV(void,  pfDrawElements,          glDrawElements,            (GLenum, GLsizei, GLenum, const void *)) \
    FV(void,  pfLineWidth,             glLineWidth,               (GLfloat)) \
    FV(void,  pfPointSize,             glPointSize,               (GLfloat)) \
    FV(void,  pfPixelStorei,           glPixelStorei,             (GLenum, GLint)) \
    FV(void,  pfReadPixels,            glReadPixels,              (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    FV(void,  pfGetIntegerv,           glGetIntegerv,             (GLenum, GLint *)) \
    FV(const GLubyte *, pfGetString, glGetString,               (GLenum)) \
    FV(void,  pfGetFloatv,             glGetFloatv,               (GLenum, GLfloat *)) \
    FV(void,  pfGetBooleanv,           glGetBooleanv,             (GLenum, GLboolean *)) \
    FV(void,  pfGenBuffers,            glGenBuffers,              (GLsizei, GLuint *)) \
    FV(void,  pfDeleteBuffers,         glDeleteBuffers,           (GLsizei, const GLuint *)) \
    FV(void,  pfBindBuffer,            glBindBuffer,              (GLenum, GLuint)) \
    FV(void,  pfBufferData,            glBufferData,              (GLenum, GLsizeiptr, const void *, GLenum)) \
    FV(void,  pfBufferSubData,         glBufferSubData,           (GLenum, GLintptr, GLsizeiptr, const void *)) \
    FV(void,  pfShaderSource,          glShaderSource,            (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    FV(void,  pfCompileShader,         glCompileShader,           (GLuint)) \
    FV(void,  pfGetShaderiv,           glGetShaderiv,             (GLuint, GLenum, GLint *)) \
    FV(void,  pfGetShaderInfoLog,      glGetShaderInfoLog,        (GLuint, GLsizei, GLsizei *, GLchar *)) \
    FV(void,  pfDeleteShader,          glDeleteShader,            (GLuint)) \
    FV(void,  pfAttachShader,          glAttachShader,            (GLuint, GLuint)) \
    FV(void,  pfLinkProgram,           glLinkProgram,             (GLuint)) \
    FV(void,  pfGetProgramiv,          glGetProgramiv,            (GLuint, GLenum, GLint *)) \
    FV(void,  pfGetProgramInfoLog,     glGetProgramInfoLog,       (GLuint, GLsizei, GLsizei *, GLchar *)) \
    FV(void,  pfUseProgram,            glUseProgram,              (GLuint)) \
    FV(void,  pfDeleteProgram,         glDeleteProgram,           (GLuint)) \
    FV(void,  pfUniform1i,             glUniform1i,               (GLint, GLint)) \
    FV(void,  pfUniform1f,             glUniform1f,               (GLint, GLfloat)) \
    FV(void,  pfUniform2f,             glUniform2f,               (GLint, GLfloat, GLfloat)) \
    FV(void,  pfUniform3f,             glUniform3f,               (GLint, GLfloat, GLfloat, GLfloat)) \
    FV(void,  pfUniform4f,             glUniform4f,               (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    FV(void,  pfUniformMatrix4fv,      glUniformMatrix4fv,        (GLint, GLsizei, GLboolean, const GLfloat *)) \
    FV(void,  pfVertexAttribPointer,   glVertexAttribPointer,     (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    FV(void,  pfEnableVertexAttribArray, glEnableVertexAttribArray, (GLuint)) \
    FV(void,  pfDisableVertexAttribArray, glDisableVertexAttribArray, (GLuint)) \
    FV(void,  pfGenVertexArrays,       glGenVertexArrays,         (GLsizei, GLuint *)) \
    FV(void,  pfBindVertexArray,       glBindVertexArray,         (GLuint)) \
    FV(void,  pfDeleteVertexArrays,    glDeleteVertexArrays,      (GLsizei, const GLuint *))

#define FV(ret, var, name, args) \
    typedef ret (*PFN_##var) args; \
    static PFN_##var var;
GLES_FNS
#undef FV

/* ------------------------------------------------------------------ */
/* GLX object model                                                    */
/* ------------------------------------------------------------------ */

#define MAXCFGS 4
typedef struct {
    int  used;
    int  id;
    int  visual_id;
    int  doublebuffer;
    int  rgba_sizes[4];
    int  depth, stencil, samples, sample_buffers;
} ShimFBConfig;

typedef Window   GLXWindow;
typedef Window   GLXPbuffer;
typedef Window   GLXPixmap;

typedef struct {
    int   magic;
    EGLContext ctx;
    Display *dpy;
    EGLSurface surf;
    GLXDrawable surf_draw;
} ShimContext;

#define CTX_MAGIC 0x676c7831
static ShimFBConfig cfgs[MAXCFGS];
static GLXFBConfig  cfg_handles[MAXCFGS];
static Display     *x_display;
static XVisualInfo *x_visual;
static EGLDisplay   egl_dpy;
static int          egl_ready;
static int          swap_interval = 1;

#define HYBRIS_DIR      "/usr/lib/hybris"
#define HYBRIS_SHIMS    "/usr/lib/hybris/gl-shims"

/* libhybris reads these itself at runtime, so setenv() is enough for them.
 * LD_LIBRARY_PATH is deliberately not relied on: glibc caches the dlopen
 * search path at process start, so changing it here cannot redirect dlopen. */
static void load_gles(void);

/* A GLX client is asking for an X11 drawable, and the shim only ever builds
 * windows out of X11 window ids, so the X11 platform is the only one that can
 * serve it. An ambient HYBRIS_EGLPLATFORM=wayland -- which the Termux session
 * exports for its own EGL clients -- would otherwise send libhybris into the
 * Wayland platform, where it aborts with "failed to connect to the server".
 * GLXSHIM_EGLPLATFORM overrides this for debugging. */
static void ensure_env(void)
{
    const char *p = getenv("GLXSHIM_EGLPLATFORM");
    setenv("HYBRIS_EGLPLATFORM", (p && *p) ? p : "x11", 1);
    if (getenv("HYBRIS_LD_LIBRARY_PATH") == NULL)
        setenv("HYBRIS_LD_LIBRARY_PATH",
               "/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:"
               "/system/lib64:/apex/com.android.runtime/lib64", 1);
}

/* Prefer the hybris libraries by absolute path: the system ld.so cache points
 * libEGL.so.1 at Mesa, and libGLESv2.so.2 at the desktop GLES build. */
static void *open_lib(const char *env_name, const char *hybris_path, const char *soname)
{
    const char *override = getenv(env_name);
    if (override && *override)
        return dlopen(override, RTLD_NOW | RTLD_GLOBAL);
    void *h = dlopen(hybris_path, RTLD_NOW | RTLD_GLOBAL);
    if (!h)
        h = dlopen(soname, RTLD_NOW | RTLD_GLOBAL);
    return h;
}

static int dbg(void){ static int v=-1; if(v<0){ const char*e=getenv("GLXSHIM_DEBUG"); v = e&&*e=='1'; } return v; }

static void shim_init(void)
{
    if (egl_ready) return;
    ensure_env();
    egl_so  = open_lib("GLXSHIM_EGL",   HYBRIS_DIR "/libEGL.so.1",   "libEGL.so.1");
    x11_so  = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
    gles_so = open_lib("GLXSHIM_GLES",  HYBRIS_SHIMS "/libGLESv2.so.2", "libGLESv2.so.2");
    if (!egl_so) { fprintf(stderr, "glxshim: no libEGL: %s\n", dlerror()); return; }

    LOADSYM(egl_so, p_eglGetDisplay);
    LOADSYM(egl_so, p_eglInitialize);
    LOADSYM(egl_so, p_eglChooseConfig);
    LOADSYM(egl_so, p_eglGetConfigAttrib);
    LOADSYM(egl_so, p_eglCreateWindowSurface);
    LOADSYM(egl_so, p_eglCreatePbufferSurface);
    LOADSYM(egl_so, p_eglCreateContext);
    LOADSYM(egl_so, p_eglMakeCurrent);
    LOADSYM(egl_so, p_eglDestroyContext);
    LOADSYM(egl_so, p_eglDestroySurface);
    LOADSYM(egl_so, p_eglSwapBuffers);
    LOADSYM(egl_so, p_eglQuerySurface);
    LOADSYM(egl_so, p_eglGetError);
    p_eglGetCurrentSurface = (EGLSurface (*)(EGLint))sym(egl_so, "eglGetCurrentSurface");
    eglGetProcAddress_f = (PFNGLEGPROC)sym(egl_so, "eglGetProcAddress");

    x_display = XOpenDisplay(NULL);
    if (x_display) {
        XVisualInfo tpl; memset(&tpl, 0, sizeof tpl);
        tpl.screen = DefaultScreen(x_display);
        tpl.depth = 24; tpl.class = TrueColor;
        int n = 0;
        XVisualInfo *v = XGetVisualInfo(x_display, VisualScreenMask | VisualDepthMask | VisualClassMask, &tpl, &n);
        if (v && n > 0) x_visual = v; /* kept alive for process lifetime */
    }

    egl_dpy = p_eglGetDisplay ? p_eglGetDisplay((void *)0x0) : EGL_NO_DISPLAY;
    if (dbg()) fprintf(stderr, "[shim] eglGetDisplay -> %p\n", (void*)egl_dpy);
    if (egl_dpy != EGL_NO_DISPLAY && p_eglInitialize) {
        EGLint ma=0, mi=0;
        EGLBoolean ok = p_eglInitialize(egl_dpy, &ma, &mi);
        if (dbg()) fprintf(stderr, "[shim] eglInitialize -> %d (%d.%d) err=0x%x\n", ok, ma, mi,
                           p_eglGetError ? p_eglGetError() : 0);
    }
    if (dbg()) fprintf(stderr, "[shim] visual=0x%x dpy=%p eglInitialize=%p eglChooseConfig=%p eglCreateWindowSurface=%p\n",
                       x_visual ? (unsigned)XVisualIDFromVisual(x_visual->visual) : 0,
                       (void*)x_display, (void*)p_eglInitialize, (void*)p_eglChooseConfig,
                       (void*)p_eglCreateWindowSurface);

    for (int i = 0; i < MAXCFGS; i++) {
        cfgs[i].used = 1;
        cfgs[i].id = i + 1;
        cfgs[i].visual_id = x_visual ? (int)XVisualIDFromVisual(x_visual->visual) : 0;
        cfgs[i].doublebuffer = 1;
        cfgs[i].rgba_sizes[0] = cfgs[i].rgba_sizes[1] = cfgs[i].rgba_sizes[2] = 8;
        cfgs[i].rgba_sizes[3] = 8;
        cfgs[i].depth = 24; cfgs[i].stencil = 8;
        cfgs[i].samples = 0; cfgs[i].sample_buffers = 0;
        cfg_handles[i] = (GLXFBConfig)&cfgs[i];
    }
    egl_ready = 1;
}

static ShimFBConfig *cfg_of(GLXFBConfig c) { return (ShimFBConfig *)c; }

static int cfg_int(ShimFBConfig *c, int attr)
{
    switch (attr) {
    case GLX_FBCONFIG_ID:          return c->id;
    case GLX_X_VISUAL_ID:          return c->visual_id;
    case GLX_VISUAL_ID:            return c->visual_id;
    case GLX_DOUBLEBUFFER:         return c->doublebuffer;
    case GLX_RED_SIZE:             return c->rgba_sizes[0];
    case GLX_GREEN_SIZE:           return c->rgba_sizes[1];
    case GLX_BLUE_SIZE:            return c->rgba_sizes[2];
    case GLX_ALPHA_SIZE:           return c->rgba_sizes[3];
    case GLX_BUFFER_SIZE:          return 32;
    case GLX_DEPTH_SIZE:           return c->depth;
    case GLX_STENCIL_SIZE:         return c->stencil;
    case GLX_SAMPLES:              return c->samples;
    case GLX_SAMPLE_BUFFERS:       return c->sample_buffers;
    case GLX_X_RENDERABLE:         return True;
    case GLX_RENDER_TYPE:          return GLX_RGBA_BIT;
    case GLX_DRAWABLE_TYPE:        return GLX_WINDOW_BIT | GLX_PBUFFER_BIT;
    case GLX_X_VISUAL_TYPE:        return GLX_TRUE_COLOR;
    case GLX_CONFIG_CAVEAT:        return GLX_NONE;
    case GLX_LEVEL:                return 0;
    case GLX_AUX_BUFFERS:          return 0;
    case GLX_STEREO:               return False;
    case GLX_TRANSPARENT_TYPE:     return GLX_NONE;
    case GLX_TRANSPARENT_INDEX:   return 0;
    case GLX_TRANSPARENT_RED_VALUE:   return 0;
    case GLX_TRANSPARENT_GREEN_VALUE: return 0;
    case GLX_TRANSPARENT_BLUE_VALUE:  return 0;
    case GLX_TRANSPARENT_ALPHA_VALUE: return 0;
    case GLX_MAX_PBUFFER_WIDTH:    return 16384;
    case GLX_MAX_PBUFFER_HEIGHT:   return 16384;
    case GLX_MAX_PBUFFER_PIXELS:   return 16384 * 16384;
    case GLX_SWAP_METHOD:          return GLX_SWAP_COPY;
    case GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB: return True;
    default:                       return 0;
    }
}

static int match_cfg(const int *attrs)
{
    ShimFBConfig *c = &cfgs[0];
    for (const int *a = attrs; a && a[0] != None; a += 2) {
        if (a[0] == GLX_RENDER_TYPE && a[1] != GLX_RGBA_BIT) return 0;
        if (a[0] == GLX_DRAWABLE_TYPE && !(a[1] & GLX_WINDOW_BIT)) return 0;
        if (a[0] == GLX_X_VISUAL_TYPE && a[1] != GLX_TRUE_COLOR) return 0;
        if (a[0] == GLX_DOUBLEBUFFER && c->doublebuffer != a[1]) return 0;
        if (a[0] == GLX_RED_SIZE   && c->rgba_sizes[0] < a[1]) return 0;
        if (a[0] == GLX_GREEN_SIZE && c->rgba_sizes[1] < a[1]) return 0;
        if (a[0] == GLX_BLUE_SIZE  && c->rgba_sizes[2] < a[1]) return 0;
        if (a[0] == GLX_ALPHA_SIZE && a[1] > 0 && c->rgba_sizes[3] < a[1]) return 0;
        if (a[0] == GLX_DEPTH_SIZE   && c->depth < a[1]) return 0;
        if (a[0] == GLX_STENCIL_SIZE && c->stencil < a[1]) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* GLX entry points                                                    */
/* ------------------------------------------------------------------ */

static Display *disp_of(Display *d) { return d ? d : x_display; }

GLXContext glXCreateNewContext(Display *d, GLXFBConfig c, GLXContext share, int direct)
{
    (void)d; (void)c; (void)share; (void)direct;
    shim_init();
    ShimContext *s = calloc(1, sizeof *s);
    s->magic = CTX_MAGIC;
    s->dpy = disp_of(d);
    return (GLXContext)s;
}

GLXContext glXCreateContextAttribsARB(Display *d, GLXFBConfig c, GLXContext share, Bool direct, const int *attrs)
{
    (void)c; (void)share; (void)direct; (void)attrs;
    return glXCreateNewContext(d, c, share, direct);
}

GLXContext glXCreateContext(Display *d, XVisualInfo *v, GLXContext share, Bool direct)
{
    (void)v; (void)share; (void)direct;
    return glXCreateNewContext(d, NULL, share, direct);
}

void glXDestroyContext(Display *d, GLXContext ctx)
{
    (void)d;
    if (!ctx) return;
    ShimContext *s = (ShimContext *)ctx;
    if (s->magic != CTX_MAGIC) return;
    if (s->ctx && p_eglDestroyContext && egl_dpy != EGL_NO_DISPLAY)
        p_eglDestroyContext(egl_dpy, s->ctx);
    free(s);
}

static GLXContext cur_ctx;
static GLXDrawable cur_draw;
static Display    *cur_dpy;

static int make_current(Display *d, GLXDrawable draw, GLXDrawable rdraw, GLXContext ctx)
{
    (void)rdraw;
    shim_init();
    if (!ctx) {
        cur_ctx = NULL; cur_draw = 0; cur_dpy = NULL;
        if (p_eglMakeCurrent && egl_dpy != EGL_NO_DISPLAY)
            p_eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return True;
    }
    ShimContext *s = (ShimContext *)ctx;
    if (s->magic != CTX_MAGIC) return False;

    /* The app's window is created on *its* X connection, while libhybris
     * queries the geometry on its own. Without a round trip the server can
     * see the EGL GetGeometry before the app's XCreateWindow and reject the
     * window as a native window. */
    if (draw && d) XSync(d, False);

    EGLSurface surf = EGL_NO_SURFACE;
    if (s->surf != EGL_NO_SURFACE && s->surf_draw == draw) {
        surf = s->surf;                 /* already current for this drawable */
    } else if (draw) {
        /* draw is an X11 Window id; libEGL turns it into an X11NativeWindow. */
        const EGLint cfg_attr[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
            EGL_NONE
        };
        EGLConfig ec; EGLint n = 0;
        if (!p_eglChooseConfig(egl_dpy, cfg_attr, &ec, 1, &n) || n < 1) {
            fprintf(stderr, "glxshim: eglChooseConfig failed (0x%x)\n", p_eglGetError ? p_eglGetError() : 0);
            return False;
        }
        if (dbg()) fprintf(stderr, "[shim] eglCreateWindowSurface(dpy=%p, win=0x%lx, cfg=%p)\n", (void*)egl_dpy, (unsigned long)draw, (void*)ec);
        surf = p_eglCreateWindowSurface(egl_dpy, ec, (EGLNativeWindowType)(uintptr_t)draw, NULL);
        if (surf == EGL_NO_SURFACE) {
            fprintf(stderr, "glxshim: eglCreateWindowSurface failed (0x%x)\n", p_eglGetError ? p_eglGetError() : 0);
            return False;
        }
    } else {
        const EGLint pb[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
        EGLConfig ec; EGLint n = 0;
        p_eglChooseConfig(egl_dpy, NULL, &ec, 1, &n);
        if (n > 0) surf = p_eglCreatePbufferSurface(egl_dpy, ec, pb);
    }

    if (!s->ctx) {
        const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        s->ctx = p_eglCreateContext(egl_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
        if (s->ctx == EGL_NO_CONTEXT) {
            /* fall back to default config */
            s->ctx = p_eglCreateContext(egl_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, NULL);
        }
        if (s->ctx == EGL_NO_CONTEXT) {
            fprintf(stderr, "glxshim: eglCreateContext failed (0x%x)\n", p_eglGetError ? p_eglGetError() : 0);
            return False;
        }
    }
    if (dbg()) fprintf(stderr, "[shim] eglMakeCurrent(surf=%p ctx=%p)\n", (void*)surf, (void*)s->ctx);
    if (!p_eglMakeCurrent(egl_dpy, surf, surf, s->ctx)) return False;
    cur_ctx = ctx; cur_draw = draw; cur_dpy = d;
    return True;
}

Bool glXMakeCurrent(Display *d, GLXDrawable draw, GLXContext ctx) { return make_current(d, draw, draw, ctx); }
Bool glXMakeContextCurrent(Display *d, GLXDrawable draw, GLXDrawable rdraw, GLXContext ctx) { return make_current(d, draw, rdraw, ctx); }
int  glXIsDirect(Display *d, GLXContext c) { (void)d; (void)c; return True; }
int  glXQueryExtension(Display *d, int *e, int *ev) { (void)d; if (e) *e = 1; if (ev) *ev = 1; return True; }
int  glXQueryVersion(Display *d, int *maj, int *min) { (void)d; if (dbg()) fprintf(stderr, "[shim] glXQueryVersion\n"); if (maj) *maj = 1; if (min) *min = 4; return True; }
void glXDestroyWindow(Display *d, Window w) { (void)d; (void)w; }

/* Classic GLX 1.1/1.2 surface used by directly linked apps (glxgears, glxinfo).
 * We advertise exactly one visual, so this is a thin wrapper around it. */
XVisualInfo *glXChooseVisual(Display *d, int screen, int *attribList)
{
    (void)screen;
    shim_init();
    if (!x_visual) { fprintf(stderr, "glxshim: glXChooseVisual: no x_visual\n"); return NULL; }
    for (const int *a = attribList; a && a[0] != None; a += 2) {
        if (dbg()) fprintf(stderr, "[shim] glXChooseVisual attr 0x%x = %d\n", a[0], a[1]);
        switch (a[0]) {
        case GLX_RENDER_TYPE:
            if (a[1] != GLX_RGBA_BIT) { if (dbg()) fprintf(stderr, "[shim]   reject RENDER_TYPE\n"); return NULL; }
            break;
        case GLX_DRAWABLE_TYPE:
            if (!(a[1] & GLX_WINDOW_BIT)) { if (dbg()) fprintf(stderr, "[shim]   reject DRAWABLE_TYPE\n"); return NULL; }
            break;
        case GLX_X_VISUAL_TYPE:
            if (a[1] != GLX_TRUE_COLOR) { if (dbg()) fprintf(stderr, "[shim]   reject VISUAL_TYPE\n"); return NULL; }
            break;
        case GLX_RED_SIZE: case GLX_GREEN_SIZE: case GLX_BLUE_SIZE:
            if (a[1] > 8) { if (dbg()) fprintf(stderr, "[shim]   reject COLOR_SIZE\n"); return NULL; }
            break;
        case GLX_DEPTH_SIZE:
            if (a[1] > 24) { if (dbg()) fprintf(stderr, "[shim]   reject DEPTH_SIZE\n"); return NULL; }
            break;
        case GLX_DOUBLEBUFFER:
            if (a[1] != True) { if (dbg()) fprintf(stderr, "[shim]   reject DOUBLEBUFFER\n"); return NULL; }
            break;
        case GLX_STEREO:
            if (a[1] != False) return NULL;
            break;
        default: break;
        }
    }
    XVisualInfo *v = (XVisualInfo *)malloc(sizeof *v);
    if (v) *v = *x_visual;
    (void)d;
    return v;
}

/* GLX 1.3 window objects: our drawables are plain X11 windows */
GLXWindow glXCreateWindow(Display *d, GLXFBConfig c, Window win, const int *attrs)
{
    (void)d; (void)c; (void)attrs;
    return (GLXWindow)win;
}

int glXUseXFont(Font f, int a, int b, int c, int d)
{
    (void)f; (void)a; (void)b; (void)c; (void)d;
    return True;                 /* no server-side fonts; harmless to accept */
}

int glXGetSwapIntervalMESA(void) { return swap_interval < 0 ? 0 : swap_interval; }
int glXSwapIntervalMESA(unsigned int i) { swap_interval = (int)i; return 0; }
GLXContext glXGetCurrentContext(void) { return cur_ctx; }
GLXDrawable glXGetCurrentDrawable(void) { return cur_draw; }
GLXDrawable glXGetCurrentReadDrawable(void) { return cur_draw; }
void glXWaitGL(void) { load_gles(); if (pfFinish) pfFinish(); }
void glXWaitX(void) { }
void glXWaitForSbc(Display *d) { (void)d; }
int  glXGetError(void) { return 0; }
void glXSetError(XErrorHandler h) { (void)h; }
Display *glXGetCurrentDisplay(void) { return cur_dpy ? cur_dpy : x_display; }
int  glXQueryDrawable(Display *d, GLXDrawable dr, int attr, unsigned int *v)
{
    (void)d; (void)dr;
    if (!v) return GLX_BAD_ATTRIBUTE;
    switch (attr) {
    case GLX_SWAP_INTERVAL_EXT: *v = (unsigned int)(swap_interval < 0 ? 0 : swap_interval); return Success;
    case GLX_FBCONFIG_ID:       *v = (unsigned int)cfgs[0].id; return Success;
    default:                    *v = 0; return Success;
    }
}

int glXSwapIntervalEXT(Display *d, GLXDrawable dr, int interval)
{
    (void)d; (void)dr; swap_interval = interval; return 0;
}

void glXSwapBuffers(Display *d, GLXDrawable draw)
{
    (void)d;
    if (p_eglSwapBuffers && egl_dpy != EGL_NO_DISPLAY && draw) {
        EGLSurface s = p_eglGetCurrentSurface ? p_eglGetCurrentSurface(EGL_DRAW) : EGL_NO_SURFACE;
        if (s != EGL_NO_SURFACE) p_eglSwapBuffers(egl_dpy, s);
    }
}

/* fbconfig queries */
GLXFBConfig *glXChooseFBConfig(Display *d, int screen, const int *attrs, int *n)
{
    (void)screen;
    shim_init();
    if (dbg()) { fprintf(stderr, "[shim] glXChooseFBConfig(dpy=%p", (void*)d); for (const int*a=attrs;a&&a[0]!=None;a+=2) fprintf(stderr, " 0x%x=%d", a[0], a[1]); fprintf(stderr, ")\n"); }
    if (!match_cfg(attrs)) { if (dbg()) fprintf(stderr, "[shim]  -> NO MATCH\n"); if (n) *n = 0; return NULL; }
    if (dbg()) fprintf(stderr, "[shim]  -> matched, n=1\n");
    /* The array must be freeable: callers copy it and then XFree() it, which
     * is plain free(). Returning static storage corrupts the heap. */
    GLXFBConfig *arr = (GLXFBConfig *)malloc(sizeof(GLXFBConfig));
    if (!arr) { if (n) *n = 0; return NULL; }
    arr[0] = cfg_handles[0];
    if (n) *n = 1;
    return arr;
}

int glXGetFBConfigAttrib(Display *d, GLXFBConfig c, int attr, int *val)
{
    (void)d;
    ShimFBConfig *s = cfg_of(c);
    if (!s || !val) return GLX_BAD_ATTRIBUTE;
    *val = cfg_int(s, attr);
    if (dbg()) {
        Dl_info di; void *ra = __builtin_return_address(0);
        const char *who = "?", *sym = "?";
        if (dladdr(ra, &di)) { if (di.dli_fname) who = di.dli_fname; if (di.dli_sname) sym = di.dli_sname; }
        fprintf(stderr, "[shim] glXGetFBConfigAttrib(attr=0x%x/=%d) = %d   <- %s in %s\n",
                attr, attr, *val, sym, who);
    }
    return Success;
}

XVisualInfo *glXGetVisualFromFBConfig(Display *d, GLXFBConfig c)
{
    (void)d;
    if (!x_visual) return NULL;
    if (c) {
        XVisualInfo *v = malloc(sizeof *v);
        *v = *x_visual;
        return v;
    }
    return x_visual;
}

XVisualInfo *glXGetVisualFromWindow(Display *d, Window w) { (void)d; (void)w; return x_visual; }

const char *glXQueryExtensionsString(Display *d, int screen)
{
    (void)d; (void)screen;
    return
"GLX_ARB_create_context GLX_ARB_create_context_profile GLX_ARB_create_context_robustness "
"GLX_ARB_fbconfig_float GLX_ARB_framebuffer_sRGB GLX_ARB_get_proc_address "
"GLX_EXT_swap_control GLX_EXT_swap_control_tear GLX_SGI_swap_control "
"GLX_MESA_swap_control GLX_ARB_get_proc_address";
}

const char *glXQueryServerString(Display *d, int screen, int name)
{
    (void)d; (void)screen;
    switch (name) {
    case GLX_EXTENSIONS: return "";
    case GLX_VENDOR:     return "Termux X11";
    case GLX_VERSION:    return "1.4";
    default:             return "";
    }
}

const char *glXGetClientString(Display *d, int name)
{
    (void)d;
    switch (name) {
    case GLX_EXTENSIONS: return glXQueryExtensionsString(d, 0);
    case GLX_VENDOR:     return "glxshim";
    case GLX_VERSION:    return "1.4";
    default:             return "";
    }
}

const char *glXQueryServerString_legacy(Display *d, int s, int n) { return glXQueryServerString(d, s, n); }

int glXGetConfig(Display *d, XVisualInfo *v, int attr, int *val)
{
    (void)d; (void)v;
    if (!val) return GLX_BAD_ATTRIBUTE;
    *val = cfg_int(&cfgs[0], attr);
    return Success;
}

int glXGetConfig_legacy(Display *d, XVisualInfo *v, int a, int *val) { return glXGetConfig(d, v, a, val); }

/* legacy drawable creation: not needed for GLFW but keep links happy */
GLXPixmap glXCreateGLXPixmap(Display *d, XVisualInfo *v, Pixmap p) { (void)d; (void)v; (void)p; return 0; }
void glXDestroyGLXPixmap(Display *d, GLXPixmap p) { (void)d; (void)p; }
GLXPbuffer glXCreatePbuffer(Display *d, GLXPbufferConfig c, const int *attrs) { (void)d; (void)c; (void)attrs; return 0; }
void glXDestroyPbuffer(Display *d, GLXPbuffer p) { (void)d; (void)p; }
int  glXQueryContext(Display *d, GLXContext c, int attr, int *val) { (void)d; (void)c; (void)attr; if (val) *val = 0; return 0; }
int  glXQueryDrawable_legacy(Display *d, GLXDrawable dr, int a, unsigned int *v) { return glXQueryDrawable(d, dr, a, v); }

/* ------------------------------------------------------------------ */
/* GL entry points: forward to GLES                                   */
/* ------------------------------------------------------------------ */

static void load_gles(void)
{
    if (pfGetError || !eglGetProcAddress_f) return;
#define FV(ret, var, name, args) var = (PFN_##var)eglGetProcAddress_f(#name);
    GLES_FNS
#undef FV
}

#define FWD0(name) void gl##name(void) { load_gles(); if (pf##name) pf##name(); }

void glClear(GLbitfield m)            { load_gles(); if (pfClear) pfClear(m); }
void glClearColor(GLfloat r,GLfloat g,GLfloat b,GLfloat a){ load_gles(); if(pfClearColor) pfClearColor(r,g,b,a); }
void glClearDepth(GLdouble d){ load_gles(); if(pfClearDepth) pfClearDepth((GLfloat)d); }
void glViewport(GLint x,GLint y,GLsizei w,GLsizei h){ load_gles(); if(pfViewport) pfViewport(x,y,w,h); }
void glScissor(GLint x,GLint y,GLsizei w,GLsizei h){ load_gles(); if(pfScissor) pfScissor(x,y,w,h); }
void glEnable(GLenum c)                { load_gles(); if(pfEnable) pfEnable(c); }
void glDisable(GLenum c)               { load_gles(); if(pfDisable) pfDisable(c); }
void glBlendFunc(GLenum s,GLenum d)    { load_gles(); if(pfBlendFunc) pfBlendFunc(s,d); }
void glDepthFunc(GLenum f)             { load_gles(); if(pfDepthFunc) pfDepthFunc(f); }
void glDepthMask(GLboolean m)          { load_gles(); if(pfDepthMask) pfDepthMask(m); }
void glColorMask(GLboolean r,GLboolean g,GLboolean b,GLboolean a){ load_gles(); if(pfColorMask) pfColorMask(r,g,b,a); }
void glCullFace(GLenum m)              { load_gles(); if(pfCullFace) pfCullFace(m); }
void glLineWidth(GLfloat w)            { load_gles(); if(pfLineWidth) pfLineWidth(w); }
void glPointSize(GLfloat s)            { load_gles(); if(pfPointSize) pfPointSize(s); }
void glPixelStorei(GLenum p,GLint v)   { load_gles(); if(pfPixelStorei) pfPixelStorei(p,v); }
void glFinish(void)                    { load_gles(); if(pfFinish) pfFinish(); }
void glFlush(void)                     { load_gles(); if(pfFlush) pfFlush(); }
GLenum glGetError(void)                { load_gles(); return pfGetError ? pfGetError() : 0; }
void glDrawArrays(GLenum m,GLint f,GLsizei c){ load_gles(); if(pfDrawArrays) pfDrawArrays(m,f,c); }
void glDrawElements(GLenum m,GLsizei c,GLenum t,const void *i){ load_gles(); if(pfDrawElements) pfDrawElements(m,c,t,i); }
void glReadPixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum f,GLenum t,void *p){ load_gles(); if(pfReadPixels) pfReadPixels(x,y,w,h,f,t,p); }

void glGenBuffers(GLsizei n,GLuint *v) { load_gles(); if(pfGenBuffers) pfGenBuffers(n,v); }
void glDeleteBuffers(GLsizei n,const GLuint *v){ load_gles(); if(pfDeleteBuffers) pfDeleteBuffers(n,v); }
void glBindBuffer(GLenum t,GLuint b)   { load_gles(); if(pfBindBuffer) pfBindBuffer(t,b); }
void glBufferData(GLenum t,GLsizeiptr s,const void *d,GLenum u){ load_gles(); if(pfBufferData) pfBufferData(t,s,d,u); }
void glBufferSubData(GLenum t,GLintptr o,GLsizeiptr s,const void *d){ load_gles(); if(pfBufferSubData) pfBufferSubData(t,o,s,d); }

GLuint glCreateShader(GLenum t)        { load_gles(); return pfCreateShader ? pfCreateShader(t) : 0; }
void glShaderSource(GLuint s,GLsizei c,const GLchar *const *v,const GLint *l){ load_gles(); if(pfShaderSource) pfShaderSource(s,c,v,l); }
void glCompileShader(GLuint s)         { load_gles(); if(pfCompileShader) pfCompileShader(s); }
void glGetShaderiv(GLuint s,GLenum p,GLint *v){ load_gles(); if(pfGetShaderiv) pfGetShaderiv(s,p,v); }
void glGetShaderInfoLog(GLuint s,GLsizei m,GLsizei *l,GLchar *b){ load_gles(); if(pfGetShaderInfoLog) pfGetShaderInfoLog(s,m,l,b); }
void glDeleteShader(GLuint s)          { load_gles(); if(pfDeleteShader) pfDeleteShader(s); }
GLuint glCreateProgram(void)           { load_gles(); return pfCreateProgram ? pfCreateProgram() : 0; }
void glAttachShader(GLuint p,GLuint s) { load_gles(); if(pfAttachShader) pfAttachShader(p,s); }
void glLinkProgram(GLuint p)           { load_gles(); if(pfLinkProgram) pfLinkProgram(p); }
void glGetProgramiv(GLuint p,GLenum a,GLint *v){ load_gles(); if(pfGetProgramiv) pfGetProgramiv(p,a,v); }
void glGetProgramInfoLog(GLuint p,GLsizei m,GLsizei *l,GLchar *b){ load_gles(); if(pfGetProgramInfoLog) pfGetProgramInfoLog(p,m,l,b); }
void glUseProgram(GLuint p)            { load_gles(); if(pfUseProgram) pfUseProgram(p); }
void glDeleteProgram(GLuint p)         { load_gles(); if(pfDeleteProgram) pfDeleteProgram(p); }
GLint glGetAttribLocation(GLuint p,const GLchar *n){ load_gles(); return pfGetAttribLocation ? pfGetAttribLocation(p,n) : -1; }
GLint glGetUniformLocation(GLuint p,const GLchar *n){ load_gles(); return pfGetUniformLocation ? pfGetUniformLocation(p,n) : -1; }
void glUniform1i(GLint l,GLint a)      { load_gles(); if(pfUniform1i) pfUniform1i(l,a); }
void glUniform1f(GLint l,GLfloat a)    { load_gles(); if(pfUniform1f) pfUniform1f(l,a); }
void glUniform2f(GLint l,GLfloat a,GLfloat b){ load_gles(); if(pfUniform2f) pfUniform2f(l,a,b); }
void glUniform3f(GLint l,GLfloat a,GLfloat b,GLfloat c){ load_gles(); if(pfUniform3f) pfUniform3f(l,a,b,c); }
void glUniform4f(GLint l,GLfloat a,GLfloat b,GLfloat c,GLfloat d){ load_gles(); if(pfUniform4f) pfUniform4f(l,a,b,c,d); }
void glUniformMatrix4fv(GLint l,GLsizei t,GLboolean t2,const GLfloat *v){ load_gles(); if(pfUniformMatrix4fv) pfUniformMatrix4fv(l,t,t2,v); }
void glVertexAttribPointer(GLuint i,GLint s,GLenum t,GLboolean n,GLsizei st,const void *o){ load_gles(); if(pfVertexAttribPointer) pfVertexAttribPointer(i,s,t,n,st,o); }
void glEnableVertexAttribArray(GLuint i){ load_gles(); if(pfEnableVertexAttribArray) pfEnableVertexAttribArray(i); }
void glDisableVertexAttribArray(GLuint i){ load_gles(); if(pfDisableVertexAttribArray) pfDisableVertexAttribArray(i); }
void glGenVertexArrays(GLsizei n,GLuint *v){ load_gles(); if(pfGenVertexArrays) pfGenVertexArrays(n,v); }
void glBindVertexArray(GLuint a)      { load_gles(); if(pfBindVertexArray) pfBindVertexArray(a); }
void glDeleteVertexArrays(GLsizei n,const GLuint *v){ load_gles(); if(pfDeleteVertexArrays) pfDeleteVertexArrays(n,v); }

/* state we must not blindly forward: report our own identity */
void glGetIntegerv(GLenum pname, GLint *data)
{
    load_gles();
    switch (pname) {
    case GL_MAJOR_VERSION: if (data) *data = 3; return;
    case GL_MINOR_VERSION: if (data) *data = 0; return;
    case GL_CONTEXT_PROFILE_MASK: if (data) *data = GL_CONTEXT_CORE_PROFILE_BIT; return;
    default: if (pfGetIntegerv) pfGetIntegerv(pname, data); return;
    }
}
void glGetFloatv(GLenum p, GLfloat *d)  { load_gles(); if (pfGetFloatv) pfGetFloatv(p, d); }
void glGetBooleanv(GLenum p, GLboolean *d){ load_gles(); if (pfGetBooleanv) pfGetBooleanv(p, d); }

static char extstr[4096];

const GLubyte *glGetString(GLenum name)
{
    if (dbg()) fprintf(stderr, "[shim] glGetString(0x%x)\n", name);
    switch (name) {
    case GL_VERSION:    return (const GLubyte *)"3.0 (Core Profile) Mesa 26.2.2 glxshim";
    case GL_RENDERER:   return (const GLubyte *)"Mali-G57 MC2 (glxshim, GLES3 backend)";
    case GL_VENDOR:     return (const GLubyte *)"Termux X11 glxshim";
    case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"300 es";
    case GL_EXTENSIONS: {
        load_gles();
        const char *base = pfGetString ? (const char *)pfGetString(GL_EXTENSIONS) : NULL;
        snprintf(extstr, sizeof extstr,
                 "%s%s", base ? base : "",
                 " GL_EXT_framebuffer_object GL_EXT_texture_object"
                 " GL_OES_mapbuffer GL_OES_framebuffer_object"
                 " GL_ARB_texture_non_power_of_two GL_ARB_vertex_buffer_object"
                 " GL_EXT_blend_func_separate GL_EXT_blend_equation_separate");
        return (const GLubyte *)extstr;
    }
    default:            load_gles(); return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* glXGetProcAddress: our own symbols first, then GLES                 */
/* ------------------------------------------------------------------ */


/* OpenGL 1.x fixed-function entry points.
 *
 * GLES has no fixed-function state machine and this shim does not emulate
 * one, so these exist to keep dynamically linked desktop GL apps loading and
 * running: the calls are accepted and ignored. Immediate-mode geometry passed
 * to glBegin/glEnd/glVertex* is therefore *not* rasterised. */

void glLightfv(GLenum light, const GLfloat *params) { (void)light; (void)params; }
void glLightf(GLenum light, GLfloat param) { (void)light; (void)param; }
void glLighti(GLenum light, GLint param) { (void)light; (void)param; }
void glLightiv(GLenum light, const GLint *params) { (void)light; (void)params; }
void glLightModeli(GLenum pname, GLint param) { (void)pname; (void)param; }
void glLightModelfv(GLenum pname, const GLfloat *params) { (void)pname; (void)params; }
void glLightModelf(GLenum pname, GLfloat param) { (void)pname; (void)param; }
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) { (void)face; (void)pname; (void)params; }
void glMaterialf(GLenum face, GLenum pname, GLfloat param) { (void)face; (void)pname; (void)param; }
void glMateriali(GLenum face, GLenum pname, GLint param) { (void)face; (void)pname; (void)param; }
void glColorMaterial(GLenum face, GLenum mode) { (void)face; (void)mode; }
void glShadeModel(GLenum mode) { (void)mode; }
void glAlphaFunc(GLenum func, GLfloat ref) { (void)func; (void)ref; }
void glBegin(GLenum mode) { (void)mode; }
void glEnd(void) {  }
void glVertex2f(GLfloat x, GLfloat y) { (void)x; (void)y; }
void glVertex2fv(const GLfloat *v) { (void)v; }
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { (void)x; (void)y; (void)z; }
void glVertex3fv(const GLfloat *v) { (void)v; }
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { (void)x; (void)y; (void)z; (void)w; }
void glVertex4fv(const GLfloat *v) { (void)v; }
void glNormal3f(GLfloat x, GLfloat y, GLfloat z) { (void)x; (void)y; (void)z; }
void glNormal3fv(const GLfloat *v) { (void)v; }
void glColor3f(GLfloat r, GLfloat g, GLfloat b) { (void)r; (void)g; (void)b; }
void glColor3fv(const GLfloat *v) { (void)v; }
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { (void)r; (void)g; (void)b; (void)a; }
void glColor4fv(const GLfloat *v) { (void)v; }
void glColor3ub(GLubyte r, GLubyte g, GLubyte b) { (void)r; (void)g; (void)b; }
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { (void)r; (void)g; (void)b; (void)a; }
void glColor3d(GLdouble r, GLdouble g, GLdouble b) { (void)r; (void)g; (void)b; }
void glColor4d(GLdouble r, GLdouble g, GLdouble b, GLdouble a) { (void)r; (void)g; (void)b; (void)a; }
void glTexCoord2f(GLfloat s, GLfloat t) { (void)s; (void)t; }
void glTexCoord2fv(const GLfloat *v) { (void)v; }
void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) { (void)s; (void)t; (void)r; (void)q; }
void glMatrixMode(GLenum mode) { (void)mode; }
void glLoadIdentity(void) {  }
void glLoadMatrixf(const GLfloat *m) { (void)m; }
void glLoadMatrixd(const GLdouble *m) { (void)m; }
void glMultMatrixf(const GLfloat *m) { (void)m; }
void glMultMatrixd(const GLdouble *m) { (void)m; }
void glPushMatrix(void) {  }
void glPopMatrix(void) {  }
void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { (void)x; (void)y; (void)z; }
void glTranslated(GLdouble x, GLdouble y, GLdouble z) { (void)x; (void)y; (void)z; }
void glTranslatefv(const GLfloat *v) { (void)v; }
void glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) { (void)a; (void)x; (void)y; (void)z; }
void glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z) { (void)a; (void)x; (void)y; (void)z; }
void glScalef(GLfloat x, GLfloat y, GLfloat z) { (void)x; (void)y; (void)z; }
void glScaled(GLdouble x, GLdouble y, GLdouble z) { (void)x; (void)y; (void)z; }
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) { (void)l; (void)r; (void)b; (void)t; (void)n; (void)f; }
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) { (void)l; (void)r; (void)b; (void)t; (void)n; (void)f; }
void glPolygonMode(GLenum face, GLenum mode) { (void)face; (void)mode; }
void glDrawBuffer(GLenum mode) { (void)mode; }
void glReadBuffer(GLenum mode) { (void)mode; }

void glGetDoublev(GLenum pname, GLdouble *params)
{
    (void)pname;
    if (params) params[0] = 0.0;
}

GLdouble glGetDouble(GLenum pname) { (void)pname; return 0.0; }


/* Display lists: same situation as the fixed-function pipeline. Legacy apps
 * build geometry into display lists, so provide the entry points and hand out
 * opaque ids; no list contents are ever replayed. */

void glNewList(GLuint list, GLenum mode) { (void)list; (void)mode; }
void glEndList(void) {  }
GLuint glGenLists(GLsizei range) { (void)range; return 1; }
void glDeleteLists(GLuint list, GLsizei range) { (void)list; (void)range; }
GLboolean glIsList(GLuint list) { (void)list; return 0; }
void glListBase(GLuint base) { (void)base; }
void glCallList(GLuint list) { (void)list; }
void glCallLists(GLsizei n, GLenum type, const void *lists) { (void)n; (void)type; (void)lists; }
typedef struct { const char *name; void *addr; } ShimSym;

/* Desktop spellings -> what this GLES driver actually provides. */
static const char *const alias_map[][2] = {
    { "glMapBuffer",                "glMapBufferOES" },
    { "glMapBufferOES",             "glMapBufferOES" },
    { "glUnmapBufferOES",           "glUnmapBufferOES" },
    { "glGenFramebuffersEXT",       "glGenFramebuffers" },
    { "glDeleteFramebuffersEXT",    "glDeleteFramebuffers" },
    { "glBindFramebufferEXT",       "glBindFramebuffer" },
    { "glFramebufferTexture2DEXT",  "glFramebufferTexture2D" },
    { "glFramebufferRenderbufferEXT", "glFramebufferRenderbuffer" },
    { "glCheckFramebufferStatusEXT","glCheckFramebufferStatus" },
    { "glGenRenderbuffersEXT",      "glGenRenderbuffers" },
    { "glDeleteRenderbuffersEXT",   "glDeleteRenderbuffers" },
    { "glBindRenderbufferEXT",      "glBindRenderbuffer" },
    { "glRenderbufferStorageEXT",   "glRenderbufferStorage" },
    { "glGenerateMipmapEXT",        "glGenerateMipmap" },
    { "glActiveTextureEXT",         "glActiveTexture" },
    { "glBindTextureEXT",           "glBindTexture" },
    { NULL, NULL }
};
#define S(n) { "gl" #n, (void *)&gl##n }

static const ShimSym shim_syms[] = {
    { "glNewList", (void *)&glNewList },
    { "glEndList", (void *)&glEndList },
    { "glGenLists", (void *)&glGenLists },
    { "glDeleteLists", (void *)&glDeleteLists },
    { "glIsList", (void *)&glIsList },
    { "glListBase", (void *)&glListBase },
    { "glCallList", (void *)&glCallList },
    { "glCallLists", (void *)&glCallLists },
    { "glLightfv", (void *)&glLightfv },
    { "glLightf", (void *)&glLightf },
    { "glLighti", (void *)&glLighti },
    { "glLightiv", (void *)&glLightiv },
    { "glLightModeli", (void *)&glLightModeli },
    { "glLightModelfv", (void *)&glLightModelfv },
    { "glLightModelf", (void *)&glLightModelf },
    { "glMaterialfv", (void *)&glMaterialfv },
    { "glMaterialf", (void *)&glMaterialf },
    { "glMateriali", (void *)&glMateriali },
    { "glColorMaterial", (void *)&glColorMaterial },
    { "glShadeModel", (void *)&glShadeModel },
    { "glAlphaFunc", (void *)&glAlphaFunc },
    { "glBegin", (void *)&glBegin },
    { "glEnd", (void *)&glEnd },
    { "glVertex2f", (void *)&glVertex2f },
    { "glVertex2fv", (void *)&glVertex2fv },
    { "glVertex3f", (void *)&glVertex3f },
    { "glVertex3fv", (void *)&glVertex3fv },
    { "glVertex4f", (void *)&glVertex4f },
    { "glVertex4fv", (void *)&glVertex4fv },
    { "glNormal3f", (void *)&glNormal3f },
    { "glNormal3fv", (void *)&glNormal3fv },
    { "glColor3f", (void *)&glColor3f },
    { "glColor3fv", (void *)&glColor3fv },
    { "glColor4f", (void *)&glColor4f },
    { "glColor4fv", (void *)&glColor4fv },
    { "glColor3ub", (void *)&glColor3ub },
    { "glColor4ub", (void *)&glColor4ub },
    { "glColor3d", (void *)&glColor3d },
    { "glColor4d", (void *)&glColor4d },
    { "glTexCoord2f", (void *)&glTexCoord2f },
    { "glTexCoord2fv", (void *)&glTexCoord2fv },
    { "glTexCoord4f", (void *)&glTexCoord4f },
    { "glMatrixMode", (void *)&glMatrixMode },
    { "glLoadIdentity", (void *)&glLoadIdentity },
    { "glLoadMatrixf", (void *)&glLoadMatrixf },
    { "glLoadMatrixd", (void *)&glLoadMatrixd },
    { "glMultMatrixf", (void *)&glMultMatrixf },
    { "glMultMatrixd", (void *)&glMultMatrixd },
    { "glPushMatrix", (void *)&glPushMatrix },
    { "glPopMatrix", (void *)&glPopMatrix },
    { "glTranslatef", (void *)&glTranslatef },
    { "glTranslated", (void *)&glTranslated },
    { "glTranslatefv", (void *)&glTranslatefv },
    { "glRotatef", (void *)&glRotatef },
    { "glRotated", (void *)&glRotated },
    { "glScalef", (void *)&glScalef },
    { "glScaled", (void *)&glScaled },
    { "glOrtho", (void *)&glOrtho },
    { "glFrustum", (void *)&glFrustum },
    { "glPolygonMode", (void *)&glPolygonMode },
    { "glDrawBuffer", (void *)&glDrawBuffer },
    { "glReadBuffer", (void *)&glReadBuffer },
    { "glGetDoublev", (void *)&glGetDoublev },
    { "glGetDouble", (void *)&glGetDouble },
    { "glXChooseFBConfig", (void *)&glXChooseFBConfig },
    { "glXGetFBConfigAttrib", (void *)&glXGetFBConfigAttrib },
    { "glXGetVisualFromFBConfig", (void *)&glXGetVisualFromFBConfig },
    { "glXCreateNewContext", (void *)&glXCreateNewContext },
    { "glXCreateContextAttribsARB", (void *)&glXCreateContextAttribsARB },
    { "glXCreateContext", (void *)&glXCreateContext },
    { "glXDestroyContext", (void *)&glXDestroyContext },
    { "glXMakeCurrent", (void *)&glXMakeCurrent },
    { "glXMakeContextCurrent", (void *)&glXMakeContextCurrent },
    { "glXSwapBuffers", (void *)&glXSwapBuffers },
    { "glXSwapIntervalEXT", (void *)&glXSwapIntervalEXT },
    { "glXQueryExtension", (void *)&glXQueryExtension },
    { "glXQueryVersion", (void *)&glXQueryVersion },
    { "glXQueryExtensionsString", (void *)&glXQueryExtensionsString },
    { "glXQueryServerString", (void *)&glXQueryServerString },
    { "glXGetClientString", (void *)&glXGetClientString },
    { "glXGetConfig", (void *)&glXGetConfig },
    { "glXGetVisualFromWindow", (void *)&glXGetVisualFromWindow },
    { "glXIsDirect", (void *)&glXIsDirect },
    { "glXGetCurrentContext", (void *)&glXGetCurrentContext },
    { "glXGetCurrentDrawable", (void *)&glXGetCurrentDrawable },
    { "glXGetCurrentDisplay", (void *)&glXGetCurrentDisplay },
    { "glXWaitGL", (void *)&glXWaitGL },
    { "glXWaitX", (void *)&glXWaitX },
    { "glXGetError", (void *)&glXGetError },
    { "glXQueryDrawable", (void *)&glXQueryDrawable },
    { "glXCreateGLXPixmap", (void *)&glXCreateGLXPixmap },
    { "glXDestroyGLXPixmap", (void *)&glXDestroyGLXPixmap },
    { "glXCreatePbuffer", (void *)&glXCreatePbuffer },
    { "glXDestroyPbuffer", (void *)&glXDestroyPbuffer },
    { "glXQueryContext", (void *)&glXQueryContext },
    S(Clear), S(ClearColor), S(Viewport), S(Scissor),
    S(Enable), S(Disable), S(BlendFunc), S(DepthFunc), S(DepthMask),
    S(ColorMask), S(CullFace), S(Finish), S(Flush), S(DrawArrays),
    S(DrawElements), S(LineWidth), S(PointSize), S(PixelStorei),
    S(ReadPixels), S(GetIntegerv), S(GetFloatv), S(GetBooleanv),
    S(GetError), S(GetString),
    S(GenBuffers), S(DeleteBuffers), S(BindBuffer), S(BufferData),
    S(BufferSubData), S(CreateShader), S(ShaderSource), S(CompileShader),
    S(GetShaderiv), S(GetShaderInfoLog), S(DeleteShader), S(CreateProgram),
    S(AttachShader), S(LinkProgram), S(GetProgramiv), S(GetProgramInfoLog),
    S(UseProgram), S(DeleteProgram), S(GetAttribLocation),
    S(GetUniformLocation), S(Uniform1i), S(Uniform1f), S(Uniform2f),
    S(Uniform3f), S(Uniform4f), S(UniformMatrix4fv), S(VertexAttribPointer),
    S(EnableVertexAttribArray), S(DisableVertexAttribArray), S(GenVertexArrays),
    S(BindVertexArray), S(DeleteVertexArrays),
    { NULL, NULL }
};

__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *name)
{
    shim_init();
    load_gles();
    if (dbg() && strstr((const char *)name, "GetString"))
        fprintf(stderr, "[shim] glXGetProcAddress(\"%s\")\n", (const char *)name);
    for (const ShimSym *s = shim_syms; s->name; s++)
        if (strcmp(s->name, (const char *)name) == 0) return (__GLXextFuncPtr)s->addr;
    for (int i = 0; alias_map[i][0]; i++)
        if (strcmp(alias_map[i][0], (const char *)name) == 0) {
            if (eglGetProcAddress_f) {
                void *p = eglGetProcAddress_f(alias_map[i][1]);
                if (p) return (__GLXextFuncPtr)p;
            }
            break;
        }
    if (eglGetProcAddress_f) {
        void *p = eglGetProcAddress_f((const char *)name);
        if (p) return (__GLXextFuncPtr)p;
    }
    if (dbg()) fprintf(stderr, "[shim] proc UNRESOLVED: %s\n", (const char *)name);
    if (gles_so) {
        void *p = sym(gles_so, (const char *)name);
        if (p) return (__GLXextFuncPtr)p;
    }
    return NULL;
}

__GLXextFuncPtr glXGetProcAddress(const GLubyte *name) { return glXGetProcAddressARB(name); }
