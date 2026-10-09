/* gl43_proof -- end-to-end proof that the glxshim 4.3-core front end really
 * executes desktop GLSL 4.30 compute and tessellation on the GLES 3.2 blob
 * (via GLSL version pinning), not just that the strings say "4.3".
 *
 *  1. make a 4.3 core context and confirm GL_VERSION / GLSL version
 *  2. compile+link+dispatch a #version 430 core compute shader over an SSBO,
 *     read the buffer back and validate the arithmetic
 *  3. compile+link a #version 430 core vs+tcs+tes+fs pipeline, draw GL_PATCHES
 *     and confirm no GL error and a non-empty framebuffer
 *
 * Exit 0 only if every check passes.
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#define None 0L
#define GLX_RGBA 4
#define GLX_RENDER_TYPE 0x8011
#define GLX_DOUBLEBUFFER 0x5A32
#define GLX_RED_SIZE 0x8005
#define GLX_GREEN_SIZE 0x8006
#define GLX_BLUE_SIZE 0x8007
#define GLX_DEPTH_SIZE 0x800A
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001

typedef struct __GLXcontextRec *GLXContext;
typedef struct __GLXFBConfigRec *GLXFBConfig;

/* GL enums we need */
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_VENDOR 0x1F00
#define GL_NO_ERROR 0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPUTE_SHADER 0x91B9
#define GL_TESS_CONTROL_SHADER 0x8E88
#define GL_TESS_EVALUATION_SHADER 0x8E87
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#define GL_ARRAY_BUFFER 0x8892
#define GL_DYNAMIC_READ 0x88E8
#define GL_MAP_READ_BIT 0x0001
#define GL_DISPATCH_INDIRECT_BUFFER 0x90EE
#define GL_ALL_BARRIER_BITS 0xFFFFFFFF
#define GL_SHADER_STORAGE_BARRIER_BIT 0x2000
#define GL_PATCHES 0x000E
#define GL_PATCH_VERTICES 0x8E72
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef long GLintptr;
typedef long GLsizeiptr;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;

