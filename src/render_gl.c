/* psprecomp -- hardware OpenGL 3.3 Core renderer ("gl") with 1x-4x internal
 * resolution scaling.
 *
 * Uses dynamic EGL (libEGL.so.1) on Linux to keep the GPU render context
 * isolated from the SDL host window/renderer thread (zero X11/Wayland races),
 * falling back to SDL_GL if needed.
 *
 * Coordinate convention:
 *   PSP screen coordinates (fx in [0, 480], fy in [0, 272]) map to OpenGL NDC
 *   x = fx / 240.0 - 1.0,  y = fy / 136.0 - 1.0 (WITHOUT flipping Y).
 *   That places PSP row y=0 at FBO memory offset y=0, so glReadPixels yields
 *   top-down rows directly and g_gl.Scissor(x0*S, y0*S, w*S, h*S) needs no Y-flip.
 */

#ifdef PSPRECOMP_SDL2_WINDOW

#include "psprecomp/render.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"

#include <SDL.h>
#include <SDL_thread.h>
#include <GL/gl.h>
#include <GL/glext.h>

#ifdef __linux__
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE_W 480
#define BASE_H 272
#define MAX_SCALE 4
#define MAX_FBOS 12
#define TEX_CACHE_SLOTS 128

typedef struct {
    float x, y;
    float z;
    float r, g, b, a;
    float u, v;
} gl_vert;

typedef struct {
    uint32_t addr;
    int      stride;
    int      fmt;
    int      scale;
    GLuint   fbo;
    GLuint   color_tex;
    GLuint   ds_rbo;
    int      has_draws;
} gl_fbo;

typedef struct {
    uint64_t hash;
    int      w, h;
    int      fmt;
    GLuint   tex;
    uint64_t last_use;
} gl_tex_entry;

/* ---- dynamic GL 1.1 + 3.3 proc table ------------------------------------- */

typedef void (APIENTRY *PFN_GL_ENABLE)(GLenum);
typedef void (APIENTRY *PFN_GL_DISABLE)(GLenum);
typedef void (APIENTRY *PFN_GL_VIEWPORT)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_GL_SCISSOR)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_GL_COLORMASK)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (APIENTRY *PFN_GL_DEPTHMASK)(GLboolean);
typedef void (APIENTRY *PFN_GL_DEPTHFUNC)(GLenum);
typedef void (APIENTRY *PFN_GL_STENCILMASK)(GLuint);
typedef void (APIENTRY *PFN_GL_STENCILFUNC)(GLenum, GLint, GLuint);
typedef void (APIENTRY *PFN_GL_STENCILOP)(GLenum, GLenum, GLenum);
typedef void (APIENTRY *PFN_GL_CULLFACE)(GLenum);
typedef void (APIENTRY *PFN_GL_FRONTFACE)(GLenum);
typedef void (APIENTRY *PFN_GL_CLEARCOLOR)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_GL_CLEARDEPTH)(GLdouble);
typedef void (APIENTRY *PFN_GL_CLEARSTENCIL)(GLint);
typedef void (APIENTRY *PFN_GL_CLEAR)(GLbitfield);
typedef void (APIENTRY *PFN_GL_GENTEXTURES)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_GL_DELETETEXTURES)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_GL_BINDTEXTURE)(GLenum, GLuint);
typedef void (APIENTRY *PFN_GL_TEXIMAGE2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (APIENTRY *PFN_GL_TEXPARAMETERI)(GLenum, GLenum, GLint);
typedef void (APIENTRY *PFN_GL_PIXELSTOREI)(GLenum, GLint);
typedef void (APIENTRY *PFN_GL_READPIXELS)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef void (APIENTRY *PFN_GL_DRAWARRAYS)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *PFN_GL_POINTSIZE)(GLfloat);
typedef void (APIENTRY *PFN_GL_LINEWIDTH)(GLfloat);

typedef struct {
    PFN_GL_ENABLE                    Enable;
    PFN_GL_DISABLE                   Disable;
    PFN_GL_VIEWPORT                  Viewport;
    PFN_GL_SCISSOR                   Scissor;
    PFN_GL_COLORMASK                 ColorMask;
    PFN_GL_DEPTHMASK                 DepthMask;
    PFN_GL_DEPTHFUNC                 DepthFunc;
    PFN_GL_STENCILMASK               StencilMask;
    PFN_GL_STENCILFUNC               StencilFunc;
    PFN_GL_STENCILOP                 StencilOp;
    PFN_GL_CULLFACE                  CullFace;
    PFN_GL_FRONTFACE                 FrontFace;
    PFN_GL_CLEARCOLOR                ClearColor;
    PFN_GL_CLEARDEPTH                ClearDepth;
    PFN_GL_CLEARSTENCIL              ClearStencil;
    PFN_GL_CLEAR                     Clear;
    PFN_GL_GENTEXTURES               GenTextures;
    PFN_GL_DELETETEXTURES            DeleteTextures;
    PFN_GL_BINDTEXTURE               BindTexture;
    PFN_GL_TEXIMAGE2D                TexImage2D;
    PFN_GL_TEXPARAMETERI             TexParameteri;
    PFN_GL_PIXELSTOREI               PixelStorei;
    PFN_GL_READPIXELS                ReadPixels;
    PFN_GL_DRAWARRAYS                DrawArrays;
    PFN_GL_POINTSIZE                 PointSize;
    PFN_GL_LINEWIDTH                 LineWidth;
    PFNGLGENFRAMEBUFFERSPROC         GenFramebuffers;
    PFNGLDELETEFRAMEBUFFERSPROC      DeleteFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC         BindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC    FramebufferTexture2D;
    PFNGLGENRENDERBUFFERSPROC        GenRenderbuffers;
    PFNGLDELETERENDERBUFFERSPROC     DeleteRenderbuffers;
    PFNGLBINDRENDERBUFFERPROC        BindRenderbuffer;
    PFNGLRENDERBUFFERSTORAGEPROC     RenderbufferStorage;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC  CheckFramebufferStatus;
    PFNGLGENVERTEXARRAYSPROC         GenVertexArrays;
    PFNGLDELETEVERTEXARRAYSPROC      DeleteVertexArrays;
    PFNGLBINDVERTEXARRAYPROC         BindVertexArray;
    PFNGLGENBUFFERSPROC              GenBuffers;
    PFNGLDELETEBUFFERSPROC           DeleteBuffers;
    PFNGLBINDBUFFERPROC              BindBuffer;
    PFNGLBUFFERDATAPROC              BufferData;
    PFNGLBUFFERSUBDATAPROC           BufferSubData;
    PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray;
    PFNGLVERTEXATTRIBPOINTERPROC     VertexAttribPointer;
    PFNGLCREATESHADERPROC            CreateShader;
    PFNGLDELETESHADERPROC            DeleteShader;
    PFNGLSHADERSOURCEPROC            ShaderSource;
    PFNGLCOMPILESHADERPROC           CompileShader;
    PFNGLGETSHADERIVPROC             GetShaderiv;
    PFNGLGETSHADERINFOLOGPROC        GetShaderInfoLog;
    PFNGLCREATEPROGRAMPROC           CreateProgram;
    PFNGLDELETEPROGRAMPROC           DeleteProgram;
    PFNGLATTACHSHADERPROC            AttachShader;
    PFNGLLINKPROGRAMPROC             LinkProgram;
    PFNGLGETPROGRAMIVPROC            GetProgramiv;
    PFNGLGETPROGRAMINFOLOGPROC       GetProgramInfoLog;
    PFNGLUSEPROGRAMPROC              UseProgram;
    PFNGLGETUNIFORMLOCATIONPROC      GetUniformLocation;
    PFNGLUNIFORM1IPROC               Uniform1i;
    PFNGLUNIFORM1FPROC               Uniform1f;
    PFNGLUNIFORM2FPROC               Uniform2f;
    PFNGLUNIFORM4FPROC               Uniform4f;
    PFNGLACTIVETEXTUREPROC           ActiveTexture;
    PFNGLBLENDFUNCSEPARATEPROC       BlendFuncSeparate;
    PFNGLBLENDEQUATIONSEPARATEPROC   BlendEquationSeparate;
    PFNGLBLENDCOLORPROC              BlendColor;
} gl_procs;

