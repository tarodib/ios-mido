/*
 * midogl.c: Mesa freedreno on iOS without EGL. Creates the gallium screen on the fake
 * DRM descriptor from midogl_drm.c and OpenGL ES contexts through Mesa's state tracker
 * API; contexts are surfaceless (render into framebuffer objects).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "pipe/p_screen.h"
#include "frontend/api.h"
#include "state_tracker/st_context.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "freedreno/drm/freedreno_drm_public.h"
#include "renderonly/renderonly.h"

#include "midogl.h"

void midogl_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int midogl_open(void);

static const driOptionDescription midogl_driconf[] = {
#include "pipe-loader/driinfo_gallium.h"
#include "freedreno/driinfo_freedreno.h"
};

/* Only used with a KMS scanout device, which we never pass. */
void
renderonly_scanout_destroy(struct renderonly_scanout *s, struct renderonly *ro)
{
	(void)s;
	(void)ro;
}

struct renderonly_scanout *
renderonly_create_gpu_import_for_resource(struct pipe_resource *r, struct renderonly *ro,
    struct winsys_handle *h)
{
	(void)r;
	(void)ro;
	(void)h;
	return NULL;
}

/* The state tracker asks this unconditionally; 0 = no quirks. */
static int
fs_get_param(struct pipe_frontend_screen *fs, enum st_manager_param param)
{
	(void)fs;
	(void)param;
	return 0;
}

static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pipe_frontend_screen *fscreen;
static driOptionCache opt_info, opts;

bool
midogl_init(void)
{
	pthread_mutex_lock(&init_lock);
	if (fscreen == NULL) {
		int fd = midogl_open();
		if (fd < 0) {
			midogl_log("no MidoGPU service");
		} else {
			/* Direct rendering to memory: no GMEM tile resolve past surface ends. */
			setenv("FD_MESA_DEBUG", "sysmem", 0);
			driParseOptionInfo(&opt_info, midogl_driconf, ARRAY_SIZE(midogl_driconf));
			struct pipe_screen_config cfg = {.options = &opts, .options_info = &opt_info};
			struct pipe_screen *screen = fd_drm_screen_create_renderonly(fd, NULL, &cfg);
			if (screen == NULL) {
				midogl_log("freedreno screen creation failed");
			} else {
				fscreen = calloc(1, sizeof(*fscreen));
				fscreen->screen = screen;
				fscreen->get_param = fs_get_param;
				midogl_log("screen: %s / %s", screen->get_name(screen), screen->get_vendor(screen));
			}
		}
	}
	bool ok = fscreen != NULL;
	pthread_mutex_unlock(&init_lock);
	return ok;
}

struct midogl_ctx {
	struct st_context *st;
};

struct midogl_ctx *
midogl_create(int major, int minor)
{
	if (!midogl_init()) {
		return NULL;
	}
	struct st_context_attribs attribs = {
		.profile = API_OPENGLES2,
		.major = major,
		.minor = minor,
	};
	enum st_context_error err = ST_CONTEXT_SUCCESS;
	struct st_context *st = st_api_create_context(fscreen, &attribs, &err, NULL);
	if (st == NULL) {
		midogl_log("GLES %d.%d context failed: %d", major, minor, err);
		return NULL;
	}
	struct midogl_ctx *c = calloc(1, sizeof(*c));
	c->st = st;
	return c;
}

bool
midogl_make_current(struct midogl_ctx *c)
{
	return st_api_make_current(c ? c->st : NULL, NULL, NULL);
}
