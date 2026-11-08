/*
 * midogl_drm.c: the libdrm/MSM kernel interface Mesa's freedreno driver expects,
 * implemented on top of the MidoGPU IOKit user client (mido/xnu/src/MidoGPU.cpp).
 *
 * Reports itself as "msm" 1.7: softpin (no relocations, a submit is a list of command
 * buffers), no cached-coherent BOs (a5xx). GPU addresses are physical (GEM_INFO IOVA).
 * Buffers are mapped write-combined and zeroed when created: Linux GEM memory arrives
 * zeroed, the MidoGPU pool does not. DRM handle = MidoGPU handle + 1 (0 is invalid);
 * the "mmap offset" of a buffer is its handle shifted left by 16.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>

#include <xf86drm.h>
#include "drm-uapi/msm_drm.h"

/* IOKit has no public headers in the iOS SDK. */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern const mach_port_t kIOMainPortDefault;
extern void *IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t main, void *matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOConnectCallScalarMethod(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt,
    uint64_t *out, uint32_t *outCnt);
extern kern_return_t IOConnectCallStructMethod(io_connect_t c, uint32_t sel, const void *in, size_t inSize,
    void *out, size_t *outSize);
extern kern_return_t IOConnectMapMemory64(io_connect_t c, uint32_t type, task_port_t task,
    mach_vm_address_t *addr, mach_vm_size_t *size, uint32_t options);
extern kern_return_t IOConnectUnmapMemory64(io_connect_t c, uint32_t type, task_port_t task, mach_vm_address_t addr);
#define kIOMapAnywhere           0x00000001
#define kIOMapWriteCombineCache  0x00000400

#define MAX_BOS 4096
#define MGPU_PARAM_READY 3

struct mbo {
	uint64_t gpu, size, cpu;
	uint32_t fence;                     /* last submit that used it */
	int used;
};

static io_connect_t conn;
static struct mbo bos[MAX_BOS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void midogl_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void
midogl_log(const char *fmt, ...)
{
	char buf[240];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(stderr, "midogl: %s\n", buf);
	if (conn) {
		IOConnectCallStructMethod(conn, 5, buf, strlen(buf), NULL, NULL);
	}
}

/* Opens MidoGPU; returns a descriptor to hand to Mesa, or -1. */
int
midogl_open(void)
{
	if (!conn) {
		io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("MidoGPU"));
		if (!svc || IOServiceOpen(svc, mach_task_self(), 0, &conn)) {
			conn = 0;
			return -1;
		}
	}
	return open("/dev/null", O_RDWR);
}

static struct mbo *
lookup(uint32_t handle)
{
	if (handle == 0 || handle > MAX_BOS || !bos[handle - 1].used) {
		return NULL;
	}
	return &bos[handle - 1];
}

/* Kernel fence comparison is wrap-safe; 0 = passed, 1 = still pending. */
static int
fence_wait(uint32_t fence, uint32_t timeout_ms)
{
	uint64_t in[2] = { fence, timeout_ms }, timedout = 1;
	uint32_t n = 1;
	if (fence == 0) {
		return 0;
	}
	if (IOConnectCallScalarMethod(conn, 4, in, 2, &timedout, &n)) {
		return 1;
	}
	return timedout ? 1 : 0;
}

static uint32_t
remaining_ms(const struct drm_msm_timespec *abs)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t ms = (abs->tv_sec - now.tv_sec) * 1000 + (abs->tv_nsec - now.tv_nsec) / 1000000;
	if (ms <= 0) {
		return 0;
	}
	return ms > 60000 ? 60000 : (uint32_t)ms;
}

static int
gem_new(struct drm_msm_gem_new *r)
{
	uint64_t in = r->size, out[3];
	uint32_t n = 3;
	kern_return_t kr = IOConnectCallScalarMethod(conn, 1, &in, 1, out, &n);
	if (kr) {
		midogl_log("BONew(%llu) failed 0x%x", (unsigned long long)r->size, kr);
		return -ENOMEM;
	}
	mach_vm_address_t addr = 0;
	mach_vm_size_t len = 0;
	kr = IOConnectMapMemory64(conn, (uint32_t)out[0], mach_task_self(), &addr, &len,
	    kIOMapAnywhere | kIOMapWriteCombineCache);
	if (kr) {
		midogl_log("map of BO %llu failed 0x%x", (unsigned long long)out[0], kr);
		IOConnectCallScalarMethod(conn, 2, out, 1, NULL, NULL);
		return -ENOMEM;
	}
	memset((void *)addr, 0, out[2]);
	pthread_mutex_lock(&lock);
	struct mbo *b = &bos[out[0]];
	b->gpu = out[1];
	b->size = out[2];
	b->cpu = addr;
	b->fence = 0;
	b->used = 1;
	pthread_mutex_unlock(&lock);
	r->handle = (uint32_t)out[0] + 1;
	return 0;
}