/* ---- static renderer state ----------------------------------------------- */

static gl_procs g_gl;
static int      g_ready = 0;
static int      g_scale = 1;
static SDL_threadID g_ctx_tid = 0;

#ifdef __linux__
static void      *g_egl_so = NULL;
static void      *g_gl_so = NULL;
static EGLDisplay g_egl_dpy = EGL_NO_DISPLAY;
static EGLContext g_egl_ctx = EGL_NO_CONTEXT;
static EGLSurface g_egl_surf = EGL_NO_SURFACE;
static void *(*g_egl_get_proc)(const char *);
static EGLBoolean (*g_egl_make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*g_egl_destroy_ctx)(EGLDisplay, EGLContext);
static EGLBoolean (*g_egl_destroy_surf)(EGLDisplay, EGLSurface);
static EGLBoolean (*g_egl_terminate)(EGLDisplay);
#endif
static SDL_Window   *g_sdl_win = NULL;
static SDL_GLContext g_sdl_ctx = NULL;

static GLuint g_prog = 0;
static GLuint g_vao = 0;
static GLuint g_vbo = 0;
static size_t g_vbo_cap = 0;

/* Cached uniform locations */
static GLint u_tex_enable = -1;
static GLint u_clear_mode = -1;
static GLint u_gouraud    = -1;
static GLint u_tex_func   = -1;
static GLint u_tex_alpha  = -1;
static GLint u_tex_double = -1;
static GLint u_tex_env    = -1;
static GLint u_tex_size   = -1;
static GLint u_atest      = -1;
static GLint u_atest_func = -1;
static GLint u_atest_ref  = -1;
static GLint u_atest_mask = -1;
static GLint u_tex        = -1;

static gl_fbo       g_fbos[MAX_FBOS];
static gl_tex_entry g_tex_cache[TEX_CACHE_SLOTS];
static uint64_t     g_tex_use_tick = 1;

static uint32_t g_cur_fb_addr   = 0x04000000u;
static uint32_t g_cur_fb_stride = 512;
static int      g_cur_fb_fmt    = 3;

/* Scratch vertex buffer */
static gl_vert *g_vbuf = NULL;
static size_t   g_vbuf_cap = 0;

/* Shared host front-buffer snapshot (guarded by g_front_lock) */
static SDL_mutex *g_front_lock = NULL;
static uint32_t  *g_front_px   = NULL;
static int        g_front_w    = 0;
static int        g_front_h    = 0;
static int        g_front_valid = 0;
static int        g_presented_this_list = 0;

static void (*gl_load_fn(const char *name))(void) {
    void *p = NULL;
#ifdef __linux__
    if (g_egl_get_proc) p = g_egl_get_proc(name);
    if (!p && g_gl_so)  p = dlsym(g_gl_so, name);
#endif
    if (!p && g_sdl_ctx) p = SDL_GL_GetProcAddress(name);
    void (*fn)(void) = NULL;
    memcpy(&fn, &p, sizeof fn);
    return fn;
}

#define LOAD_GL(field, type, glname) do { \
    void (*fp_)(void) = gl_load_fn(glname); \
    memcpy(&g_gl.field, &fp_, sizeof g_gl.field); \
    if (!g_gl.field) return -1; \
} while (0)

static int load_gl_procs(void) {
    LOAD_GL(Enable,                  PFN_GL_ENABLE,                    "glEnable");
    LOAD_GL(Disable,                 PFN_GL_DISABLE,                   "glDisable");
    LOAD_GL(Viewport,                PFN_GL_VIEWPORT,                  "glViewport");
    LOAD_GL(Scissor,                 PFN_GL_SCISSOR,                   "glScissor");
    LOAD_GL(ColorMask,               PFN_GL_COLORMASK,                 "glColorMask");
    LOAD_GL(DepthMask,               PFN_GL_DEPTHMASK,                 "glDepthMask");
    LOAD_GL(DepthFunc,               PFN_GL_DEPTHFUNC,                 "glDepthFunc");
    LOAD_GL(StencilMask,             PFN_GL_STENCILMASK,               "glStencilMask");
    LOAD_GL(StencilFunc,             PFN_GL_STENCILFUNC,               "glStencilFunc");
    LOAD_GL(StencilOp,               PFN_GL_STENCILOP,                 "glStencilOp");
    LOAD_GL(CullFace,                PFN_GL_CULLFACE,                  "glCullFace");
    LOAD_GL(FrontFace,               PFN_GL_FRONTFACE,                 "glFrontFace");
    LOAD_GL(ClearColor,              PFN_GL_CLEARCOLOR,                "glClearColor");
    LOAD_GL(ClearDepth,              PFN_GL_CLEARDEPTH,                "glClearDepth");
    LOAD_GL(ClearStencil,            PFN_GL_CLEARSTENCIL,              "glClearStencil");
    LOAD_GL(Clear,                   PFN_GL_CLEAR,                     "glClear");
    LOAD_GL(GenTextures,             PFN_GL_GENTEXTURES,               "glGenTextures");
    LOAD_GL(DeleteTextures,          PFN_GL_DELETETEXTURES,            "glDeleteTextures");
    LOAD_GL(BindTexture,             PFN_GL_BINDTEXTURE,               "glBindTexture");
    LOAD_GL(TexImage2D,              PFN_GL_TEXIMAGE2D,                "glTexImage2D");
    LOAD_GL(TexParameteri,           PFN_GL_TEXPARAMETERI,             "glTexParameteri");
    LOAD_GL(PixelStorei,             PFN_GL_PIXELSTOREI,               "glPixelStorei");
    LOAD_GL(ReadPixels,              PFN_GL_READPIXELS,                "glReadPixels");
    LOAD_GL(DrawArrays,              PFN_GL_DRAWARRAYS,                "glDrawArrays");
    LOAD_GL(PointSize,               PFN_GL_POINTSIZE,                 "glPointSize");
    LOAD_GL(LineWidth,               PFN_GL_LINEWIDTH,                 "glLineWidth");
    LOAD_GL(GenFramebuffers,         PFNGLGENFRAMEBUFFERSPROC,         "glGenFramebuffers");
    LOAD_GL(DeleteFramebuffers,      PFNGLDELETEFRAMEBUFFERSPROC,      "glDeleteFramebuffers");
    LOAD_GL(BindFramebuffer,         PFNGLBINDFRAMEBUFFERPROC,         "glBindFramebuffer");
    LOAD_GL(FramebufferTexture2D,    PFNGLFRAMEBUFFERTEXTURE2DPROC,    "glFramebufferTexture2D");
    LOAD_GL(GenRenderbuffers,        PFNGLGENRENDERBUFFERSPROC,        "glGenRenderbuffers");
    LOAD_GL(DeleteRenderbuffers,     PFNGLDELETERENDERBUFFERSPROC,     "glDeleteRenderbuffers");
    LOAD_GL(BindRenderbuffer,        PFNGLBINDRENDERBUFFERPROC,        "glBindRenderbuffer");
    LOAD_GL(RenderbufferStorage,     PFNGLRENDERBUFFERSTORAGEPROC,     "glRenderbufferStorage");
    LOAD_GL(FramebufferRenderbuffer, PFNGLFRAMEBUFFERRENDERBUFFERPROC, "glFramebufferRenderbuffer");
    LOAD_GL(CheckFramebufferStatus,  PFNGLCHECKFRAMEBUFFERSTATUSPROC,  "glCheckFramebufferStatus");
    LOAD_GL(GenVertexArrays,         PFNGLGENVERTEXARRAYSPROC,         "glGenVertexArrays");
    LOAD_GL(DeleteVertexArrays,      PFNGLDELETEVERTEXARRAYSPROC,      "glDeleteVertexArrays");
    LOAD_GL(BindVertexArray,         PFNGLBINDVERTEXARRAYPROC,         "glBindVertexArray");
    LOAD_GL(GenBuffers,              PFNGLGENBUFFERSPROC,              "glGenBuffers");
    LOAD_GL(DeleteBuffers,           PFNGLDELETEBUFFERSPROC,           "glDeleteBuffers");
    LOAD_GL(BindBuffer,              PFNGLBINDBUFFERPROC,              "glBindBuffer");
    LOAD_GL(BufferData,              PFNGLBUFFERDATAPROC,              "glBufferData");
    LOAD_GL(BufferSubData,           PFNGLBUFFERSUBDATAPROC,           "glBufferSubData");
    LOAD_GL(EnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC, "glEnableVertexAttribArray");
    LOAD_GL(VertexAttribPointer,     PFNGLVERTEXATTRIBPOINTERPROC,     "glVertexAttribPointer");
    LOAD_GL(CreateShader,            PFNGLCREATESHADERPROC,            "glCreateShader");
    LOAD_GL(DeleteShader,            PFNGLDELETESHADERPROC,            "glDeleteShader");
    LOAD_GL(ShaderSource,            PFNGLSHADERSOURCEPROC,            "glShaderSource");
    LOAD_GL(CompileShader,           PFNGLCOMPILESHADERPROC,           "glCompileShader");
    LOAD_GL(GetShaderiv,             PFNGLGETSHADERIVPROC,             "glGetShaderiv");
    LOAD_GL(GetShaderInfoLog,        PFNGLGETSHADERINFOLOGPROC,        "glGetShaderInfoLog");
    LOAD_GL(CreateProgram,           PFNGLCREATEPROGRAMPROC,           "glCreateProgram");
    LOAD_GL(DeleteProgram,           PFNGLDELETEPROGRAMPROC,           "glDeleteProgram");
    LOAD_GL(AttachShader,            PFNGLATTACHSHADERPROC,            "glAttachShader");
    LOAD_GL(LinkProgram,             PFNGLLINKPROGRAMPROC,             "glLinkProgram");
    LOAD_GL(GetProgramiv,            PFNGLGETPROGRAMIVPROC,            "glGetProgramiv");
    LOAD_GL(GetProgramInfoLog,       PFNGLGETPROGRAMINFOLOGPROC,       "glGetProgramInfoLog");
    LOAD_GL(UseProgram,              PFNGLUSEPROGRAMPROC,              "glUseProgram");
    LOAD_GL(GetUniformLocation,      PFNGLGETUNIFORMLOCATIONPROC,      "glGetUniformLocation");
    LOAD_GL(Uniform1i,               PFNGLUNIFORM1IPROC,               "glUniform1i");
    LOAD_GL(Uniform1f,               PFNGLUNIFORM1FPROC,               "glUniform1f");
    LOAD_GL(Uniform2f,               PFNGLUNIFORM2FPROC,               "glUniform2f");
    LOAD_GL(Uniform4f,               PFNGLUNIFORM4FPROC,               "glUniform4f");
    LOAD_GL(ActiveTexture,           PFNGLACTIVETEXTUREPROC,           "glActiveTexture");
    LOAD_GL(BlendFuncSeparate,       PFNGLBLENDFUNCSEPARATEPROC,       "glBlendFuncSeparate");
    LOAD_GL(BlendEquationSeparate,   PFNGLBLENDEQUATIONSEPARATEPROC,   "glBlendEquationSeparate");
    LOAD_GL(BlendColor,              PFNGLBLENDCOLORPROC,              "glBlendColor");
    return 0;
}

