/*
 * midogl-inject: loaded into backboardd (boot-arg
 * midoenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/local/bin/midogl-test). Installs the
 * EAGLContext replacement so CoreAnimation's GLES renderer runs on Mesa.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "midogl.h"
#include "sweetgl.h"

extern const char *getprogname(void);
extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t size);

__attribute__((constructor))
static void
midogl_inject_init(void)
{
	if (strcmp(getprogname(), "backboardd") != 0) {
		return;
	}
	struct { int32_t a; uint32_t aa; int32_t i; uint32_t ia; } lim = { 2048, 0, 2048, 0 };
	memorystatus_control(7 /* SET_MEMLIMIT_PROPERTIES */, getpid(), 0, &lim, sizeof(lim));
	midogl_open();
	midogl_log("inject: loaded in backboardd, pid %d, installing EAGL", getpid());
	sweetgl_eagl_install();
}
