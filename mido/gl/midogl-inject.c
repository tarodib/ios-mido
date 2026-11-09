/*
 * midogl-inject: loaded into backboardd (boot-arg
 * midoenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/local/bin/midogl-test). Installs the
 * EAGLContext replacement so CoreAnimation's GLES renderer runs on Mesa.
 *
 * Also a sampling profiler for backboardd: about 200 times a second every running thread
 * is suspended just long enough to read its pc (symbol lookup happens after resume, so a
 * thread holding the dyld or malloc lock cannot deadlock us). Every 10 s it logs the top
 * symbols, the top images and per-thread CPU time ("prof:" lines in the kernel log).
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "midogl.h"
#include "sweetgl.h"

extern const char *getprogname(void);
extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t size);

#define PROF_SYMS    2048
#define PROF_IMGS    64
#define PROF_THREADS 128
#define PROF_TOP     14

struct prof_sym {
	uintptr_t key;          /* symbol address, or the pc's 4 KB page without a symbol */
	const char *img, *sym;
	uintptr_t off;          /* key - image base, to find it in the shared cache */
	uint32_t n;
};
struct prof_img {
	const char *img;
	uint32_t n;
};
struct prof_thr {
	thread_act_t port;
	uint64_t last_ns, delta_ns;
	char name[24];
	uint32_t seen;
};

static struct prof_sym prof_syms[PROF_SYMS];
static struct prof_img prof_imgs[PROF_IMGS];
static struct prof_thr prof_thrs[PROF_THREADS];
static uint32_t prof_samples, prof_ticks, prof_round;

static const char *
prof_base(const char *path)
{
	const char *s = path ? strrchr(path, '/') : NULL;
	return s ? s + 1 : path ? path : "?";
}

static void
prof_add(uintptr_t pc)
{
	Dl_info di;
	uintptr_t key = pc & ~(uintptr_t)0xfff, off = 0;
	const char *img = "?", *sym = NULL;

	memset(&di, 0, sizeof(di));
	if (dladdr((void *)pc, &di)) {
		img = prof_base(di.dli_fname);
		if (di.dli_saddr != NULL) {
			key = (uintptr_t)di.dli_saddr;
			sym = di.dli_sname;
		}
		off = key - (uintptr_t)di.dli_fbase;
	}
	uint32_t h = (uint32_t)((key >> 2) * 2654435761u) % PROF_SYMS;
	for (uint32_t i = 0; i < PROF_SYMS; i++) {
		struct prof_sym *e = &prof_syms[(h + i) % PROF_SYMS];
		if (e->key == key) {
			e->n++;
			break;
		}
		if (e->key == 0) {
			e->key = key;
			e->img = img;
			e->sym = sym;
			e->off = off;
			e->n = 1;
			break;
		}
	}
	for (uint32_t i = 0; i < PROF_IMGS; i++) {
		if (prof_imgs[i].img == img || prof_imgs[i].img == NULL) {
			prof_imgs[i].img = img;
			prof_imgs[i].n++;
			break;
		}
	}
	prof_samples++;
}

