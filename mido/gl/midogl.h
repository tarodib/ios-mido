#pragma once
#include <stdbool.h>

struct midogl_ctx;

bool midogl_init(void);
struct midogl_ctx *midogl_create(int major, int minor);
bool midogl_make_current(struct midogl_ctx *c);
void midogl_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
