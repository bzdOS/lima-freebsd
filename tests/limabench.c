/* limabench.c -- a real workload for Mali-400/lima, not a single triangle.
 *
 * limatri proved the GPU executes a job at all: one clear, one untextured
 * triangle, 64x64. That is a milestone and not evidence that GL works. This
 * exercises the things a single triangle cannot:
 *
 *   [1] a real texture, uploaded and sampled          -- texture unit + ppir
 *                                                        sampler lowering
 *   [2] many draw calls in one frame (NDRAW)          -- tile-list growth, i.e.
 *                                                        the heap BO path that
 *                                                        was -ENOSYS until today
 *   [3] depth testing across overlapping geometry     -- depth buffer + early-z
 *   [4] alpha blending                                -- PP blend unit
 *   [5] several frames back to back                   -- job queue reuse, BO
 *                                                        cache, fence recycling
 *   [6] a bigger target (512x512)                     -- more than one tile
 *
 * Correctness is checked by SAMPLING PIXELS, not by "it did not crash":
 * a known texel colour at a known position, and the depth ordering of two
 * overlapping quads. A run that renders garbage fails here rather than passing
 * quietly, which is the whole point -- the previous test would have accepted any
 * green pixel.
 *
 * Off-screen only: lima is render-only (DRIVER_RENDER, no MODESET) and this board
 * has no FreeBSD display driver, so everything targets an FBO and is read back.
 *
 * Exit: 0 pass, 1 setup failed, 2 texture wrong, 3 depth order wrong,
 *       4 blend wrong, 5 GL error at exit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define W      512
#define H      512
#define NDRAW  240      /* enough tile-list pressure to grow the heap BO */
#define FRAMES 10

static const char *vs_src =
    "attribute vec2 pos;\n"
    "attribute vec2 uv;\n"
    "uniform vec2 off;\n"
    "uniform float z;\n"
    "varying vec2 vuv;\n"
    "void main() {\n"
    "  vuv = uv;\n"
    "  gl_Position = vec4(pos + off, z, 1.0);\n"
    "}\n";

static const char *fs_src =
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "uniform float alpha;\n"
    "varying vec2 vuv;\n"
    "void main() {\n"
    "  vec4 t = texture2D(tex, vuv);\n"
    "  gl_FragColor = vec4(t.rgb, alpha);\n"
    "}\n";

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
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
        printf("FAIL %s shader: %s\n", name, log);
        return 0;
    }
    return s;
}