/* ---- shader compilation -------------------------------------------------- */

static const char *VS_SRC =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in float a_z;\n"
    "layout(location = 2) in vec4 a_col;\n"
    "layout(location = 3) in vec2 a_uv;\n"
    "flat out vec4 v_flat;\n"
    "out vec4 v_smooth;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    float nx = a_pos.x * (2.0 / 480.0) - 1.0;\n"
    "    float ny = a_pos.y * (2.0 / 272.0) - 1.0;\n"
    "    float nz = clamp(a_z, 0.0, 65535.0) * (2.0 / 65535.0) - 1.0;\n"
    "    gl_Position = vec4(nx, ny, nz, 1.0);\n"
    "    v_flat = a_col;\n"
    "    v_smooth = a_col;\n"
    "    v_uv = a_uv;\n"
    "}\n";

static const char *FS_SRC =
    "#version 330 core\n"
    "flat in vec4 v_flat;\n"
    "in vec4 v_smooth;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_col;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_tex_enable;\n"
    "uniform int u_clear_mode;\n"
    "uniform int u_gouraud;\n"
    "uniform int u_tex_func;\n"
    "uniform int u_tex_alpha;\n"
    "uniform float u_tex_double;\n"
    "uniform vec4 u_tex_env;\n"
    "uniform vec2 u_tex_size;\n"
    "uniform int u_atest;\n"
    "uniform int u_atest_func;\n"
    "uniform int u_atest_ref;\n"
    "uniform int u_atest_mask;\n"
    "void main() {\n"
    "    vec4 c = (u_clear_mode != 0 || u_gouraud == 0) ? v_flat : v_smooth;\n"
    "    if (u_clear_mode == 0) {\n"
    "        if (u_tex_enable != 0) {\n"
    "            vec4 t = texture(u_tex, v_uv / u_tex_size);\n"
    "            vec3 rgb;\n"
    "            float a = (u_tex_alpha != 0) ? (t.a * c.a) : c.a;\n"
    "            if (u_tex_func == 0) {\n"
    "                rgb = t.rgb * c.rgb;\n"
    "            } else if (u_tex_func == 1) {\n"
    "                vec3 m = c.rgb * (1.0 - t.a) + u_tex_env.rgb * t.a;\n"
    "                rgb = mix(c.rgb, m, u_tex_alpha != 0 ? 1.0 : 0.0);\n"
    "                if (u_tex_alpha == 0) rgb = c.rgb;\n"
    "                a = c.a;\n"
    "            } else if (u_tex_func == 2) {\n"
    "                rgb = c.rgb * (1.0 - t.rgb) + u_tex_env.rgb * t.rgb;\n"
    "            } else if (u_tex_func == 3) {\n"
    "                rgb = t.rgb;\n"
    "                a = (u_tex_alpha != 0) ? t.a : c.a;\n"
    "            } else {\n"
    "                rgb = t.rgb + c.rgb;\n"
    "            }\n"
    "            c = vec4(clamp(rgb * u_tex_double, 0.0, 1.0), clamp(a, 0.0, 1.0));\n"
    "        }\n"
    "        if (u_atest != 0) {\n"
    "            int ia = int(round(clamp(c.a, 0.0, 1.0) * 255.0)) & u_atest_mask;\n"
    "            int ir = u_atest_ref & u_atest_mask;\n"
    "            bool pass = true;\n"
    "            if (u_atest_func == 0) pass = false;\n"
    "            else if (u_atest_func == 1) pass = true;\n"
    "            else if (u_atest_func == 2) pass = (ia == ir);\n"
    "            else if (u_atest_func == 3) pass = (ia != ir);\n"
    "            else if (u_atest_func == 4) pass = (ia < ir);\n"
    "            else if (u_atest_func == 5) pass = (ia <= ir);\n"
    "            else if (u_atest_func == 6) pass = (ia > ir);\n"
    "            else pass = (ia >= ir);\n"
    "            if (!pass) discard;\n"
    "        }\n"
    "    }\n"
    "    o_col = c;\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = g_gl.CreateShader(type);
    if (!s) return 0;
    g_gl.ShaderSource(s, 1, &src, NULL);
    g_gl.CompileShader(s);
    GLint ok = 0;
    g_gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        g_gl.GetShaderInfoLog(s, (GLsizei)sizeof log, NULL, log);
        fprintf(stderr, "render_gl: shader error: %s\n", log);
        g_gl.DeleteShader(s);
        return 0;
    }
    return s;
}

