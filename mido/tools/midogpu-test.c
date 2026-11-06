/*
 * midogpu-test: user-space test of the MidoGPU IOKit interface (mido/xnu/src/MidoGPU.cpp).
 * Runs once at boot as a launchd job (mido/tools/org.mido.gpu-test.plist) and reports
 * through the user client's Log selector, so the result lands in the kernel log.
 *   1. GetParam: chip id / GMEM / ready
 *   2. three buffer objects (source, destination, command buffer), mapped write-combined
 *   3. CPU draws a gradient into the source; the command buffer holds the same 2D blit
 *      setup as the kernel self-test (Mesa fd5_blitter emit_setup + one CP_BLIT)
 *   4. Submit + Wait, then the CPU compares all 65536 pixels.
 * Build (macOS, iOS SDK): see mido/tools/build-ios-tools.sh. Ad-hoc signed.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>

/* IOKit.framework has no public headers in the iOS SDK: declare what we use. */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
typedef int kern_return_t_;
extern const mach_port_t kIOMainPortDefault;
extern void *IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t main, void *matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOConnectCallScalarMethod(io_connect_t c, uint32_t sel, const uint64_t *in, uint32_t inCnt,
    uint64_t *out, uint32_t *outCnt);
extern kern_return_t IOConnectCallStructMethod(io_connect_t c, uint32_t sel, const void *in, size_t inSize,
    void *out, size_t *outSize);
extern kern_return_t IOConnectMapMemory64(io_connect_t c, uint32_t type, task_port_t task,
    mach_vm_address_t *addr, mach_vm_size_t *size, uint32_t options);
#define kIOMapAnywhere           0x00000001
#define kIOMapWriteCombineCache  0x00000400

static io_connect_t conn;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	char buf[240];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("%s\n", buf);
	if (conn) {
		IOConnectCallStructMethod(conn, 5, buf, strlen(buf), NULL, NULL);
	}
}

struct bo { uint64_t handle, gpu, size; volatile uint32_t *cpu; };

static int bo_new(struct bo *b, uint64_t size)
{
	uint64_t in = size, out[3];
	uint32_t n = 3;
	kern_return_t kr = IOConnectCallScalarMethod(conn, 1, &in, 1, out, &n);
	if (kr) {
		say("BONew(%llu) failed 0x%x", size, kr);
		return -1;
	}
	b->handle = out[0];
	b->gpu = out[1];
	b->size = out[2];
	mach_vm_address_t addr = 0;
	mach_vm_size_t len = 0;
	kr = IOConnectMapMemory64(conn, (uint32_t)b->handle, mach_task_self(), &addr, &len,
	    kIOMapAnywhere | kIOMapWriteCombineCache);
	if (kr) {
		say("map of BO %llu failed 0x%x", b->handle, kr);
		return -1;
	}
	b->cpu = (volatile uint32_t *)addr;
	return 0;
}

static uint32_t par(uint32_t v)
{
	v ^= v >> 16; v ^= v >> 8; v ^= v >> 4;
	return (0x9669u >> (v & 0xf)) & 1u;
}
static uint32_t pkt7(uint32_t op, uint32_t cnt)
{
	return (7u << 28) | cnt | (par(cnt) << 15) | ((op & 0x7fu) << 16) | (par(op) << 23);
}
static uint32_t pkt4(uint32_t reg, uint32_t cnt)
{
	return (4u << 28) | cnt | (par(cnt) << 7) | ((reg & 0x3ffffu) << 8) | (par(reg) << 27);
}