static void
prof_threads_cpu(void)
{
	thread_act_array_t th;
	mach_msg_type_number_t nth;

	if (task_threads(mach_task_self(), &th, &nth) != KERN_SUCCESS) {
		return;
	}
	prof_round++;
	for (mach_msg_type_number_t i = 0; i < nth; i++) {
		struct thread_extended_info xi;
		mach_msg_type_number_t cnt = THREAD_EXTENDED_INFO_COUNT;
		if (thread_info(th[i], THREAD_EXTENDED_INFO, (thread_info_t)&xi, &cnt) == KERN_SUCCESS) {
			uint64_t ns = xi.pth_user_time + xi.pth_system_time;
			struct prof_thr *t = NULL;
			for (uint32_t j = 0; j < PROF_THREADS && t == NULL; j++) {
				if (prof_thrs[j].port == th[i]) {
					t = &prof_thrs[j];
				}
			}
			for (uint32_t j = 0; j < PROF_THREADS && t == NULL; j++) {
				if (prof_thrs[j].port == 0 || prof_thrs[j].seen + 2 < prof_round) {
					t = &prof_thrs[j];
					t->port = th[i];
					t->last_ns = ns;
				}
			}
			if (t != NULL) {
				t->delta_ns = ns - t->last_ns;
				t->last_ns = ns;
				t->seen = prof_round;
				strlcpy(t->name, xi.pth_name[0] ? xi.pth_name : "-", sizeof(t->name));
			}
		}
		mach_port_deallocate(mach_task_self(), th[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)th, nth * sizeof(*th));
}

static void
prof_report(double secs)
{
	midogl_log("prof: %u samples in %.1f s (%.2f threads running on average)",
	    prof_samples, secs, prof_ticks ? (double)prof_samples / prof_ticks : 0.0);
	for (int k = 0; k < PROF_TOP; k++) {
		struct prof_sym *best = NULL;
		for (uint32_t i = 0; i < PROF_SYMS; i++) {
			if (prof_syms[i].n != 0 && (best == NULL || prof_syms[i].n > best->n)) {
				best = &prof_syms[i];
			}
		}
		if (best == NULL) {
			break;
		}
		midogl_log("prof: %5.1f%% %s %s +0x%lx", 100.0 * best->n / (prof_samples ? prof_samples : 1),
		    best->img, best->sym ? best->sym : "(no symbol)", (unsigned long)best->off);
		best->n = 0;
	}
	char line[400];
	int n = snprintf(line, sizeof(line), "prof: images");
	for (int k = 0; k < 8; k++) {
		struct prof_img *best = NULL;
		for (uint32_t i = 0; i < PROF_IMGS; i++) {
			if (prof_imgs[i].n != 0 && (best == NULL || prof_imgs[i].n > best->n)) {
				best = &prof_imgs[i];
			}
		}
		if (best == NULL || n >= (int)sizeof(line) - 1) {
			break;
		}
		n += snprintf(line + n, sizeof(line) - n, " %s %.0f%%", best->img,
		    100.0 * best->n / (prof_samples ? prof_samples : 1));
		best->n = 0;
	}
	midogl_log("%s", line);
	prof_threads_cpu();
	n = snprintf(line, sizeof(line), "prof: thread CPU");
	for (int k = 0; k < 8; k++) {
		struct prof_thr *best = NULL;
		for (uint32_t i = 0; i < PROF_THREADS; i++) {
			struct prof_thr *t = &prof_thrs[i];
			if (t->port != 0 && t->seen == prof_round && t->delta_ns != 0 &&
			    (best == NULL || t->delta_ns > best->delta_ns)) {
				best = t;
			}
		}
		if (best == NULL || n >= (int)sizeof(line) - 1) {
			break;
		}
		n += snprintf(line + n, sizeof(line) - n, " %s:%x %.0f%%", best->name, best->port,
		    100.0 * best->delta_ns / (secs * 1e9));
		best->delta_ns = 0;     /* printed; last_ns keeps the running total */
	}
	midogl_log("%s", line);
	memset(prof_syms, 0, sizeof(prof_syms));
	memset(prof_imgs, 0, sizeof(prof_imgs));
	prof_samples = prof_ticks = 0;
}

static uint64_t
prof_now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void *
prof_thread(void *arg)
{
	(void)arg;
	pthread_setname_np("midogl-prof");
	sleep(20);
	prof_threads_cpu();     /* baseline for the per-thread CPU deltas */
	thread_act_t self = mach_thread_self();
	uint64_t window = prof_now_ns();
	for (;;) {
		usleep(5000);
		thread_act_array_t th;
		mach_msg_type_number_t nth;
		uintptr_t pcs[32];
		int np = 0;
		if (task_threads(mach_task_self(), &th, &nth) == KERN_SUCCESS) {
			for (mach_msg_type_number_t i = 0; i < nth; i++) {
				struct thread_basic_info bi;
				mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
				if (th[i] != self && np < 32 &&
				    thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &cnt) == KERN_SUCCESS &&
				    bi.run_state == TH_STATE_RUNNING && thread_suspend(th[i]) == KERN_SUCCESS) {
					arm_thread_state64_t st;
					cnt = ARM_THREAD_STATE64_COUNT;
					if (thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
						pcs[np++] = (uintptr_t)(st.__pc & 0x0000007fffffffffull);
					}
					thread_resume(th[i]);
				}
				mach_port_deallocate(mach_task_self(), th[i]);
			}
			vm_deallocate(mach_task_self(), (vm_address_t)th, nth * sizeof(*th));
		}
		for (int i = 0; i < np; i++) {
			prof_add(pcs[i]);
		}
		prof_ticks++;
		uint64_t now = prof_now_ns();
		if (now - window >= 10000000000ull) {
			if (prof_samples >= 50) {
				prof_report((now - window) / 1e9);
			} else {
				memset(prof_syms, 0, sizeof(prof_syms));
				memset(prof_imgs, 0, sizeof(prof_imgs));
				prof_samples = prof_ticks = 0;
				prof_threads_cpu();
			}
			window = now;
		}
	}
	return NULL;
}

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
	pthread_t t;
	if (pthread_create(&t, NULL, prof_thread, NULL) == 0) {
		pthread_detach(t);
	}
}