static int build_program(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, VS_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FS_SRC);
    if (!vs || !fs) {
        if (vs) g_gl.DeleteShader(vs);
        if (fs) g_gl.DeleteShader(fs);
        return -1;
    }
    GLuint p = g_gl.CreateProgram();
    g_gl.AttachShader(p, vs);
    g_gl.AttachShader(p, fs);
    g_gl.LinkProgram(p);
    g_gl.DeleteShader(vs);
    g_gl.DeleteShader(fs);
    GLint ok = 0;
    g_gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        g_gl.GetProgramInfoLog(p, (GLsizei)sizeof log, NULL, log);
        fprintf(stderr, "render_gl: link error: %s\n", log);
        g_gl.DeleteProgram(p);
        return -1;
    }
    g_prog = p;
    u_tex_enable = g_gl.GetUniformLocation(p, "u_tex_enable");
    u_clear_mode = g_gl.GetUniformLocation(p, "u_clear_mode");
    u_gouraud    = g_gl.GetUniformLocation(p, "u_gouraud");
    u_tex_func   = g_gl.GetUniformLocation(p, "u_tex_func");
    u_tex_alpha  = g_gl.GetUniformLocation(p, "u_tex_alpha");
    u_tex_double = g_gl.GetUniformLocation(p, "u_tex_double");
    u_tex_env    = g_gl.GetUniformLocation(p, "u_tex_env");
    u_tex_size   = g_gl.GetUniformLocation(p, "u_tex_size");
    u_atest      = g_gl.GetUniformLocation(p, "u_atest");
    u_atest_func = g_gl.GetUniformLocation(p, "u_atest_func");
    u_atest_ref  = g_gl.GetUniformLocation(p, "u_atest_ref");
    u_atest_mask = g_gl.GetUniformLocation(p, "u_atest_mask");
    u_tex        = g_gl.GetUniformLocation(p, "u_tex");
    return 0;
}

/* ---- context & thread binding -------------------------------------------- */

static int ensure_context_current(void) {
    SDL_threadID tid = SDL_ThreadID();
    if (tid == g_ctx_tid) return 0;
    if (g_ctx_tid != 0) return -1;
#ifdef __linux__
    if (g_egl_dpy != EGL_NO_DISPLAY && g_egl_make_current) {
        if (g_egl_make_current(g_egl_dpy, g_egl_surf, g_egl_surf, g_egl_ctx)) {
            g_ctx_tid = tid;
            return 0;
        }
    }
#endif
    if (g_sdl_win && g_sdl_ctx && SDL_GL_MakeCurrent(g_sdl_win, g_sdl_ctx) == 0) {
        g_ctx_tid = tid;
        return 0;
    }
    return -1;
}

static void release_context_current(void) {
    if (g_ctx_tid == 0) return;
#ifdef __linux__
    if (g_egl_dpy != EGL_NO_DISPLAY && g_egl_make_current) {
        g_egl_make_current(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
#endif
    if (g_sdl_win && g_sdl_ctx) {
        SDL_GL_MakeCurrent(g_sdl_win, NULL);
    }
    g_ctx_tid = 0;
}

static void drop_all_fbos(void) {
    for (int i = 0; i < MAX_FBOS; i++) {
        if (g_fbos[i].fbo)       g_gl.DeleteFramebuffers(1, &g_fbos[i].fbo);
        if (g_fbos[i].color_tex) g_gl.DeleteTextures(1, &g_fbos[i].color_tex);
        if (g_fbos[i].ds_rbo)    g_gl.DeleteRenderbuffers(1, &g_fbos[i].ds_rbo);
        memset(&g_fbos[i], 0, sizeof g_fbos[i]);
    }
}

static void drop_all_textures(void) {
    for (int i = 0; i < TEX_CACHE_SLOTS; i++) {
        if (g_tex_cache[i].tex) g_gl.DeleteTextures(1, &g_tex_cache[i].tex);
        memset(&g_tex_cache[i], 0, sizeof g_tex_cache[i]);
    }
}

/* Normalize VRAM mirror aliases so FBP and sceDisplaySetFrameBuf match cleanly. */
static uint32_t norm_vram_addr(uint32_t addr) {
    addr &= 0x3FFFFFFFu;
    if (addr < 0x00400000u) addr += PSP_VRAM_BASE;
    if (addr >= PSP_VRAM_BASE && addr < PSP_VRAM_BASE + 0x00800000u) {
        addr = PSP_VRAM_BASE + ((addr - PSP_VRAM_BASE) & (PSP_VRAM_SIZE - 1u));
    }
    return addr;
}

static gl_fbo *find_fbo(uint32_t addr) {
    addr = norm_vram_addr(addr);
    for (int i = 0; i < MAX_FBOS; i++) {
        if (g_fbos[i].fbo && g_fbos[i].addr == addr) return &g_fbos[i];
    }
    return NULL;
}

static gl_fbo *get_or_create_fbo(uint32_t addr, int stride, int fmt) {
    addr = norm_vram_addr(addr);
    gl_fbo *f = find_fbo(addr);
    if (f) {
        f->stride = stride;
        f->fmt = fmt;
        return f;
    }
    int slot = -1;
    for (int i = 0; i < MAX_FBOS; i++) {
        if (!g_fbos[i].fbo) { slot = i; break; }
    }
    if (slot < 0) {
        /* Evict first non-displayed FBO. */
        uint32_t disp = norm_vram_addr(psp_display_framebuffer());
        slot = 0;
        for (int i = 0; i < MAX_FBOS; i++) {
            if (g_fbos[i].addr != disp) { slot = i; break; }
        }
        g_gl.DeleteFramebuffers(1, &g_fbos[slot].fbo);
        g_gl.DeleteTextures(1, &g_fbos[slot].color_tex);
        g_gl.DeleteRenderbuffers(1, &g_fbos[slot].ds_rbo);
        memset(&g_fbos[slot], 0, sizeof g_fbos[slot]);
    }
    f = &g_fbos[slot];
    const int sw = BASE_W * g_scale;
    const int sh = BASE_H * g_scale;

    g_gl.GenFramebuffers(1, &f->fbo);
    g_gl.BindFramebuffer(GL_FRAMEBUFFER, f->fbo);

    g_gl.GenTextures(1, &f->color_tex);
    g_gl.BindTexture(GL_TEXTURE_2D, f->color_tex);
    g_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->color_tex, 0);

    g_gl.GenRenderbuffers(1, &f->ds_rbo);
    g_gl.BindRenderbuffer(GL_RENDERBUFFER, f->ds_rbo);
    g_gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, sw, sh);
    g_gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, f->ds_rbo);

    if (g_gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        g_gl.DeleteFramebuffers(1, &f->fbo);
        g_gl.DeleteTextures(1, &f->color_tex);
        g_gl.DeleteRenderbuffers(1, &f->ds_rbo);
        memset(f, 0, sizeof *f);
        return NULL;
    }

    g_gl.Disable(GL_SCISSOR_TEST);
    g_gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g_gl.DepthMask(GL_TRUE);
    g_gl.StencilMask(0xFFu);
    g_gl.ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    g_gl.ClearDepth(0.0);
    g_gl.ClearStencil(0);
    g_gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    f->addr = addr;
    f->stride = stride;
    f->fmt = fmt;
    f->scale = g_scale;
    f->has_draws = 0;
    return f;
}

/* ---- texture hashing & cache --------------------------------------------- */

static const int GL_TEX_BPP[11] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8 };

static uint64_t fnv_step(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ULL;
    return h;
}

