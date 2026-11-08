/*
 * midogl-test: first real OpenGL ES through Mesa freedreno on iOS. Compiles a shader
 * pair, draws one triangle into a 256x256 texture, reads the pixels back. Reports in
 * the kernel log ("MIDO GPU USER: midogl-test: ...").
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <GLES2/gl2.h>

#include "midogl.h"

static const char vs[] =
    "attribute vec2 pos;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char fs[] =
    "precision mediump float;\n"
    "void main() { gl_FragColor = vec4(1.0, 0.5, 0.0, 1.0); }\n";

static GLuint
shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[200] = "";
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		midogl_log("shader compile failed: %s", log);
		return 0;
	}
	return s;
}

/* iOS kills daemons over a small per-process limit (reason 7): Mesa needs more. */
extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t size);
#define MEMORYSTATUS_CMD_SET_JETSAM_TASK_LIMIT 6

int
main(void)
{
	sleep(5);
	int r6 = memorystatus_control(MEMORYSTATUS_CMD_SET_JETSAM_TASK_LIMIT, getpid(), 1024, NULL, 0);
	int e6 = errno;
	int r5 = memorystatus_control(5 /* SET_JETSAM_HIGH_WATER_MARK */, getpid(), 1024, NULL, 0);
	int e5 = errno;
	if (!midogl_init()) {
		return 1;
	}
	midogl_log("memory limit calls: task %d errno %d, hwm %d errno %d, uid %d", r6, e6, r5, e5, getuid());
	struct midogl_ctx *c = midogl_create(2, 0);
	if (!c || !midogl_make_current(c)) {
		midogl_log("no context");
		return 1;
	}
	midogl_log("GL: %s | %s | %s", glGetString(GL_VERSION), glGetString(GL_RENDERER), glGetString(GL_VENDOR));

	GLuint tex, fbo;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	midogl_log("framebuffer status %x", st);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		return 1;
	}

	GLuint vsh = shader(GL_VERTEX_SHADER, vs), fsh = shader(GL_FRAGMENT_SHADER, fs);
	if (!vsh || !fsh) {
		return 1;
	}
	GLuint p = glCreateProgram();
	GLint linked = 0;
	glAttachShader(p, vsh);
	glAttachShader(p, fsh);
	glBindAttribLocation(p, 0, "pos");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &linked);
	midogl_log("shaders compiled, link status %d", linked);
	if (!linked) {
		return 1;
	}

	static const GLfloat tri[] = { -0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f };
	glViewport(0, 0, 256, 256);
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(p);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
	glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	midogl_log("draw issued, GL error %x", glGetError());
	glFinish();

	unsigned char px[256 * 256 * 4];
	glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, px);
	unsigned tri_px = 0, clear_px = 0, other = 0;
	for (unsigned i = 0; i < 256 * 256; i++) {
		const unsigned char *q = px + i * 4;
		if (q[0] > 240 && q[1] > 110 && q[1] < 145 && q[2] < 16) {
			tri_px++;
		} else if (q[0] < 16 && q[1] < 16 && q[2] > 240) {
			clear_px++;
		} else {
			other++;
		}
	}
	const unsigned char *m = px + (128 * 256 + 128) * 4, *e = px + (4 * 256 + 4) * 4;
	midogl_log("center %02x%02x%02x%02x corner %02x%02x%02x%02x: triangle %u clear %u other %u",
	    m[0], m[1], m[2], m[3], e[0], e[1], e[2], e[3], tri_px, clear_px, other);
	midogl_log("%s", tri_px > 10000 && clear_px > 10000 ? "OPENGL ES TRIANGLE DRAWN BY THE GPU" : "TRIANGLE TEST FAILED");
	return 0;
}