/* resolved entry points */
static const GLubyte *(*pGetString)(GLenum);
static GLenum (*pGetError)(void);
static void (*pGetIntegerv)(GLenum, GLint *);
static GLuint (*pCreateShader)(GLenum);
static void (*pShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
static void (*pCompileShader)(GLuint);
static void (*pGetShaderiv)(GLuint, GLenum, GLint *);
static void (*pGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static GLuint (*pCreateProgram)(void);
static void (*pAttachShader)(GLuint, GLuint);
static void (*pLinkProgram)(GLuint);
static void (*pGetProgramiv)(GLuint, GLenum, GLint *);
static void (*pGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static void (*pUseProgram)(GLuint);
static void (*pGenBuffers)(GLsizei, GLuint *);
static void (*pBindBuffer)(GLenum, GLuint);
static void (*pBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void (*pBindBufferBase)(GLenum, GLuint, GLuint);
static void (*pDispatchCompute)(GLuint, GLuint, GLuint);
static void (*pMemoryBarrier)(unsigned int);
static void *(*pMapBufferRange)(GLenum, GLintptr, GLsizeiptr, unsigned int);
static GLboolean (*pUnmapBuffer)(GLenum);
static void (*pGenVertexArrays)(GLsizei, GLuint *);
static void (*pBindVertexArray)(GLuint);
static void (*pEnableVertexAttribArray)(GLuint);
static void (*pVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void (*pPatchParameteri)(GLenum, GLint);
static void (*pDrawArrays)(GLenum, GLint, GLsizei);
static void (*pClearColor)(float, float, float, float);
static void (*pClear)(unsigned int);
static void (*pViewport)(GLint, GLint, GLsizei, GLsizei);
static void (*pReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);

typedef void *(*GP)(const char *);

static int fails;

static void chk(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

static void *sym(GP g, const char *n)
{
    void *p = g ? g(n) : NULL;
    if (!p) { fprintf(stderr, "unresolved: %s\n", n); exit(2); }
    return p;
}

static void *sym_ld(const char *n)
{
    void *p = dlsym(RTLD_DEFAULT, n);
    if (!p) { fprintf(stderr, "unresolved(libGL): %s\n", n); exit(2); }
    return p;
}

static void linklog(GLuint p, const char *name)
{
    GLint len = 0; pGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
    char log[8192]; GLsizei got = 0;
    if (len > 0 && len < (GLint)sizeof log) pGetProgramInfoLog(p, len, &got, log);
    fprintf(stderr, "link %s failed:\n%s\n", name, log);
}

static GLuint compile(const char *name, GLenum type, const char *src)
{
    GLuint s = pCreateShader(type);
    pShaderSource(s, 1, &src, NULL);
    pCompileShader(s);
    GLint ok = 0;
    pGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0; pGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        char log[4096]; GLsizei got = 0;
        if (len > 0 && len < (GLint)sizeof log) pGetShaderInfoLog(s, len, &got, log);
        fprintf(stderr, "compile %s failed:\n%s\n", name, log);
        fails++;
    } else {
        printf("  [PASS] compile %s\n", name);
    }
    return s;
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "no display\n"); return 2; }

    GP gpa = (GP)dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
    if (!gpa) gpa = (GP)dlsym(RTLD_DEFAULT, "glXGetProcAddress");
    GLXContext (*createCtx)(Display *, GLXFBConfig, GLXContext, int, const int *) =
        (void *)sym_ld("glXCreateContextAttribsARB");
    GLXFBConfig *(*chooseFB)(Display *, int, const int *, int *) =
        (void *)sym_ld("glXChooseFBConfig");
    int (*makeCur)(Display *, unsigned long, GLXContext) = (void *)sym_ld("glXMakeCurrent");
    int (*swap)(Display *, unsigned long) = (void *)sym_ld("glXSwapBuffers");

    int nfb = 0;
    int vis[] = { GLX_RENDER_TYPE, 1 /* GLX_RGBA_TYPE */, GLX_DOUBLEBUFFER, 1,
                  GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
                  GLX_DEPTH_SIZE, 24, None };
    GLXFBConfig *fb = chooseFB(dpy, DefaultScreen(dpy), vis, &nfb);
    if (!fb || nfb < 1) { fprintf(stderr, "no FBConfig\n"); return 2; }

    int attribs[] = { GLX_CONTEXT_MAJOR_VERSION_ARB, 4,
                      GLX_CONTEXT_MINOR_VERSION_ARB, 3,
                      GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                      0 };
    GLXContext ctx = createCtx(dpy, fb[0], None, 1, attribs);
    if (!ctx) { fprintf(stderr, "no 4.3 context\n"); return 2; }

    Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 256, 256, 0, 0, 0);
    XMapWindow(dpy, win);
    XSync(dpy, 0);
    if (!makeCur(dpy, win, ctx)) { fprintf(stderr, "makecurrent failed\n"); return 2; }

#define R(n) p##n = sym(gpa, "gl" #n)
    R(GetString); R(GetError); R(GetIntegerv);
    R(CreateShader); R(ShaderSource); R(CompileShader); R(GetShaderiv); R(GetShaderInfoLog);
    R(CreateProgram); R(AttachShader); R(LinkProgram); R(GetProgramiv); R(GetProgramInfoLog);
    R(UseProgram); R(GenBuffers); R(BindBuffer); R(BufferData); R(BindBufferBase);
    R(DispatchCompute); R(MemoryBarrier); R(MapBufferRange); R(UnmapBuffer);
    R(GenVertexArrays); R(BindVertexArray); R(EnableVertexAttribArray);
    R(VertexAttribPointer); R(PatchParameteri); R(DrawArrays);
    R(ClearColor); R(Clear); R(Viewport); R(ReadPixels);
#undef R

    const char *ver = (const char *)pGetString(GL_VERSION);
    const char *sl = (const char *)pGetString(GL_SHADING_LANGUAGE_VERSION);
    printf("== context\n  VERSION: %s\n  GLSL:    %s\n  VENDOR:  %s\n",
           ver, sl, (const char *)pGetString(GL_VENDOR));
    chk(ver && strstr(ver, "4.3"), "GL_VERSION reports 4.3");
    chk(sl && strstr(sl, "4.30"), "GLSL version reports 4.30");

    /* ---- compute over an SSBO ---------------------------------------- */
    printf("== compute (#version 430 core)\n");
    const char *cs =
        "#version 430 core\n"
        "layout(local_size_x = 64) in;\n"
        "layout(std430, binding = 0) buffer Data { uint v[]; } d;\n"
        "void main() {\n"
        "    uint i = gl_GlobalInvocationID.x;\n"
        "    d.v[i] = d.v[i] * 2u + 1u;\n"
        "}\n";
    GLuint csh = compile("compute 430", GL_COMPUTE_SHADER, cs);
    GLuint cp = pCreateProgram();
    pAttachShader(cp, csh);
    pLinkProgram(cp);
    GLint lok = 0; pGetProgramiv(cp, GL_LINK_STATUS, &lok);
    chk(lok, "link compute program");

    const GLsizei N = 256;
    unsigned int in[N], out[N];
    for (GLsizei i = 0; i < N; i++) in[i] = (unsigned int)i;
    GLuint ssbo = 0;
    pGenBuffers(1, &ssbo);
    pBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    pBufferData(GL_SHADER_STORAGE_BUFFER, sizeof in, in, GL_DYNAMIC_READ);
    pBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ssbo);
    pUseProgram(cp);
    pDispatchCompute((GLuint)(N / 64), 1, 1);
    pMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    unsigned int *m = (unsigned int *)pMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                                                      sizeof out, GL_MAP_READ_BIT);
    int ok = (m != NULL);
    if (ok) {
        memcpy(out, m, sizeof out);
        pUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        for (GLsizei i = 0; i < N; i++)
            if (out[i] != (unsigned int)i * 2u + 1u) { ok = 0; break; }
    }
    chk(ok, "compute dispatch mutated SSBO correctly (v*2+1)");
    chk(pGetError() == GL_NO_ERROR, "no GL error after compute");

    /* ---- tessellation pipeline --------------------------------------- */
    printf("== tessellation (#version 430 core)\n");
    const char *vs =
        "#version 430 core\n"
        "layout(location = 0) in vec2 pos;\n"
        "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
    const char *tcs =
        "#version 430 core\n"
        "layout(vertices = 3) out;\n"
        "void main() {\n"
        "    if (gl_InvocationID == 0) {\n"
        "        gl_TessLevelInner[0] = 1.0;\n"
        "        gl_TessLevelOuter[0] = 1.0;\n"
        "        gl_TessLevelOuter[1] = 1.0;\n"
        "        gl_TessLevelOuter[2] = 1.0;\n"
        "    }\n"
        "    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
        "}\n";
    const char *tes =
        "#version 430 core\n"
        "layout(triangles, equal_spacing, ccw) in;\n"
        "void main() {\n"
        "    gl_Position = gl_TessCoord.x * gl_in[0].gl_Position\n"
        "                + gl_TessCoord.y * gl_in[1].gl_Position\n"
        "                + gl_TessCoord.z * gl_in[2].gl_Position;\n"
        "}\n";
    const char *fs =
        "#version 430 core\n"
        "layout(location = 0) out vec4 o;\n"
        "void main() { o = vec4(0.0, 1.0, 0.0, 1.0); }\n";
    GLuint vsh = compile("vertex 430", GL_VERTEX_SHADER, vs);
    GLuint tcsh = compile("tess-control 430", GL_TESS_CONTROL_SHADER, tcs);
    GLuint tesh = compile("tess-eval 430", GL_TESS_EVALUATION_SHADER, tes);
    GLuint fsh = compile("fragment 430", GL_FRAGMENT_SHADER, fs);
    GLuint tp = pCreateProgram();
    pAttachShader(tp, vsh); pAttachShader(tp, tcsh);
    pAttachShader(tp, tesh); pAttachShader(tp, fsh);
    pLinkProgram(tp);
    pGetProgramiv(tp, GL_LINK_STATUS, &lok);
    chk(lok, "link tessellation program");
    if (!lok) linklog(tp, "tessellation");

    GLuint vao = 0;
    pGenVertexArrays(1, &vao);
    pBindVertexArray(vao);
    GLuint vbo = 0;
    pGenBuffers(1, &vbo);
    pBindBuffer(GL_ARRAY_BUFFER, vbo);
    float tri[6] = { -1.f, -1.f,  3.f, -1.f,  -1.f, 3.f };
    pBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_DYNAMIC_READ);
    pEnableVertexAttribArray(0);
    pVertexAttribPointer(0, 2, 0x1406 /* GL_FLOAT */, 0, 0, (const void *)0);
    pViewport(0, 0, 64, 64);
    pClearColor(0.f, 0.f, 0.f, 1.f);
    pClear(GL_COLOR_BUFFER_BIT);
    pUseProgram(tp);
    pPatchParameteri(GL_PATCH_VERTICES, 3);
    pDrawArrays(GL_PATCHES, 0, 3);
    chk(pGetError() == GL_NO_ERROR, "no GL error after tessellated draw");
    unsigned char pixel[4] = { 0, 0, 0, 0 };
    pReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    chk(pixel[1] > 0 && pixel[0] == 0, "tessellated draw wrote green pixel");

    swap(dpy, win);
    printf("== %s (%d failure%s)\n", fails ? "FAILED" : "OK", fails, fails == 1 ? "" : "s");
    XCloseDisplay(dpy);
    return fails ? 1 : 0;
}