static uint64_t hash_texture(const psp_draw_state *st) {
    uint64_t h = 14695981039346656037ULL;
    h = fnv_step(h, st->tex_addr);
    h = fnv_step(h, (uint64_t)st->tex_bw);
    h = fnv_step(h, ((uint64_t)st->tex_w << 16) | (uint64_t)st->tex_h);
    h = fnv_step(h, ((uint64_t)st->tex_fmt << 8) | (uint64_t)(st->tex_swizzle & 1));

    const int fmt = (st->tex_fmt >= 0 && st->tex_fmt <= 10) ? st->tex_fmt : 3;
    const int bpp = GL_TEX_BPP[fmt];
    const size_t stride_bytes = (size_t)(st->tex_bw > 0 ? st->tex_bw : st->tex_w) * (size_t)bpp / 8u;
    size_t byte_len = stride_bytes * (size_t)st->tex_h;
    if (byte_len == 0) byte_len = 64;

    const uint8_t *ptr = psp_mem_ptr(st->tex_addr, 4);
    if (ptr) {
        const size_t n32 = byte_len / 4u;
        const size_t step = n32 > 256u ? n32 / 256u : 1u;
        for (size_t i = 0; i < n32; i += step) {
            uint32_t w;
            memcpy(&w, ptr + i * 4u, 4);
            h = fnv_step(h, w);
        }
    }
    if (fmt >= PSP_TPSM_T4 && fmt <= PSP_TPSM_T32 && st->clut) {
        h = fnv_step(h, ((uint64_t)st->clut_fmt << 24) |
                        ((uint64_t)st->clut_shift << 16) |
                        ((uint64_t)st->clut_mask << 8) |
                        (uint64_t)st->clut_start);
        const size_t clut_bytes = (fmt == PSP_TPSM_T4) ? 64u : 512u;
        const uint8_t *cb = st->clut + (size_t)(st->clut_start * 16) * (st->clut_fmt == 3 ? 4u : 2u);
        for (size_t i = 0; i + 4u <= clut_bytes; i += 4u) {
            uint32_t w;
            memcpy(&w, cb + i, 4);
            h = fnv_step(h, w);
        }
    }
    return h;
}

static GLuint get_texture(const psp_draw_state *st, int *out_w, int *out_h) {
    int w = st->tex_w > 0 ? st->tex_w : 1;
    int h = st->tex_h > 0 ? st->tex_h : 1;
    if (w > 1024) w = 1024;
    if (h > 1024) h = 1024;
    *out_w = w;
    *out_h = h;

    /* Check render-to-texture FBO first (keeps dynamic effects hi-res). */
    const uint32_t ta = norm_vram_addr(st->tex_addr);
    const uint32_t cur = norm_vram_addr(g_cur_fb_addr);
    if (ta >= PSP_VRAM_BASE && ta < PSP_VRAM_BASE + PSP_VRAM_SIZE && ta != cur) {
        gl_fbo *rt = find_fbo(ta);
        if (rt && rt->has_draws) {
            const GLint fil = psp_render_get_tex_filter() ? GL_LINEAR : GL_NEAREST;
            g_gl.BindTexture(GL_TEXTURE_2D, rt->color_tex);
            g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, fil);
            g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, fil);
            g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            st->tex_clamp_u ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            st->tex_clamp_v ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            return rt->color_tex;
        }
    }

    const uint64_t hash = hash_texture(st);
    int empty = -1, lru = 0;
    uint64_t best_tick = UINT64_MAX;

    for (int i = 0; i < TEX_CACHE_SLOTS; i++) {
        if (g_tex_cache[i].tex) {
            if (g_tex_cache[i].hash == hash && g_tex_cache[i].w == w &&
                g_tex_cache[i].h == h && g_tex_cache[i].fmt == st->tex_fmt) {
                g_tex_cache[i].last_use = g_tex_use_tick++;
                const GLint fil = psp_render_get_tex_filter() ? GL_LINEAR : GL_NEAREST;
                g_gl.BindTexture(GL_TEXTURE_2D, g_tex_cache[i].tex);
                g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, fil);
                g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, fil);
                g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                st->tex_clamp_u ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                st->tex_clamp_v ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                return g_tex_cache[i].tex;
            }
            if (g_tex_cache[i].last_use < best_tick) {
                best_tick = g_tex_cache[i].last_use;
                lru = i;
            }
        } else if (empty < 0) {
            empty = i;
        }
    }

    const int slot = (empty >= 0) ? empty : lru;
    gl_tex_entry *e = &g_tex_cache[slot];
    if (!e->tex) g_gl.GenTextures(1, &e->tex);

    const size_t n_px = (size_t)w * (size_t)h;
    uint32_t *rgba = (uint32_t *)malloc(n_px * 4u);
    if (!rgba) return 0;

    psp_draw_state tmp = *st;
    tmp.tex_clamp_u = 1;
    tmp.tex_clamp_v = 1;
    for (int y = 0; y < h; y++) {
        uint32_t *row = rgba + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            row[x] = psp_tex_fetch(&tmp, x, y);
        }
    }

    g_gl.BindTexture(GL_TEXTURE_2D, e->tex);
    const GLint fil = psp_render_get_tex_filter() ? GL_LINEAR : GL_NEAREST;
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, fil);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, fil);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    st->tex_clamp_u ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    g_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    st->tex_clamp_v ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    g_gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);

    e->hash = hash;
    e->w = w;
    e->h = h;
    e->fmt = st->tex_fmt;
    e->last_use = g_tex_use_tick++;
    return e->tex;
}

/* ---- GL state helpers ---------------------------------------------------- */

static GLenum gl_cmp_func(int func) {
    switch (func) {
    case 0:  return GL_NEVER;
    case 1:  return GL_ALWAYS;
    case 2:  return GL_EQUAL;
    case 3:  return GL_NOTEQUAL;
    case 4:  return GL_LESS;
    case 5:  return GL_LEQUAL;
    case 6:  return GL_GREATER;
    default: return GL_GEQUAL;
    }
}

static GLenum gl_stencil_op(int op) {
    switch (op) {
    case 0:  return GL_KEEP;
    case 1:  return GL_ZERO;
    case 2:  return GL_REPLACE;
    case 3:  return GL_INVERT;
    case 4:  return GL_INCR;
    case 5:  return GL_DECR;
    default: return GL_KEEP;
    }
}

static GLenum gl_blend_factor(int code, int is_src, uint32_t fix) {
    switch (code) {
    case 0:  return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 1:  return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 2:  return GL_SRC_ALPHA;
    case 3:  return GL_ONE_MINUS_SRC_ALPHA;
    case 4:  return GL_DST_ALPHA;
    case 5:  return GL_ONE_MINUS_DST_ALPHA;
    case 6:  return GL_SRC_ALPHA;           /* 2*srcA approximated */
    case 7:  return GL_ONE_MINUS_SRC_ALPHA;
    case 8:  return GL_DST_ALPHA;
    case 9:  return GL_ONE_MINUS_DST_ALPHA;
    default:
        if ((fix & 0x00FFFFFFu) == 0x00FFFFFFu) return GL_ONE;
        if ((fix & 0x00FFFFFFu) == 0u)          return GL_ZERO;
        return GL_CONSTANT_COLOR;
    }
}

static GLenum gl_blend_eq(int op) {
    switch (op) {
    case 0:  return GL_FUNC_ADD;
    case 1:  return GL_FUNC_SUBTRACT;
    case 2:  return GL_FUNC_REVERSE_SUBTRACT;
    case 3:  return GL_MIN;
    case 4:  return GL_MAX;
    default: return GL_FUNC_ADD;
    }
}

/* ---- front buffer snapshot for window thread ----------------------------- */

static void sync_scale_if_needed(void) {
    const int req = psp_render_get_scale();
    if (req != g_scale && req >= 1 && req <= MAX_SCALE) {
        drop_all_fbos();
        g_scale = req;
    }
}

static void capture_fbo_to_front(gl_fbo *f) {
    if (!f || !f->has_draws || !g_front_lock || !g_front_px) return;
    const int w = BASE_W * f->scale;
    const int h = BASE_H * f->scale;
    if (w > BASE_W * MAX_SCALE || h > BASE_H * MAX_SCALE) return;

    SDL_LockMutex(g_front_lock);
    g_gl.BindFramebuffer(GL_FRAMEBUFFER, f->fbo);
    g_gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    g_gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_front_px);
    g_front_w = w;
    g_front_h = h;
    g_front_valid = 1;
    SDL_UnlockMutex(g_front_lock);
}

