/*
 * midogl-inject: the library loaded into backboardd (boot-arg
 * midoenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/local/bin/midogl-test).
 * Step 4a: prove it loads, and report what Core Animation has to work with:
 * EAGLContext, Metal, and whether Mesa comes up inside this process.
 */
#include <dlfcn.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <GLES2/gl2.h>

#include "midogl.h"

extern const char *getprogname(void);
extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t size);

static void *
probe_thread(void *arg)
{
	(void)arg;
	sleep(20);
	midogl_open();                         /* the log goes through the GPU connection */
	midogl_log("inject: loaded in %s, pid %d", getprogname(), getpid());
	void *(*mtl)(void) = dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice");
	midogl_log("inject: %s: EAGLContext %p, CAContext %p, MTLCreateSystemDefaultDevice %p -> %p",
	    getprogname(), (void *)objc_getClass("EAGLContext"), (void *)objc_getClass("CAContext"),
	    (void *)mtl, mtl ? mtl() : NULL);
	struct { int32_t a; uint32_t aa; int32_t i; uint32_t ia; } lim = { 1024, 0, 1024, 0 };
	memorystatus_control(7 /* SET_MEMLIMIT_PROPERTIES */, getpid(), 0, &lim, sizeof(lim));
	if (!midogl_init()) {
		midogl_log("inject: Mesa init failed");
		return NULL;
	}
	struct midogl_ctx *c = midogl_create(2, 0);
	if (!c || !midogl_make_current(c)) {
		midogl_log("inject: no context");
		return NULL;
	}
	midogl_log("inject: GL inside %s: %s", getprogname(), glGetString(GL_VERSION));
	midogl_make_current(NULL);
	return NULL;
}

__attribute__((constructor))
static void
midogl_inject_init(void)
{
	if (strcmp(getprogname(), "backboardd") != 0) {
		return;
	}
	pthread_t t;
	pthread_create(&t, NULL, probe_thread, NULL);
	pthread_detach(t);
}
