/* Names used by eagl.c (adapted from ios-sailfish), implemented by midogl. */
#ifndef SWEET_DRM_H
#define SWEET_DRM_H

#include <stdint.h>
#include "midogl.h"

#define sweet_drm_log midogl_log

/* A GEM handle for an IOSurface's memory: 0 or -errno. */
int sweet_drm_import_iosurface(uint32_t surface_id, uint64_t map, uint32_t *handle, uint64_t *size);

#endif
