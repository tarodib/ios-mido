/* The context API eagl.c (adapted from ios-sailfish) expects; implemented in midogl.c. */
#ifndef SWEETGL_H
#define SWEETGL_H

#include <stdbool.h>

struct sweetgl_ctx;

bool sweetgl_init(void);
struct sweetgl_ctx *sweetgl_create(int api, struct sweetgl_ctx *share);
bool sweetgl_make_current(struct sweetgl_ctx *c);
void sweetgl_destroy(struct sweetgl_ctx *c);

struct pipe_screen *sweetgl_screen(void);
struct pipe_frontend_screen;
void sweetgl_image_hooks(struct pipe_frontend_screen *fscreen);
void sweetgl_eagl_install(void);

#endif