static int gl_front_reader(uint32_t *dst_buf, uint32_t dst_cap_px,
                           int *out_w, int *out_h) {
    if (!g_front_lock || !g_front_px) return -1;
    int ok = -1;
    SDL_LockMutex(g_front_lock);
    if (g_front_valid && g_front_w > 0 && g_front_h > 0) {
        const uint32_t npx = (uint32_t)g_front_w * (uint32_t)g_front_h;
        if (npx <= dst_cap_px) {
            memcpy(dst_buf, g_front_px, (size_t)npx * 4u);
            if (out_w) *out_w = g_front_w;
            if (out_h) *out_h = g_front_h;
            ok = 0;
        }
    }
    SDL_UnlockMutex(g_front_lock);
    return ok;
}

/* ---- backend vtable ------------------------------------------------------ */

static void gl_shutdown(void);

static int gl_init(int width, int height) {
    (void)width;
    (void)height;
    if (g_ready) return 0;

    const char *vd = getenv("SDL_VIDEODRIVER");
    if (vd && strcmp(vd, "dummy") == 0) {
        return -1;
    }

    memset(&g_gl, 0, sizeof g_gl);
    memset(g_fbos, 0, sizeof g_fbos);
    memset(g_tex_cache, 0, sizeof g_tex_cache);
    g_scale = psp_render_get_scale();
    if (g_scale < 1) g_scale = 1;
    if (g_scale > MAX_SCALE) g_scale = MAX_SCALE;

    int ctx_ok = 0;

#ifdef __linux__
    g_gl_so = dlopen("libGL.so.1", RTLD_LAZY);
    g_egl_so = dlopen("libEGL.so.1", RTLD_LAZY);
    if (g_egl_so) {
        EGLDisplay (*egl_get_display)(EGLNativeDisplayType) = NULL;
        EGLBoolean (*egl_init_fn)(EGLDisplay, EGLint *, EGLint *) = NULL;
        EGLBoolean (*egl_bind_api)(EGLenum) = NULL;
        EGLBoolean (*egl_choose_cfg)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = NULL;
        EGLSurface (*egl_create_pbuf)(EGLDisplay, EGLConfig, const EGLint *) = NULL;
        EGLContext (*egl_create_ctx)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = NULL;

        *(void **)&egl_get_display    = dlsym(g_egl_so, "eglGetDisplay");
        *(void **)&egl_init_fn        = dlsym(g_egl_so, "eglInitialize");
        *(void **)&egl_bind_api       = dlsym(g_egl_so, "eglBindAPI");
        *(void **)&egl_choose_cfg     = dlsym(g_egl_so, "eglChooseConfig");
        *(void **)&egl_create_pbuf    = dlsym(g_egl_so, "eglCreatePbufferSurface");
        *(void **)&egl_create_ctx     = dlsym(g_egl_so, "eglCreateContext");
        *(void **)&g_egl_make_current = dlsym(g_egl_so, "eglMakeCurrent");
        *(void **)&g_egl_destroy_ctx  = dlsym(g_egl_so, "eglDestroyContext");
        *(void **)&g_egl_destroy_surf = dlsym(g_egl_so, "eglDestroySurface");
        *(void **)&g_egl_terminate    = dlsym(g_egl_so, "eglTerminate");
        *(void **)&g_egl_get_proc     = dlsym(g_egl_so, "eglGetProcAddress");

        if (egl_get_display && egl_init_fn && egl_bind_api && egl_choose_cfg &&
            egl_create_pbuf && egl_create_ctx && g_egl_make_current && g_egl_get_proc) {
            g_egl_dpy = egl_get_display(EGL_DEFAULT_DISPLAY);
            if (g_egl_dpy != EGL_NO_DISPLAY && egl_init_fn(g_egl_dpy, NULL, NULL) &&
                egl_bind_api(EGL_OPENGL_API)) {
                static const EGLint cfg_attr[] = {
                    EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
                    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                    EGL_RED_SIZE,        8,
                    EGL_GREEN_SIZE,      8,
                    EGL_BLUE_SIZE,       8,
                    EGL_ALPHA_SIZE,      8,
                    EGL_NONE
                };
                EGLConfig cfg = NULL;
                EGLint ncfg = 0;
                if (egl_choose_cfg(g_egl_dpy, cfg_attr, &cfg, 1, &ncfg) && ncfg > 0) {
                    static const EGLint pbuf_attr[] = {
                        EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE
                    };
                    static const EGLint ctx_attr[] = {
                        EGL_CONTEXT_MAJOR_VERSION, 3,
                        EGL_CONTEXT_MINOR_VERSION, 3,
                        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                        EGL_NONE
                    };
                    g_egl_surf = egl_create_pbuf(g_egl_dpy, cfg, pbuf_attr);
                    g_egl_ctx  = egl_create_ctx(g_egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
                    if (g_egl_ctx != EGL_NO_CONTEXT && g_egl_surf != EGL_NO_SURFACE &&
                        g_egl_make_current(g_egl_dpy, g_egl_surf, g_egl_surf, g_egl_ctx)) {
                        ctx_ok = 1;
                    }
                }
            }
        }
    }
#endif

    if (!ctx_ok) {
        if (!SDL_WasInit(SDL_INIT_VIDEO)) {
            if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) { gl_shutdown(); return -1; }
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        g_sdl_win = SDL_CreateWindow("psprecomp-gl", SDL_WINDOWPOS_UNDEFINED,
                                     SDL_WINDOWPOS_UNDEFINED, 16, 16,
                                     SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (g_sdl_win) {
            g_sdl_ctx = SDL_GL_CreateContext(g_sdl_win);
            if (g_sdl_ctx && SDL_GL_MakeCurrent(g_sdl_win, g_sdl_ctx) == 0) {
                ctx_ok = 1;
            }
        }
    }

    if (!ctx_ok || load_gl_procs() != 0 || build_program() != 0) {
        gl_shutdown();
        return -1;
    }

    g_ctx_tid = SDL_ThreadID();
    g_gl.GenVertexArrays(1, &g_vao);
    g_gl.GenBuffers(1, &g_vbo);
    g_gl.BindVertexArray(g_vao);
    g_gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
    g_gl.EnableVertexAttribArray(0);
    g_gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(gl_vert), (void *)offsetof(gl_vert, x));
    g_gl.EnableVertexAttribArray(1);
    g_gl.VertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(gl_vert), (void *)offsetof(gl_vert, z));
    g_gl.EnableVertexAttribArray(2);
    g_gl.VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(gl_vert), (void *)offsetof(gl_vert, r));
    g_gl.EnableVertexAttribArray(3);
    g_gl.VertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(gl_vert), (void *)offsetof(gl_vert, u));

    g_front_lock = SDL_CreateMutex();
    g_front_px = (uint32_t *)calloc((size_t)(BASE_W * MAX_SCALE) * (size_t)(BASE_H * MAX_SCALE), 4u);
    if (!g_front_lock || !g_front_px) {
        gl_shutdown();
        return -1;
    }

    g_front_valid = 0;
    g_presented_this_list = 0;
    /* Release context from the init thread so guest pthread(s) can bind it cleanly. */
    release_context_current();
    psp_render_set_front_reader(gl_front_reader);
    g_ready = 1;
    fprintf(stderr, "render_gl: initialized OpenGL 3.3 Core (%dx scale)\n", g_scale);
    return 0;
}