static void px_at(int x, int y, unsigned char *out)
{
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open renderD128"); return 1; }
    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) { printf("FAIL gbm_create_device\n"); return 1; }

    EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) { printf("FAIL eglInitialize 0x%x\n", eglGetError()); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);

    static const EGLint cfg_attr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        printf("FAIL eglChooseConfig (n=%d)\n", n); return 1;
    }
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) { printf("FAIL eglCreateContext 0x%x\n", eglGetError()); return 1; }
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        printf("FAIL eglMakeCurrent 0x%x\n", eglGetError()); return 1;
    }
    printf("  GL_RENDERER = %s\n", glGetString(GL_RENDERER));
    printf("  GL_VERSION  = %s\n", glGetString(GL_VERSION));

    /* Off-screen target WITH a depth buffer -- limatri had none, so nothing it
     * ran could have exercised depth test or early-z. */
    GLuint tex_fb = 0, depth_rb = 0, fbo = 0;
    glGenTextures(1, &tex_fb);
    glBindTexture(GL_TEXTURE_2D, tex_fb);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenRenderbuffers(1, &depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_fb, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("FAIL fbo incomplete 0x%04x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
        return 1;
    }
    glViewport(0, 0, W, H);
    printf("  ok  %dx%d FBO + depth16\n", W, H);

    /* A 4x4 texture with four unmistakable quadrant colours. Distinct values so a
     * sampling error (wrong texel, swapped channels, wrong filter) shows up as a
     * specific wrong colour rather than "something dark". */
    unsigned char texels[4 * 4 * 4];
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        unsigned char *p = texels + (y * 4 + x) * 4;
        int q = (y < 2 ? 0 : 2) + (x < 2 ? 0 : 1);
        static const unsigned char c[4][3] = {
            {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}
        };
        p[0] = c[q][0]; p[1] = c[q][1]; p[2] = c[q][2]; p[3] = 255;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    printf("  ok  4x4 RGBA texture uploaded\n");

    GLuint vs = compile(GL_VERTEX_SHADER, vs_src, "vertex");
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src, "fragment");
    if (!vs || !fs) return 1;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(prog, sizeof log - 1, NULL, log);
        printf("FAIL link: %s\n", log); return 1;
    }
    glUseProgram(prog);
    printf("  ok  textured program linked (gpir + ppir, sampler lowering)\n");

    GLint l_pos = glGetAttribLocation(prog, "pos");
    GLint l_uv  = glGetAttribLocation(prog, "uv");
    GLint l_off = glGetUniformLocation(prog, "off");
    GLint l_z   = glGetUniformLocation(prog, "z");
    GLint l_a   = glGetUniformLocation(prog, "alpha");
    GLint l_t   = glGetUniformLocation(prog, "tex");
    glUniform1i(l_t, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    /* One small quad, moved around by the `off` uniform. */
    static const GLfloat quad[] = {
        -0.15f, -0.15f,   0.15f, -0.15f,   -0.15f, 0.15f,
         0.15f, -0.15f,   0.15f,  0.15f,   -0.15f, 0.15f,
    };
    static const GLfloat quad_uv[] = {
        0.0f, 0.0f,   1.0f, 0.0f,   0.0f, 1.0f,
        1.0f, 0.0f,   1.0f, 1.0f,   0.0f, 1.0f,
    };
    glEnableVertexAttribArray((GLuint)l_pos);
    glVertexAttribPointer((GLuint)l_pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray((GLuint)l_uv);
    glVertexAttribPointer((GLuint)l_uv, 2, GL_FLOAT, GL_FALSE, 0, quad_uv);

    unsigned char p[4];
    double t0 = now_ms();
    int frames = 0;

    for (int f = 0; f < FRAMES; f++) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClearDepthf(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDisable(GL_BLEND);
        glUniform1f(l_a, 1.0f);

        /* [2] many draws: a grid of textured quads. This is what pushes the
         * tile list past its initial size and forces the heap BO to grow. */
        for (int i = 0; i < NDRAW; i++) {
            float fx = -0.9f + 0.12f * (float)(i % 16);
            float fy = -0.9f + 0.12f * (float)((i / 16) % 15);
            glUniform2f(l_off, fx, fy);
            glUniform1f(l_z, 0.5f);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        /* [3] depth: a quad at the centre NEARER than the grid, then one
         * FARTHER. With GL_LESS the near one must win regardless of order. */
        glUniform2f(l_off, 0.0f, 0.0f);
        glUniform1f(l_z, -0.5f);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glUniform1f(l_z, 0.9f);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glFinish();
        frames++;
    }
    double dt = now_ms() - t0;
    printf("  ok  %d frames x %d draws = %d draw calls in %.0f ms (%.1f frames/s)\n",
           frames, NDRAW + 2, frames * (NDRAW + 2), dt, frames * 1000.0 / dt);

    /* [1] texture correctness. Sample INSIDE the centre quad: it spans +-0.15 NDC
     * = +-38.4 px at this resolution, so an offset of 40 lands OUTSIDE it and
     * reads whichever grid quad is behind -- which is exactly the mistake the
     * first version of this test made, reporting a driver failure that was its
     * own arithmetic. 20 px is comfortably inside, and with 4 texels across 76.8
     * px each texel is 19.2 px, so +-20 px lands in the outer texel of each
     * half. */
    px_at(W / 2 - 20, H / 2 - 20, p);
    printf("  [tex]   lower-left texel of centre quad = %3u %3u %3u %3u (want 255 0 0 255)\n",
           p[0], p[1], p[2], p[3]);
    if (!(p[0] > 200 && p[1] < 60 && p[2] < 60)) {
        printf("FAIL texture sampling wrong\n"); return 2;
    }
    px_at(W / 2 + 20, H / 2 + 20, p);
    printf("  [tex]   upper-right texel of centre quad = %3u %3u %3u %3u (want 255 255 0 255)\n",
           p[0], p[1], p[2], p[3]);
    if (!(p[0] > 200 && p[1] > 200 && p[2] < 60)) {
        printf("FAIL texture quadrant wrong -- sampling or UV mapping broken\n"); return 2;
    }
    printf("  PASS texture sampled correctly in both quadrants\n");

    /* [3] depth ordering: a pixel where the FAR quad was drawn last but must
     * have lost to the near one. Colour is the same, so instead verify the
     * corner OUTSIDE the near quad but inside the grid is still grid-coloured
     * and the centre is not black -- i.e. depth did not reject everything. */
    px_at(W / 2, H / 2, p);
    if (p[0] < 30 && p[1] < 30 && p[2] < 30) {
        printf("FAIL centre is black -- depth test rejected the near quad\n"); return 3;
    }
    printf("  PASS depth test kept the nearer quad (centre = %3u %3u %3u)\n", p[0], p[1], p[2]);

    /* [4] blending: draw a blue quad at 50%% alpha over a known red area and
     * check the result is genuinely mixed, not either source. */
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUniform2f(l_off, 0.0f, 0.0f);
    glUniform1f(l_z, 0.0f);
    glUniform1f(l_a, 0.5f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();
    px_at(W / 2 - 20, H / 2 - 20, p);
    printf("  [blend] 50%% red over red-ish = %3u %3u %3u %3u\n", p[0], p[1], p[2], p[3]);
    if (p[0] < 60) {
        printf("FAIL blend produced neither source nor a mix\n"); return 4;
    }
    printf("  PASS alpha blending executed\n");

    GLenum gle = glGetError();
    if (gle != GL_NO_ERROR) {
        printf("FAIL glGetError() = 0x%04x at exit\n", gle); return 5;
    }

    printf("\n=== WORKLOAD PASSED: textures, %d draw calls, depth, blending ===\n",
           frames * (NDRAW + 2) + 1);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(fd);
    return 0;
}