static int
gem_close(uint32_t handle)
{
	pthread_mutex_lock(&lock);
	struct mbo *b = lookup(handle);
	if (!b) {
		pthread_mutex_unlock(&lock);
		return -EINVAL;
	}
	uint64_t cpu = b->cpu, h = handle - 1;
	b->used = 0;
	pthread_mutex_unlock(&lock);
	IOConnectUnmapMemory64(conn, (uint32_t)h, mach_task_self(), cpu);
	uint32_t n = 0;
	IOConnectCallScalarMethod(conn, 2, &h, 1, NULL, &n);
	return 0;
}

void *
midogl_bo_map(int fd, uint64_t offset, size_t size)
{
	(void)fd;
	(void)size;
	pthread_mutex_lock(&lock);
	struct mbo *b = lookup((uint32_t)(offset >> 16));
	void *p = b ? (void *)b->cpu : NULL;
	pthread_mutex_unlock(&lock);
	return p;
}

static int
gem_info(struct drm_msm_gem_info *r)
{
	pthread_mutex_lock(&lock);
	struct mbo *b = lookup(r->handle);
	int ret = 0;
	if (!b) {
		ret = -EINVAL;
	} else if (r->info == MSM_INFO_GET_OFFSET) {
		r->value = (uint64_t)r->handle << 16;
	} else if (r->info == MSM_INFO_GET_IOVA) {
		r->value = b->gpu;
	} else if (r->info == MSM_INFO_GET_FLAGS) {
		r->value = MSM_BO_WC;
	} else if (r->info != MSM_INFO_SET_NAME && r->info != MSM_INFO_GET_NAME &&
	    r->info != MSM_INFO_SET_METADATA && r->info != MSM_INFO_GET_METADATA) {
		ret = -EINVAL;
	}
	pthread_mutex_unlock(&lock);
	return ret;
}

static int
gem_submit(struct drm_msm_gem_submit *r)
{
	const struct drm_msm_gem_submit_bo *sbos = (const void *)(uintptr_t)r->bos;
	const struct drm_msm_gem_submit_cmd *cmds = (const void *)(uintptr_t)r->cmds;
	uint32_t fence = 0;
	for (uint32_t i = 0; i < r->nr_cmds; i++) {
		if (cmds[i].type != MSM_SUBMIT_CMD_BUF || cmds[i].submit_idx >= r->nr_bos) {
			continue;
		}
		pthread_mutex_lock(&lock);
		struct mbo *b = lookup(sbos[cmds[i].submit_idx].handle);
		uint64_t gpu = b ? b->gpu + cmds[i].submit_offset : 0;
		pthread_mutex_unlock(&lock);
		if (!gpu) {
			return -EINVAL;
		}
		uint64_t in[2] = { gpu, cmds[i].size / 4 }, seq = 0;
		uint32_t n = 1;
		kern_return_t kr = IOConnectCallScalarMethod(conn, 3, in, 2, &seq, &n);
		if (kr) {
			midogl_log("Submit failed 0x%x", kr);
			return -EIO;
		}
		fence = (uint32_t)seq;
	}
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < r->nr_bos; i++) {
		struct mbo *b = lookup(sbos[i].handle);
		if (b) {
			b->fence = fence;
		}
	}
	pthread_mutex_unlock(&lock);
	r->fence = fence;
	r->fence_fd = -1;
	return 0;
}