static void gl_shutdown(void) {
    psp_render_set_front_reader(NULL);
    if (g_ready && ensure_context_current() == 0) {
        drop_all_fbos();
        drop_all_textures();
        if (g_vbo)  { g_gl.DeleteBuffers(1, &g_vbo); g_vbo = 0; }
        if (g_vao)  { g_gl.DeleteVertexArrays(1, &g_vao); g_vao = 0; }
        if (g_prog) { g_gl.DeleteProgram(g_prog); g_prog = 0; }
    }
    g_vbo_cap = 0;
    free(g_vbuf);
    g_vbuf = NULL;
    g_vbuf_cap = 0;

    if (g_front_lock) { SDL_DestroyMutex(g_front_lock); g_front_lock = NULL; }
    free(g_front_px);
    g_front_px = NULL;
    g_front_valid = 0;

    if (g_sdl_ctx) { SDL_GL_DeleteContext(g_sdl_ctx); g_sdl_ctx = NULL; }
    if (g_sdl_win) { SDL_DestroyWindow(g_sdl_win); g_sdl_win = NULL; }
#ifdef __linux__
    if (g_egl_dpy != EGL_NO_DISPLAY) {
        if (g_egl_make_current) g_egl_make_current(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_egl_ctx != EGL_NO_CONTEXT && g_egl_destroy_ctx) g_egl_destroy_ctx(g_egl_dpy, g_egl_ctx);
        if (g_egl_surf != EGL_NO_SURFACE && g_egl_destroy_surf) g_egl_destroy_surf(g_egl_dpy, g_egl_surf);
        if (g_egl_terminate) g_egl_terminate(g_egl_dpy);
        g_egl_dpy = EGL_NO_DISPLAY;
        g_egl_ctx = EGL_NO_CONTEXT;
        g_egl_surf = EGL_NO_SURFACE;
    }
    if (g_egl_so) { dlclose(g_egl_so); g_egl_so = NULL; }
    if (g_gl_so)  { dlclose(g_gl_so);  g_gl_so = NULL; }
#endif
    g_ctx_tid = 0;
    g_ready = 0;
}

static void gl_set_target(uint32_t addr, uint32_t stride, int fmt) {
    g_cur_fb_addr = norm_vram_addr(addr);
    g_cur_fb_stride = stride;
    g_cur_fb_fmt = fmt;
}

static gl_vert *reserve_verts(size_t n) {
    if (n <= g_vbuf_cap) return g_vbuf;
    size_t cap = g_vbuf_cap ? g_vbuf_cap : 256u;
    while (cap < n) cap *= 2u;
    gl_vert *p = (gl_vert *)realloc(g_vbuf, cap * sizeof(gl_vert));
    if (!p) return NULL;
    g_vbuf = p;
    g_vbuf_cap = cap;
    return g_vbuf;
}

static void pack_vert(gl_vert *dst, const psp_vertex *src) {
    dst->x = src->fx;
    dst->y = src->fy;
    dst->z = src->z;
    dst->r = (float)(src->rgba & 0xFFu) / 255.0f;
    dst->g = (float)((src->rgba >> 8) & 0xFFu) / 255.0f;
    dst->b = (float)((src->rgba >> 16) & 0xFFu) / 255.0f;
    dst->a = (float)((src->rgba >> 24) & 0xFFu) / 255.0f;
    dst->u = src->u;
    dst->v = src->v;
}

