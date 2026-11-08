/*
 * Minimal libdrm API for Mesa freedreno on iOS (mido). There is no DRM on iOS: these
 * functions are implemented in midogl_drm.c on top of the MidoGPU IOKit user client.
 * The types come from Mesa's own no-libdrm fallback, util/libdrm.h.
 */
#ifndef MIDO_XF86DRM_H
#define MIDO_XF86DRM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ioccom.h>
#include "drm-uapi/drm.h"

/* util/libdrm.h would define inline stubs of these two: keep them out of the way. */
#define drmGetVersion mido_unused_drmGetVersion
#define drmFreeVersion mido_unused_drmFreeVersion
#include "util/libdrm.h"
#undef drmGetVersion
#undef drmFreeVersion

#ifdef __cplusplus
extern "C" {
#endif

drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr v);
int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);

#ifdef __cplusplus
}
#endif
#endif