int main(void)
{
	sleep(5);
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("MidoGPU"));
	if (!svc) {
		printf("midogpu-test: no MidoGPU service\n");
		return 1;
	}
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	if (kr) {
		printf("midogpu-test: IOServiceOpen 0x%x\n", kr);
		return 1;
	}
	uint64_t in, val;
	uint32_t n = 1;
	in = 3;
	kr = IOConnectCallScalarMethod(conn, 0, &in, 1, &val, &n);
	if (kr || !val) {
		say("GPU not ready (0x%x %llu)", kr, val);
		return 1;
	}
	in = 0; n = 1;
	IOConnectCallScalarMethod(conn, 0, &in, 1, &val, &n);
	say("opened MidoGPU, chip id %08llx", val);

	struct bo src, dst, ib;
	if (bo_new(&src, 256 * 1024) || bo_new(&dst, 256 * 1024) || bo_new(&ib, 16384)) {
		return 1;
	}
	say("BOs: src %llx dst %llx ib %llx (cpu %p %p %p)", src.gpu, dst.gpu, ib.gpu,
	    (void *)src.cpu, (void *)dst.cpu, (void *)ib.cpu);
	for (uint32_t y = 0; y < 256; y++) {
		for (uint32_t x = 0; x < 256; x++) {
			src.cpu[y * 256 + x] = (x << 16) | (y << 8) | 0x80u;
			dst.cpu[y * 256 + x] = 0x5a5a5a5au;
		}
	}
	const uint32_t fmt = 0x30, size = (1024 >> 6) | ((1024 * 256 >> 6) << 16);
	uint32_t k = 0;
#define IB(v) (ib.cpu[k++] = (v))
	IB(pkt4(0x0e91, 5)); IB(0); IB(0); IB(0); IB(0); IB(0x12);
	IB(pkt7(0x26, 0));
	IB(pkt7(0x6c, 5)); IB(1); IB(0); IB(0); IB(0); IB(0);
	IB(pkt7(0x1d, 1)); IB(0);
	IB(pkt7(0x46, 1)); IB(0x19);
	IB(pkt4(0xe3b0, 1)); IB(0);
	IB(pkt4(0xe4f0, 1)); IB(0);
	IB(pkt7(0x26, 0));
	IB(pkt4(0x0cc7, 1)); IB(0x10000000);
	IB(pkt4(0xe141, 1)); IB(0x00000008);
	IB(pkt4(0x2100, 1)); IB(0x86000000);
	IB(pkt4(0x2180, 1)); IB(0x86000000);
	IB(pkt4(0x2184, 1)); IB(0x00000009);
	IB(pkt4(0xe140, 1)); IB(1u << 17);
	IB(pkt4(0x0cc6, 1)); IB(0x00000004);
	IB(pkt4(0x0ec2, 1)); IB(0x0000000c);
	IB(pkt4(0x0f02, 1)); IB(0x00000344);
	IB(pkt4(0x0e06, 1)); IB(0x00000002);
	IB(pkt4(0xe000, 1)); IB(0x00000181);
	IB(pkt7(0x6c, 1)); IB(5);
	IB(pkt4(0x2107, 9)); IB(fmt); IB((uint32_t)src.gpu); IB((uint32_t)(src.gpu >> 32)); IB(size);
	IB(0); IB(0); IB(0); IB(0); IB(0);
	IB(pkt4(0x2181, 1)); IB(fmt);
	IB(pkt4(0x2110, 9)); IB(fmt); IB((uint32_t)dst.gpu); IB((uint32_t)(dst.gpu >> 32)); IB(size);
	IB(0); IB(0); IB(0); IB(0); IB(0);
	IB(pkt4(0x2182, 1)); IB(fmt);
	IB(pkt7(0x2c, 5)); IB(1); IB(0); IB(255 | (255u << 16)); IB(0); IB(255 | (255u << 16));
	IB(pkt7(0x6c, 1)); IB(8);
	IB(pkt7(0x26, 0));
#undef IB
	__sync_synchronize();

	uint64_t sin[2] = { ib.gpu, k }, fence;
	n = 1;
	kr = IOConnectCallScalarMethod(conn, 3, sin, 2, &fence, &n);
	if (kr) {
		say("Submit failed 0x%x", kr);
		return 1;
	}
	uint64_t win[2] = { fence, 2000 }, timedout = 1;
	n = 1;
	IOConnectCallScalarMethod(conn, 4, win, 2, &timedout, &n);
	uint32_t same = 0;
	for (uint32_t i = 0; i < 256 * 256; i++) {
		same += dst.cpu[i] == src.cpu[i];
	}
	say("submit fence %llu %s, %u of 65536 pixels match -> %s", fence,
	    timedout ? "TIMED OUT" : "signalled", same,
	    same == 65536 ? "USER-SPACE GPU SUBMISSION WORKS" : "FAILED");
	IOServiceClose(conn);
	return same == 65536 ? 0 : 1;
}
