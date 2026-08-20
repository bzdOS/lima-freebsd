/* limatri.c -- first-frame test for Mali-400 / lima on FreeBSD-under-Chimp.
 *
 * lima is a RENDER-ONLY driver (DRIVER_RENDER|GEM|SYNCOBJ, no DRIVER_MODESET) and
 * this board has no FreeBSD display driver, so there is no KMS and no on-screen
 * surface to render into. The honest test of "the GPU actually executed a job" is
 * therefore off-screen: bind a GBM device on the render node, get a surfaceless
 * EGL context, render into an FBO, and read the pixels back.
 *
 * Two stages, so a failure says WHICH part failed:
 *   [A] glClear to a known colour  -- exercises the PP (pixel processor) only.
 *   [B] draw a triangle            -- exercises GP (vertex/PLBU) *and* PP, i.e.
 *                                     both of lima's in-tree compilers (gpir/ppir)
 *                                     and a real tile-list build.
 *
 * Stage A passing alone would already be the first GPU job this project has ever
 * run. Stage B is the one that proves the whole pipeline.
 *
 * Exit codes: 0 all good, 1 setup failed, 2 stage A wrong pixels,
 *             3 stage B wrong pixels.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define W 64
#define H 64

static const char *vs_src =
    "attribute vec2 pos;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";

static const char *fs_src =
    "precision mediump float;\n"
    "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";

static void egl_err(const char *what)
{
    printf("FAIL %s: egl error 0x%04x\n", what, eglGetError());
}

static GLuint compile(GLenum type, const char *src, const char *name)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(s, sizeof log - 1, NULL, log);
        printf("FAIL %s shader compile: %s\n", name, log);
        return 0;
    }
    printf("  ok   %s shader compiled\n", name);
    return s;
}

int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open /dev/dri/renderD128"); return 1; }
    printf("  ok   opened /dev/dri/renderD128\n");

    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) { printf("FAIL gbm_create_device\n"); return 1; }
    printf("  ok   gbm_create_device, backend=%s\n", gbm_device_get_backend_name(gbm));

    EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
    if (dpy == EGL_NO_DISPLAY) { egl_err("eglGetDisplay"); return 1; }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) { egl_err("eglInitialize"); return 1; }
    printf("  ok   EGL %d.%d\n", major, minor);
    printf("       EGL_VENDOR   = %s\n", eglQueryString(dpy, EGL_VENDOR));
    printf("       EGL_VERSION  = %s\n", eglQueryString(dpy, EGL_VERSION));

    if (!eglBindAPI(EGL_OPENGL_ES_API)) { egl_err("eglBindAPI"); return 1; }

    /* Surfaceless: no window system, so no EGLSurface at all. */
    /* Deliberately does NOT constrain EGL_SURFACE_TYPE. Asking for
     * EGL_PBUFFER_BIT matched nothing here: the GBM platform advertises
     * window-type configs (for gbm_surfaces), and eglChooseConfig then returned
     * TRUE with zero configs and EGL_SUCCESS -- a "no match", not an error.
     * Since everything below renders into an FBO and the context is surfaceless,
     * the config's surface type is irrelevant. */
    static const EGLint cfg_attr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        egl_err("eglChooseConfig");
        return 1;
    }

    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) { egl_err("eglCreateContext"); return 1; }
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        egl_err("eglMakeCurrent (surfaceless)");
        return 1;
    }
    printf("  ok   surfaceless context current\n");
    printf("       GL_VENDOR    = %s\n", glGetString(GL_VENDOR));
    printf("       GL_RENDERER  = %s\n", glGetString(GL_RENDERER));
    printf("       GL_VERSION   = %s\n", glGetString(GL_VERSION));

    /* Off-screen target. */
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("FAIL framebuffer incomplete (0x%04x)\n",
               glCheckFramebufferStatus(GL_FRAMEBUFFER));
        return 1;
    }
    glViewport(0, 0, W, H);
    printf("  ok   %dx%d FBO complete\n", W, H);

    unsigned char px[4];

    /* ---- Stage A: PP-only clear ---- */
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);       /* red */
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("  [A] clear  -> centre pixel = %3u %3u %3u %3u (want 255 0 0 255)\n",
           px[0], px[1], px[2], px[3]);
    if (!(px[0] > 200 && px[1] < 60 && px[2] < 60)) {
        printf("FAIL stage A: PP clear did not land\n");
        return 2;
    }
    printf("  PASS stage A -- the PP executed a job\n");

    /* ---- Stage B: GP + PP, a real triangle ---- */
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src, "vertex");
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src, "fragment");
    if (!vs || !fs) return 1;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(prog, sizeof log - 1, NULL, log);
        printf("FAIL program link: %s\n", log);
        return 1;
    }
    printf("  ok   program linked (gpir + ppir ran)\n");
    glUseProgram(prog);

    /* A triangle that definitely covers the centre. */
    static const GLfloat verts[] = {
        -0.9f, -0.9f,
         0.9f, -0.9f,
         0.0f,  0.9f,
    };
    GLint loc = glGetAttribLocation(prog, "pos");
    glEnableVertexAttribArray((GLuint)loc);
    glVertexAttribPointer((GLuint)loc, 2, GL_FLOAT, GL_FALSE, 0, verts);

    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);       /* blue background */
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("  [B] triangle -> centre pixel = %3u %3u %3u %3u (want 0 255 0 255)\n",
           px[0], px[1], px[2], px[3]);
    if (!(px[1] > 200 && px[0] < 60 && px[2] < 60)) {
        printf("FAIL stage B: triangle did not rasterise (centre is not green)\n");
        return 3;
    }
    printf("  PASS stage B -- GP built a tile list and PP rasterised it\n");

    GLenum gle = glGetError();
    if (gle != GL_NO_ERROR)
        printf("  note glGetError() = 0x%04x at exit\n", gle);

    printf("\n=== FIRST FRAME: Mali-400 rendered a triangle ===\n");
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(fd);
    return 0;
}
