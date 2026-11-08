/*
 * MidoGPU: user-space access to the Adreno 506 brought up by bsd/dev/mido_gpu.c.
 *
 * IOServiceMatching("MidoGPU") + IOServiceOpen(type 0). Buffer objects are physically
 * contiguous pages of the GPU pool; their GPU address is their physical address.
 * Map one with IOConnectMapMemory64(connect, handle, task, &addr, &size,
 * kIOMapAnywhere | kIOMapWriteCombineCache): write-combined (normal non-cacheable),
 * so the CPU and the non-coherent GPU agree without cache maintenance and unaligned
 * CPU accesses still work. Selectors (scalars in / out):
 *   0 GetParam    (which)                 -> (value)   0 chip id, 1 gpu id, 2 GMEM, 3 ready, 4 fence
 *   1 BONew       (size)                  -> (handle, gpu address, size)
 *   2 BOFree      (handle)
 *   3 Submit      (ib gpu address, dwords) -> (fence)
 *   4 Wait        (fence, timeout ms)      -> (0 done / 1 timed out)
 *   5 Log         struct in: text -> kernel log ("MIDO GPU USER: ...")
 * All buffers of a client are freed when it closes. Built in for mido (MIDO_BOARD).
 */
#include <pexpert/arm64/board_config.h>

#if defined(MIDO_BOARD)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <sys/proc.h>

extern "C" {
int mido_gpu_ready(void);
uint64_t mido_gpu_bo_alloc(uint64_t size);
void mido_gpu_bo_free(uint64_t pa, uint64_t size);
uint32_t mido_gpu_submit(uint64_t ib, uint32_t dwords);
int mido_gpu_wait(uint32_t seq, uint32_t timeout_ms);
uint32_t mido_gpu_param(uint32_t which);
bool mido_surface_info(uint32_t id, uint64_t *pa, uint64_t *size);
}

class MidoGPU : public IOService {
	OSDeclareDefaultStructors(MidoGPU);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(MidoGPU, IOService);

bool
MidoGPU::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "MidoGPUUserClient");
	registerService();
	IOLog("MIDO GPU: MidoGPU service registered\n");
	return true;
}

#define MIDO_GPU_MAX_BOS 4096

class MidoGPUUserClient : public IOUserClient {
	OSDeclareDefaultStructors(MidoGPUUserClient);
	struct bo { uint64_t pa, size; };
	bo *fBOs;
	IOLock *fLock;
public:
	bool initWithTask(task_t owningTask, void *securityToken, UInt32 type) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
	    IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(MidoGPUUserClient, IOUserClient);

bool
MidoGPUUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityToken, type)) {
		return false;
	}
	fBOs = IONewZero(bo, MIDO_GPU_MAX_BOS);
	fLock = IOLockAlloc();
	return fBOs != NULL && fLock != NULL;
}

void
MidoGPUUserClient::free(void)
{
	if (fBOs != NULL) {
		for (unsigned i = 0; i < MIDO_GPU_MAX_BOS; i++) {
			if (fBOs[i].size) {
				mido_gpu_bo_free(fBOs[i].pa, fBOs[i].size);
			}
		}
		IODelete(fBOs, bo, MIDO_GPU_MAX_BOS);
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
	}
	IOUserClient::free();
}

IOReturn
MidoGPUUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
MidoGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	if (type >= MIDO_GPU_MAX_BOS) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	bo b = fBOs[type];
	IOLockUnlock(fLock);
	if (!b.size) {
		return kIOReturnBadArgument;
	}
	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(b.pa, b.size, kIODirectionInOut);
	if (md == NULL) {
		return kIOReturnNoMemory;
	}
	*options = kIOMapWriteCombineCache;
	*memory = md;                                       /* the caller consumes this reference */
	return kIOReturnSuccess;
}

IOReturn
MidoGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	if (!mido_gpu_ready()) {
		return kIOReturnNotReady;
	}
	switch (selector) {
	case 0:
		if (a->scalarInputCount < 1 || a->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		a->scalarOutput[0] = mido_gpu_param((uint32_t)a->scalarInput[0]);
		return kIOReturnSuccess;
	case 1: {
		if (a->scalarInputCount < 1 || a->scalarOutputCount < 3 || a->scalarInput[0] == 0 ||
		    a->scalarInput[0] > 0x4000000) {
			return kIOReturnBadArgument;
		}
		uint64_t size = (a->scalarInput[0] + 0x3fff) & ~0x3fffULL;
		IOLockLock(fLock);
		unsigned h;
		for (h = 0; h < MIDO_GPU_MAX_BOS && fBOs[h].size; h++) {
		}
		if (h == MIDO_GPU_MAX_BOS) {
			IOLockUnlock(fLock);
			return kIOReturnNoResources;
		}
		uint64_t pa = mido_gpu_bo_alloc(size);
		if (!pa) {
			IOLockUnlock(fLock);
			return kIOReturnNoMemory;
		}
		fBOs[h].pa = pa;
		fBOs[h].size = size;
		IOLockUnlock(fLock);
		a->scalarOutput[0] = h;
		a->scalarOutput[1] = pa;
		a->scalarOutput[2] = size;
		return kIOReturnSuccess;
	}
	case 2: {
		if (a->scalarInputCount < 1 || a->scalarInput[0] >= MIDO_GPU_MAX_BOS) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		bo b = fBOs[a->scalarInput[0]];
		fBOs[a->scalarInput[0]].size = 0;
		IOLockUnlock(fLock);
		if (b.size) {
			mido_gpu_bo_free(b.pa, b.size);
		}
		return kIOReturnSuccess;
	}
	case 3: {
		if (a->scalarInputCount < 2 || a->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		uint32_t seq = mido_gpu_submit(a->scalarInput[0], (uint32_t)a->scalarInput[1]);
		a->scalarOutput[0] = seq;
		return seq ? kIOReturnSuccess : kIOReturnError;
	}
	case 4:
		if (a->scalarInputCount < 2 || a->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		a->scalarOutput[0] = mido_gpu_wait((uint32_t)a->scalarInput[0], (uint32_t)a->scalarInput[1]) ? 1 : 0;
		return kIOReturnSuccess;
	case 6: {                                           /* surface id -> gpu address, size */
		uint64_t pa = 0, size = 0;
		if (a->scalarInputCount < 1 || a->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		if (!mido_surface_info((uint32_t)a->scalarInput[0], &pa, &size)) {
			return kIOReturnNotFound;
		}
		a->scalarOutput[0] = pa;
		a->scalarOutput[1] = size;
		return kIOReturnSuccess;
	}
	case 5: {
		char text[256];
		uint32_t n = a->structureInputSize < sizeof(text) - 1 ? a->structureInputSize : sizeof(text) - 1;
		if (a->structureInput == NULL) {
			return kIOReturnBadArgument;
		}
		memcpy(text, a->structureInput, n);
		text[n] = 0;
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("MIDO GPU USER: %s: %s\n", name, text);
		return kIOReturnSuccess;
	}
	default:
		return kIOReturnUnsupported;
	}
}
#endif /* MIDO_BOARD */