static int
get_param(struct drm_msm_param *r)
{
	uint64_t in, val = 0;
	uint32_t n = 1;
	switch (r->param) {
	case MSM_PARAM_GPU_ID:
		in = 1;
		break;
	case MSM_PARAM_GMEM_SIZE:
		in = 2;
		break;
	case MSM_PARAM_CHIP_ID:
		in = 0;
		break;
	case MSM_PARAM_GMEM_BASE:
		r->value = 0x00100000;
		return 0;
	case MSM_PARAM_MAX_FREQ:
		r->value = 19200000;
		return 0;
	case MSM_PARAM_PRIORITIES:
		r->value = 1;
		return 0;
	case MSM_PARAM_TIMESTAMP:
	case MSM_PARAM_FAULTS:
	case MSM_PARAM_SUSPENDS:
		r->value = 0;
		return 0;
	case MSM_PARAM_UCHE_TRAP_BASE:
		r->value = 0x0001ffffffff0000ULL;
		return 0;
	default:
		return -EINVAL;
	}
	if (IOConnectCallScalarMethod(conn, 0, &in, 1, &val, &n)) {
		return -EIO;
	}
	r->value = val;
	return 0;
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	(void)size;
	switch (index) {
	case DRM_MSM_GET_PARAM:
		return get_param(data);
	case DRM_MSM_SET_PARAM:
		return 0;
	case DRM_MSM_GEM_NEW:
		return gem_new(data);
	case DRM_MSM_GEM_INFO:
		return gem_info(data);
	case DRM_MSM_GEM_SUBMIT:
		return gem_submit(data);
	case DRM_MSM_GEM_MADVISE:
		((struct drm_msm_gem_madvise *)data)->retained = 1;
		return 0;
	case DRM_MSM_SUBMITQUEUE_NEW:
		((struct drm_msm_submitqueue *)data)->id = 1;
		return 0;
	case DRM_MSM_SUBMITQUEUE_QUERY: {
		struct drm_msm_submitqueue_query *q = data;
		if (q->data && q->len) {
			memset((void *)(uintptr_t)q->data, 0, q->len);
		}
		return 0;
	}
	default:
		return -EINVAL;
	}
}

int
drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	(void)size;
	switch (index) {
	case DRM_MSM_GEM_INFO:
		return gem_info(data);
	case DRM_MSM_GEM_CPU_PREP: {
		struct drm_msm_gem_cpu_prep *r = data;
		pthread_mutex_lock(&lock);
		struct mbo *b = lookup(r->handle);
		uint32_t fence = b ? b->fence : 0;
		pthread_mutex_unlock(&lock);
		if (!b) {
			return -EINVAL;
		}
		uint32_t ms = (r->op & MSM_PREP_NOSYNC) ? 0 : remaining_ms(&r->timeout);
		if (fence_wait(fence, ms)) {
			return (r->op & MSM_PREP_NOSYNC) ? -EBUSY : -ETIMEDOUT;
		}
		return 0;
	}
	case DRM_MSM_WAIT_FENCE: {
		struct drm_msm_wait_fence *r = data;
		return fence_wait(r->fence, remaining_ms(&r->timeout)) ? -ETIMEDOUT : 0;
	}
	case DRM_MSM_SUBMITQUEUE_CLOSE:
		return 0;
	default:
		return -EINVAL;
	}
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	if (request == DRM_IOCTL_GEM_CLOSE) {
		int ret = gem_close(((struct drm_gem_close *)arg)->handle);
		if (ret) {
			errno = -ret;
			return -1;
		}
		return 0;
	}
	errno = ENOSYS;
	return -1;
}

drmVersionPtr
drmGetVersion(int fd)
{
	(void)fd;
	drmVersionPtr v = calloc(1, sizeof(*v));
	if (v) {
		v->version_major = 1;
		v->version_minor = 7;
		v->name = strdup("msm");
		v->name_len = 3;
	}
	return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
	if (v) {
		free(v->name);
		free(v);
	}
}

/* No other process shares GPU buffers yet: everything else is unavailable. */
int drmOpenWithType(const char *n, const char *b, int t) { (void)n; (void)b; (void)t; return -1; }
int drmGetCap(int fd, uint64_t c, uint64_t *v) { (void)fd; (void)c; *v = 0; return -EINVAL; }
int drmPrimeHandleToFD(int fd, uint32_t h, uint32_t f, int *p) { (void)fd; (void)h; (void)f; (void)p; return -ENOSYS; }
int drmPrimeFDToHandle(int fd, int p, uint32_t *h) { (void)fd; (void)p; (void)h; return -ENOSYS; }
int drmSyncobjDestroy(int fd, uint32_t h) { (void)fd; (void)h; return -ENOSYS; }
int drmSyncobjFDToHandle(int fd, int o, uint32_t *h) { (void)fd; (void)o; (void)h; return -ENOSYS; }
int drmSyncobjReset(int fd, const uint32_t *h, uint32_t c) { (void)fd; (void)h; (void)c; return -ENOSYS; }
int drmSyncobjImportSyncFile(int fd, uint32_t h, int s) { (void)fd; (void)h; (void)s; return -ENOSYS; }
int drmSyncobjExportSyncFile(int fd, uint32_t h, int *s) { (void)fd; (void)h; (void)s; return -ENOSYS; }