static void gl_draw(int prim, const psp_vertex *v, int count, const psp_draw_state *st) {
    if (!g_ready) {
        if (gl_init(BASE_W, BASE_H) != 0) return;
    }
    if (!v || count <= 0 || !st) return;
    if (ensure_context_current() != 0) return;

    gl_fbo *fb = get_or_create_fbo(g_cur_fb_addr, (int)g_cur_fb_stride, g_cur_fb_fmt);
    if (!fb) return;

    const int S = fb->scale;
    g_gl.BindFramebuffer(GL_FRAMEBUFFER, fb->fbo);
    g_gl.Viewport(0, 0, BASE_W * S, BASE_H * S);

    /* Scissor: PSP uses inclusive coordinates [sc_x0..sc_x1], [sc_y0..sc_y1]. */
    int sx0 = st->sc_x0 < 0 ? 0 : st->sc_x0;
    int sy0 = st->sc_y0 < 0 ? 0 : st->sc_y0;
    int sx1 = st->sc_x1 >= BASE_W ? BASE_W - 1 : st->sc_x1;
    int sy1 = st->sc_y1 >= BASE_H ? BASE_H - 1 : st->sc_y1;
    if (sx0 > sx1 || sy0 > sy1) return;
    g_gl.Enable(GL_SCISSOR_TEST);
    g_gl.Scissor(sx0 * S, sy0 * S, (sx1 - sx0 + 1) * S, (sy1 - sy0 + 1) * S);

    /* Expand sprites into triangle pairs; preserve last-vertex provoking order. */
    GLenum gl_prim = GL_TRIANGLES;
    int emit_n = count;
    int is_sprite = (prim == PSP_PRIM_SPRITES);
    int is_2d_prim = is_sprite || prim == PSP_PRIM_POINTS ||
                     prim == PSP_PRIM_LINES || prim == PSP_PRIM_LINE_STRIP;

    if (is_sprite) {
        const int pairs = count / 2;
        if (pairs <= 0) return;
        emit_n = pairs * 6;
        gl_vert *out = reserve_verts((size_t)emit_n);
        if (!out) return;
        for (int i = 0; i < pairs; i++) {
            gl_vert a, b, ab, ba;
            pack_vert(&a, &v[i * 2]);
            pack_vert(&b, &v[i * 2 + 1]);
            ab = b; ab.y = a.y; ab.v = a.v;
            ba = a; ba.y = b.y; ba.v = b.v;
            /* Last vertex of BOTH triangles is b so flat/clear uses v1's color & z. */
            ba.r = b.r; ba.g = b.g; ba.b = b.b; ba.a = b.a; ba.z = b.z;
            ab.r = b.r; ab.g = b.g; ab.b = b.b; ab.a = b.a; ab.z = b.z;
            out[i * 6 + 0] = a;
            out[i * 6 + 1] = ab;
            out[i * 6 + 2] = b;
            out[i * 6 + 3] = a;
            out[i * 6 + 4] = ba;
            out[i * 6 + 5] = b;
        }
        gl_prim = GL_TRIANGLES;
    } else {
        gl_vert *out = reserve_verts((size_t)count);
        if (!out) return;
        for (int i = 0; i < count; i++) pack_vert(&out[i], &v[i]);
        switch (prim) {
        case PSP_PRIM_POINTS:         gl_prim = GL_POINTS; break;
        case PSP_PRIM_LINES:          gl_prim = GL_LINES; break;
        case PSP_PRIM_LINE_STRIP:     gl_prim = GL_LINE_STRIP; break;
        case PSP_PRIM_TRIANGLES:      gl_prim = GL_TRIANGLES; break;
        case PSP_PRIM_TRIANGLE_STRIP: gl_prim = GL_TRIANGLE_STRIP; break;
        case PSP_PRIM_TRIANGLE_FAN:   gl_prim = GL_TRIANGLE_FAN; break;
        default:                      gl_prim = GL_TRIANGLES; break;
        }
    }

    if (prim == PSP_PRIM_POINTS) g_gl.PointSize((float)S);
    else if (prim == PSP_PRIM_LINES || prim == PSP_PRIM_LINE_STRIP) g_gl.LineWidth((float)S);

    /* Color / Alpha write masks (bit 1 = keep dst / mask off write). */
    const GLboolean wr = st->clear ? (st->clear_color ? GL_TRUE : GL_FALSE)
                                   : ((st->mask_rgb & 0x0000FFu) != 0x0000FFu);
    const GLboolean wg = st->clear ? (st->clear_color ? GL_TRUE : GL_FALSE)
                                   : ((st->mask_rgb & 0x00FF00u) != 0x00FF00u);
    const GLboolean wb = st->clear ? (st->clear_color ? GL_TRUE : GL_FALSE)
                                   : ((st->mask_rgb & 0xFF0000u) != 0xFF0000u);
    const GLboolean wa = st->clear ? (st->clear_alpha ? GL_TRUE : GL_FALSE)
                                   : ((st->mask_a & 0xFFu) != 0xFFu);
    g_gl.ColorMask(wr, wg, wb, wa);

    /* Backface culling (+y down in our unflipped NDC reverses screen winding). */
    if (!st->clear && !is_2d_prim && st->cull) {
        g_gl.Enable(GL_CULL_FACE);
        g_gl.CullFace(GL_BACK);
        g_gl.FrontFace(st->cull_ccw ? GL_CW : GL_CCW);
    } else {
        g_gl.Disable(GL_CULL_FACE);
    }

    /* Depth test & mask. */
    if (st->clear) {
        if (st->clear_depth) {
            g_gl.Enable(GL_DEPTH_TEST);
            g_gl.DepthFunc(GL_ALWAYS);
            g_gl.DepthMask(GL_TRUE);
        } else {
            g_gl.Disable(GL_DEPTH_TEST);
            g_gl.DepthMask(GL_FALSE);
        }
    } else if (st->ztest) {
        g_gl.Enable(GL_DEPTH_TEST);
        g_gl.DepthFunc(gl_cmp_func(st->zfunc));
        g_gl.DepthMask(st->zwrite ? GL_TRUE : GL_FALSE);
    } else {
        g_gl.Disable(GL_DEPTH_TEST);
        g_gl.DepthMask(GL_FALSE);
    }

    /* Stencil buffer mirrors PSP alpha-stencil for clear_alpha & STE primitives. */
    if (st->clear && st->clear_alpha) {
        const int aref = (int)((v[count - 1].rgba >> 24) & 0xFFu);
        g_gl.Enable(GL_STENCIL_TEST);
        g_gl.StencilMask(0xFFu);
        g_gl.StencilFunc(GL_ALWAYS, aref, 0xFFu);
        g_gl.StencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
    } else if (!st->clear && st->stest) {
        g_gl.Enable(GL_STENCIL_TEST);
        g_gl.StencilMask((st->mask_a & 0xFFu) != 0xFFu ? (~st->mask_a & 0xFFu) : 0xFFu);
        g_gl.StencilFunc(gl_cmp_func(st->sfunc), st->sref & 0xFF, (GLuint)(st->smask & 0xFF));
        g_gl.StencilOp(gl_stencil_op(st->sfail), gl_stencil_op(st->szfail), gl_stencil_op(st->szpass));
    } else {
        g_gl.Disable(GL_STENCIL_TEST);
        g_gl.StencilMask(0xFFu);
    }

    /* Blending (RGB channels blended per ALPHA/SFIX/DFIX; alpha untouched). */
    if (!st->clear && st->blend) {
        g_gl.Enable(GL_BLEND);
        const GLenum src_rgb = gl_blend_factor(st->blend_src, 1, st->blend_sfix);
        const GLenum dst_rgb = gl_blend_factor(st->blend_dst, 0, st->blend_dfix);
        if (src_rgb == GL_CONSTANT_COLOR || src_rgb == GL_ONE_MINUS_CONSTANT_COLOR) {
            g_gl.BlendColor((float)(st->blend_sfix & 0xFFu) / 255.0f,
                            (float)((st->blend_sfix >> 8) & 0xFFu) / 255.0f,
                            (float)((st->blend_sfix >> 16) & 0xFFu) / 255.0f, 1.0f);
        } else if (dst_rgb == GL_CONSTANT_COLOR || dst_rgb == GL_ONE_MINUS_CONSTANT_COLOR) {
            g_gl.BlendColor((float)(st->blend_dfix & 0xFFu) / 255.0f,
                            (float)((st->blend_dfix >> 8) & 0xFFu) / 255.0f,
                            (float)((st->blend_dfix >> 16) & 0xFFu) / 255.0f, 1.0f);
        }
        g_gl.BlendEquationSeparate(gl_blend_eq(st->blend_op), GL_FUNC_ADD);
        g_gl.BlendFuncSeparate(src_rgb, dst_rgb, GL_ONE, GL_ZERO);
    } else {
        g_gl.Disable(GL_BLEND);
    }

    /* Shader & uniforms. */
    g_gl.UseProgram(g_prog);
    g_gl.Uniform1i(u_clear_mode, st->clear ? 1 : 0);
    g_gl.Uniform1i(u_gouraud, st->gouraud ? 1 : 0);

    if (!st->clear && st->tex_enable) {
        int tw = 1, th = 1;
        g_gl.ActiveTexture(GL_TEXTURE0);
        GLuint tid = get_texture(st, &tw, &th);
        g_gl.Uniform1i(u_tex_enable, tid ? 1 : 0);
        g_gl.Uniform1i(u_tex, 0);
        g_gl.Uniform2f(u_tex_size, (float)tw, (float)th);
        g_gl.Uniform1i(u_tex_func, st->tex_func);
        g_gl.Uniform1i(u_tex_alpha, st->tex_alpha ? 1 : 0);
        g_gl.Uniform1f(u_tex_double, st->tex_double ? 2.0f : 1.0f);
        g_gl.Uniform4f(u_tex_env,
                       (float)(st->tex_env & 0xFFu) / 255.0f,
                       (float)((st->tex_env >> 8) & 0xFFu) / 255.0f,
                       (float)((st->tex_env >> 16) & 0xFFu) / 255.0f,
                       (float)((st->tex_env >> 24) & 0xFFu) / 255.0f);
    } else {
        g_gl.Uniform1i(u_tex_enable, 0);
    }

    g_gl.Uniform1i(u_atest, (!st->clear && st->atest) ? 1 : 0);
    g_gl.Uniform1i(u_atest_func, st->atest_func);
    g_gl.Uniform1i(u_atest_ref, st->atest_ref);
    g_gl.Uniform1i(u_atest_mask, st->atest_mask);

    /* Upload vertices & draw. */
    g_gl.BindVertexArray(g_vao);
    g_gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
    const size_t bytes = (size_t)emit_n * sizeof(gl_vert);
    if (bytes > g_vbo_cap) {
        size_t cap = g_vbo_cap ? g_vbo_cap : 16384u;
        while (cap < bytes) cap *= 2u;
        g_gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)cap, NULL, GL_STREAM_DRAW);
        g_vbo_cap = cap;
    } else {
        g_gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_vbo_cap, NULL, GL_STREAM_DRAW);
    }
    g_gl.BufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, g_vbuf);
    g_gl.DrawArrays(gl_prim, 0, emit_n);

    fb->has_draws = 1;
    psp_render_add_pixels((uint64_t)emit_n * (uint64_t)S * (uint64_t)S);
}

static void gl_finish(void) {
    if (!g_ready) return;
    if (ensure_context_current() == 0) {
        /* For single-buffered games or tests that never call sceDisplaySetFrameBuf
         * every frame, copy once at GE_FINISH if we drew directly to front FB. */
        if (!g_presented_this_list) {
            gl_fbo *front = find_fbo(psp_display_framebuffer());
            if (front && front->has_draws) {
                capture_fbo_to_front(front);
            }
        }
        g_presented_this_list = 0;
        sync_scale_if_needed();
        release_context_current();
    }
}

static void gl_present(void) {
    if (!g_ready) return;
    if (ensure_context_current() == 0) {
        gl_fbo *front = find_fbo(psp_display_framebuffer());
        if (front && front->has_draws) {
            capture_fbo_to_front(front);
            front->has_draws = 0;
        } else {
            /* CPU wrote VRAM directly without GL draws: let window fall back cleanly. */
            if (g_front_lock) {
                SDL_LockMutex(g_front_lock);
                g_front_valid = 0;
                SDL_UnlockMutex(g_front_lock);
            }
        }
        g_presented_this_list = 1;
        sync_scale_if_needed();
        release_context_current();
    }
}

const psp_render_backend psp_render_gl = {
    "gl", gl_init, gl_shutdown, gl_set_target, gl_draw, gl_finish, gl_present
};

#endif /* PSPRECOMP_SDL2_WINDOW */
