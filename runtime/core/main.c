/*
 * Native host for a recompiled Vita executable (recompiler/recomp.py output): the proof-of-boot
 * runner. Maps the 4 GiB guest arena, loads the module's memory image (recompiler/mkimage.py),
 * gives it a stack and calls module_start natively. Every import goes through hle_call(): the
 * ones in hle.c run, every other one is logged by name with its arguments and returns 0, so a run
 * shows exactly which system calls the game makes, in order, and how far it gets.
 *
 * usage: recomp_host <image.img> <module_start hex> [max import calls]
 * env:   RECOMP_LOG=n   print the first n import calls in full (default 400)
 *        RECOMP_LOG_FROM=n   and every call from the n-th on
 */
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include "stubret.h"
#include "gaps.h"   /* GAPS-01 */
#include "seedaddr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "rt.h"
#include "events.h"
#include "platform.h"
#include "hle.h"
#include "gfx_coverage.h"
#include "settings.h"
#include <SDL3/SDL_filesystem.h>
void window_run(void *(*fn)(void *), void *arg);

uint8_t *recomp_mem;
uint32_t tls_template[3];   /* the module's TLS: template address, template size, block size (mkimage.py) */
__thread uint32_t recomp_excl_addr, recomp_excl_val;   /* rt.h's exclusive monitor */
__thread int recomp_excl_valid;
__thread uint64_t recomp_excl_val64;   /* ldrexd/strexd */
__thread uint32_t recomp_switch_seq, recomp_excl_seq;   /* P0b diag: context switches since thread start / at the ldrex */
__thread int recomp_excl_drop_reason;                   /* P0b diag: the last monitor drop site's tag (rt.h) */
__thread uint32_t recomp_tpidruro;      /* the guest thread's TLS pointer */
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;   /* the allocator and the call log */
static void prof_report(void);   /* N01b-11 diagnostic: defined with the profiler (or empty) below */
static uint64_t ncalls, budget = 200000, nlog = 400, log_from;

/* the Vita runs user code in RunFast mode (flush-to-zero, default NaN): the same on each host
 * thread that runs guest code (the FP control register is per thread) */
void recomp_runfast(void) {
#if defined(__aarch64__)
	uint64_t f; __asm__ volatile("mrs %0, fpcr" : "=r"(f)); f |= (1u << 24) | (1u << 25);
	__asm__ volatile("msr fpcr, %0" :: "r"(f));
#elif defined(__x86_64__)
	uint32_t m; __asm__ volatile("stmxcsr %0" : "=m"(m)); m |= 0x8040; __asm__ volatile("ldmxcsr %0" :: "m"(m));
#endif
}

/* ---- the guest's memory: everything below GUEST_HEAP is the image's; above it, a bump
 * allocator hands out memory blocks, thread stacks and host-made objects */
#define GUEST_HEAP 0x10000000u
#define GUEST_HEAP_END 0x80000000u
static uint32_t heap_top = GUEST_HEAP;
/* the guest memory in use: the loaded images' segments and the heap so far (gfx_capture.c snapshots these) */
#define MAX_SEGS 512   /* (64 before: a game with dozens of on-demand modules, 2-3 segments each, filled it) */
static uint32_t seg_lo[MAX_SEGS], seg_hi[MAX_SEGS]; static int nseg;
static const char *seg_img[MAX_SEGS];   /* each segment's image file name (GAPS: which image a missed function is in) */
int guest_mem_ranges(uint32_t lo[], uint32_t hi[], int max) {
	int n = 0;
	for (int i = 0; i < nseg && n < max - 1; i++) { lo[n] = seg_lo[i]; hi[n] = seg_hi[i]; n++; }
	lo[n] = GUEST_HEAP; hi[n] = heap_top; n++;
	return n;
}
uint32_t guest_alloc(uint32_t size, uint32_t align) {
	if (align < 16) align = 16;
	pthread_mutex_lock(&host_lock);
	uint32_t a = (heap_top + align - 1) & ~(align - 1);
	if (a + size > GUEST_HEAP_END || a + size < a) { fprintf(stderr, "guest heap exhausted (%#x)\n", size); exit(3); }
	heap_top = a + size;
	pthread_mutex_unlock(&host_lock);
	return a;
}

/* ---- calls out of recompiled code */
typedef struct { const char *name; uint64_t n; } Count;
static Count counts[1024];
static int ncounts;
static void count(const char *name) {
	for (int i = 0; i < ncounts; i++) if (counts[i].name == name) { counts[i].n++; return; }
	if (ncounts < 1024) counts[ncounts++] = (Count){name, 1};
}
static int by_n(const void *a, const void *b) {
	const Count *x = a, *y = b; return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}
/* each host thread's last import (where it is when a run ends) */
typedef struct { pthread_t th; const char *name; uint32_t lr, r0, r1; uint64_t n, at; } Last;
static Last lasts[64];
static int nlasts;
static __thread Last *my_last;
static void summary_threads(void) {
	fprintf(stderr, "== threads: last import call\n");
	for (int i = 0; i < nlasts; i++)
		fprintf(stderr, "  thread %d: %llu calls, last #%llu %s(%#x, %#x) from %#x\n", i, (unsigned long long)lasts[i].n,
		        (unsigned long long)lasts[i].at, lasts[i].name, lasts[i].r0, lasts[i].r1, lasts[i].lr);
}
void host_summary(const char *why);   /* window.c: the import-call histogram when the window is closed */
static void summary(const char *why) {
	ev_log("stop", "%s", why);
	ev_dump(why);   /* the bounded event tail, correlated with the input timeline (N02g-3) */
	qsort(counts, ncounts, sizeof *counts, by_n);
	fprintf(stderr, "\n== %s after %llu import calls; %d distinct:\n", why, (unsigned long long)ncalls, ncounts);
	for (int i = 0; i < ncounts; i++)
		fprintf(stderr, "  %8llu  %s%s\n", (unsigned long long)counts[i].n, counts[i].name,
		        hle_find(counts[i].name) ? "" : "   (stub)");
	summary_threads();
	{ extern void kernel_report_locks(void); kernel_report_locks(); }   /* who holds what the waiting threads want */
	{ extern void kernel_report_obj_counts(void); kernel_report_obj_counts(); }   /* SignalSema/SetEvent per object (the loader gates) */
	{ extern void gxm_sony_report(void); gxm_sony_report(); }   /* RECOMP_GXM_CHECK's findings */
#ifdef RECOMP_VERIFY
	extern void verify_summary(void), coverage_write(void), watch_summary(void);
	verify_summary();
	coverage_write();
	watch_summary();
#endif
}
void host_summary(const char *why) { summary(why); }

/* each import stub address -> its implementation, resolved once (hle_find searches by name) */
/* ---- modules loaded with the program (recomp.py --module: their tables register here before
 * main runs; their images are RECOMP_MODULES). An import goes to the module exporting it (as on
 * the console), except the plain memory/string functions the host does faster with the same result */
typedef struct { const char *name; GuestFn (*lookup)(uint32_t); const char *(*import_name)(uint32_t); int (*is_arm)(uint32_t); } Module;
#define MAX_MODULES 128   /* start-up MODULES + the ones a game loads on demand (LOAD_MODULES, modload.c) */
static Module modules[MAX_MODULES];
static int nmodules;
void recomp_register_module(const char *name, GuestFn (*lookup)(uint32_t), const char *(*import_name)(uint32_t), int (*is_arm)(uint32_t)) {
	if (nmodules < MAX_MODULES) modules[nmodules++] = (Module){name, lookup, import_name, is_arm};
	else fprintf(stderr, "module %s: more than %d modules, not registered\n", name, MAX_MODULES);
}
GuestFn guest_lookup(uint32_t a) {
	GuestFn f = recomp_lookup(a);
	for (int i = 0; !f && i < nmodules; i++) f = modules[i].lookup(a);
	return f;
}
static const char *import_name_any(uint32_t a) {
	const char *n = recomp_import_name(a);
	for (int i = 0; !n && i < nmodules; i++) n = modules[i].import_name(a);
	return n;
}
#define EXPORT_NAME 160   /* (the longest known export name is 96 characters) */
static struct { uint32_t addr; char name[EXPORT_NAME]; } *exports;   /* every loaded module's (images' EXPT / EXPL records) */
static uint32_t nexports;
/* (a module loaded on demand adds or drops exports while game threads resolve imports: the list is read and changed
 * under this lock; resolution is cached, so it is taken once per import, not per call) */
static pthread_mutex_t exports_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t export_addr(const char *name) {
	uint32_t a = 0;
	pthread_mutex_lock(&exports_lock);
	for (uint32_t i = 0; i < nexports; i++) if (exports[i].addr && !strcmp(exports[i].name, name)) { a = exports[i].addr; break; }
	pthread_mutex_unlock(&exports_lock);
	return a;
}
uint32_t module_export_addr(const char *name) { return export_addr(name); }   /* a loaded module's export by name (0: none) */
/* GXM is Sony's own code (MODULES="... libgxm_es4 libgpu_es4"): every GXM call of the game goes to
 * the module. The rendering boundary below still involves the host: its observer (gxm_sony.c) runs
 * first -- reading Sony's context and drawing through the host's renderer -- and then Sony's function
 * runs, keeping the context's own state (scenes, ring buffers, sync) exactly as on the console; what
 * Sony's code then submits to the GPU reaches gpu.c, which completes it. (The host's own GXM state
 * tracking, gxm.c, was retired on 2026-10-06 once every game rendered the same on both.) */
/* RECOMP_LOG_GXM=1: every GXM call the game makes, its first four arguments and its result, whichever
 * side handles it -- diffing a host-routed run against a Sony-routed one finds the first call that differs */
/* RECOMP_LOG_FROM_FRAME=n: the import log (RECOMP_LOG_FROM) and the GXM log start only once frame n
 * has been presented -- a stall in Uncharted's menu, 3000 frames in, needed the calls of its last
 * frames without the millions before */
static int log_frame_ok(void) {
	static long from = -2; if (from == -2) from = getenv("RECOMP_LOG_FROM_FRAME") ? atol(getenv("RECOMP_LOG_FROM_FRAME")) : -1;
	if (from < 0) return 1;
	extern uint64_t display_frames(void);
	return display_frames() >= (uint64_t)from;
}
static int log_gxm(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMP_LOG_GXM"); v = e && *e == '1'; } return v && log_frame_ok(); }
static int gxm_boundary(const char *n) {
	static const char *const b[] = {"sceGxmDraw", "sceGxmDrawPrecomputed", "sceGxmDrawInstanced", "sceGxmBeginScene", "sceGxmShaderPatcherRegisterProgram",
		"sceGxmBeginSceneEx", "sceGxmEndScene", "sceGxmMidSceneFlush", "sceGxmDisplayQueueAddEntry", "sceGxmDisplayQueueFinish",
		"sceGxmFinish", "sceGxmNotificationWait", "sceGxmWaitEvent", "sceGxmTransferCopy", "sceGxmTransferDownscale",
		"sceGxmTransferFill", "sceGxmTransferFinish", "sceGxmSetRegionClip", "sceGxmSetVisibilityBuffer", "sceGxmSetFrontVisibilityTestEnable",
		"sceGxmSetFrontVisibilityTestIndex", "sceGxmSetFrontVisibilityTestOp", "sceGxmSetFrontDepthBias",
		"sceGxmBeginCommandList", "sceGxmEndCommandList", "sceGxmExecuteCommandList", 0};
	for (int i = 0; b[i]; i++) if (!strcmp(b[i], n)) return 1;
	return 0;
}
#ifdef RECOMP_VERIFY
/* for the in-place checker (verify.c), which follows a call into a loaded module's code on both
 * sides: the import any image's stub stands for; the address of the module export it reaches (0
 * for an import only the host implements, for the GXM rendering boundary, and for __tls_get_addr,
 * whose answers the host owns); and whether code at an address of any image is ARM (not Thumb) */
const char *recomp_import_name_any(uint32_t a) { return import_name_any(a); }
uint32_t recomp_export_addr(const char *n) { return export_addr(n); }
uint32_t recomp_module_target(uint32_t stub) {
	static struct { uint32_t stub, target; } cache[1024];
	uint32_t h = (stub >> 1) & 1023;
	if (cache[h].stub == stub) return cache[h].target;
	const char *n = import_name_any(stub);
	uint32_t t = !n || gxm_boundary(n) || !strcmp(n, "__tls_get_addr") ? 0 : export_addr(n);
	cache[h].stub = stub; cache[h].target = t;
	return t;
}
int recomp_is_arm_any(uint32_t a) {
	extern int recomp_is_arm(uint32_t a);
	if (recomp_lookup(a)) return recomp_is_arm(a);
	for (int i = 0; i < nmodules; i++) if (modules[i].lookup(a)) return modules[i].is_arm(a);
	return 0;
}
#endif
HleFn gxm_observer(const char *name);   /* gxm_sony.c */
static int host_preferred(const char *n) {
	static const char *const fast[] = {"memcpy", "memmove", "memset", "memcmp", "memchr", "strlen", "strnlen", "strcmp",
		"strncmp", "strcpy", "strncpy", "strcat", "strncat", "strchr", "strrchr", "strstr", "sceClibMemcpy", "sceClibMemset",
		"sceClibMemmove", "sceClibMemcmp", "sceClibStrcmp", "sceClibStrncmp", "sceClibStrnlen",
		/* libc's thread-local lookup walks the kernel's thread TLS layout (which it learns from
		 * sceKernelGetModuleInfoByAddr at start-up); the host owns the threads and their TLS blocks,
		 * so its own __tls_get_addr answers (Killzone keeps its per-thread Thread object there) */
		"__tls_get_addr", 0};
	for (int i = 0; fast[i]; i++) if (!strcmp(fast[i], n)) return 1;
	/* RECOMP_SKIP_MOVIES=1: the host's SceAvPlayer (apputil.c), which refuses every source, over the loaded module's -- the
	 * game's own no-movie path skips its videos (a test that only needs the game, not its cutscene films) */
	static int skip = -1; if (skip < 0) { const char *e = getenv("RECOMP_SKIP_MOVIES"); skip = e && *e == '1'; }
	if (skip && !strncmp(n, "sceAvPlayer", 11)) return 1;
	/* Sony's np_common, recompiled for its matchmaking module (KZ_NP_MODULES), also exports the NP auth calls and the
	 * environment: the host keeps them -- its own sign-in tickets (np.c) and the retail "np" (np_common's own is set by a
	 * shell service that isn't there, "rc" without it) */
	if (!strncmp(n, "sceNpAuth", 9) || !strcmp(n, "sceNpCommonGetNpEnviroment")) return 1;
	/* np_matching2's TLS to this copy's own matching service: plain bytes (ssl_local.c); to a chosen server, Sony's libssl */
	extern int matching_local(void);
	if ((!strncmp(n, "SceSslInternal_", 15) || !strncmp(n, "sceSslInternal", 14)) && matching_local()) return 1;
	return 0;
}
/* A function pointer built from an import stub's address (movw + movt, the way a game names a function for a callback or a task
 * table): when the import is another loaded module's export, the pointer is that function. The Vita's loader gives such a value
 * the exporter's address; kept as the stub's own address (a slot of this module's import table), a game that compares it with the
 * function it knows (God Eater's save task: Proc == saveTask) finds two different values. Calls through the stub still reach the
 * export either way (recomp_call_external). Imports the host provides (system calls, host-preferred routines) stay as they were. */
uint32_t recomp_stub_ptr(uint32_t v) {
	const char *n = import_name_any(v & ~1u);
	if (!n || gxm_boundary(n) || host_preferred(n)) return v;
	uint32_t t = export_addr(n);
	if (!t) return v;
	if (!(t & 1)) {   /* an export's address without its Thumb bit: Thumb unless its image says ARM */
		extern int recomp_is_arm(uint32_t a); int arm = 0;
		if (recomp_lookup(t)) arm = recomp_is_arm(t);
		else for (int i = 0; i < nmodules; i++) if (modules[i].lookup(t)) { arm = modules[i].is_arm(t); break; }
		if (!arm) t |= 1;
	}
	{ static int said; if (said++ < 24) fprintf(stderr, "    stub pointer %#x (%s) -> %#x\n", v, n, t); }
	return t;
}

#define RESOLVE_SLOTS 4096
static struct { uint32_t addr; HleFn fn; uint32_t target; unsigned flags; int known; } resolved[RESOLVE_SLOTS];
/* what recomp_call_external asks of an import's name, worked out once with its resolution: it compared the name against
 * a list of formatting functions (and setjmp, longjmp) on every call, 1.9% of Uncharted's main thread (2026-11-01) */
enum { RF_FMT = 1, RF_SETJMP = 2, RF_LONGJMP = 4, RF_GXM = 8 };
static unsigned name_flags(const char *n) {
	static const char *const fmts[] = {"sprintf", "snprintf", "vsprintf", "vsnprintf", "sceClibSnprintf", "sceClibVsnprintf", 0};
	unsigned f = 0;
	for (int i = 0; fmts[i]; i++) if (!strcmp(fmts[i], n)) f |= RF_FMT;   /* (the formatting functions: the buffer is the first argument) */
	if (!strcmp(n, "setjmp") || !strcmp(n, "_setjmp")) f |= RF_SETJMP;
	if (!strcmp(n, "longjmp") || !strcmp(n, "_longjmp")) f |= RF_LONGJMP;
	if (!strncmp(n, "sceGxm", 6)) f |= RF_GXM;
	return f;
}
/* an import: the host's implementation, or (*target) a loaded module's export -- which with a host
 * function as well (the GXM boundary's observers) runs after it; *flags: its name's RF_ bits */
static HleFn hle_resolve(uint32_t addr, const char *name, uint32_t *target, unsigned *flags) {
	uint32_t h = (addr >> 4) & (RESOLVE_SLOTS - 1);
	for (uint32_t k = 0; k < RESOLVE_SLOTS; k++, h = (h + 1) & (RESOLVE_SLOTS - 1)) {
		if (__atomic_load_n(&resolved[h].known, __ATOMIC_ACQUIRE) && resolved[h].addr == addr) { *target = resolved[h].target; *flags = resolved[h].flags; return resolved[h].fn; }   /* acquire: pairs with the release below, so addr/fn/target are seen filled */
		if (!resolved[h].known) {   /* benign race: two threads may both resolve the same import */
			HleFn fn = hle_find(name);
			uint32_t t = fn && host_preferred(name) ? 0 : export_addr(name);
			if (t) fn = gxm_boundary(name) ? gxm_observer(name) : 0;   /* a module's export: the host adds only the GXM boundary's observers */
			else if (!fn && !strncmp(name, "sceGxm", 6)) {   /* no libgxm loaded: the game's GXM calls have nowhere to go */
				static int said; if (!said) { said = 1; fprintf(stderr, "%s: GXM is Sony's code -- add libgxm_es4 libgpu_es4 to the game's MODULES (packs/<title>/pack.toml)\n", name); }
			}
			resolved[h].addr = addr; resolved[h].fn = fn; resolved[h].target = t; resolved[h].flags = name_flags(name);
			__atomic_store_n(&resolved[h].known, 1, __ATOMIC_RELEASE);
			*target = t; *flags = resolved[h].flags;
			return fn;
		}
	}
	*target = 0; *flags = name_flags(name);
	return hle_find(name);
}

/* Guest setjmp/longjmp. A recompiled function is a C function and its caller's frame is the host's, so a
 * longjmp has to unwind the host stack too: at each call to a setjmp import, recomp.py emits a host
 * _setjmp in the calling function itself, into the record this keeps for the guest's buffer (with the
 * registers that call had: r4-r11, sp, lr, d8-d15, fpscr -- what a longjmp gives back); a guest longjmp
 * puts them back, r0 = its value (1 for 0), and host-_longjmps into that frame, which carries on after the
 * call. Sony's own longjmp, recompiled, restored the registers and then just returned to its caller: Lua
 * 5.1 raises every script error with longjmp, so any script error ran on with another frame's registers
 * and crashed (Uncharted's rope swing at 60 fps, 2026-10-13). Records are kept per guest buffer address
 * (each thread's buffers are on its own stack): a longjmp to a buffer no setjmp here filled falls back to
 * the old path, with a warning. */
enum { NJMP = 4096 };
static RecompJmp jmps[NJMP];
static pthread_mutex_t jmp_lock = PTHREAD_MUTEX_INITIALIZER;
static RecompJmp *jmp_find(uint32_t g, int make) {   /* (jmp_lock held) */
	uint32_t h = (g >> 3) * 2654435761u & (NJMP - 1);
	for (int k = 0; k < NJMP; k++, h = (h + 1) & (NJMP - 1)) {
		if (jmps[h].guest == g) return &jmps[h];
		if (!jmps[h].guest) { if (!make) return 0; jmps[h].guest = g; return &jmps[h]; }
	}
	if (!make) return 0;
	h = (g >> 3) * 2654435761u & (NJMP - 1); jmps[h].guest = g; return &jmps[h];   /* full: the oldest at its home slot goes */
}
RecompJmp *recomp_setjmp_begin(Cpu *c) {
	pthread_mutex_lock(&jmp_lock);
	RecompJmp *j = jmp_find(c->r[0], 1);
	pthread_mutex_unlock(&jmp_lock);
	memcpy(j->r, c->r, sizeof j->r);
	for (int i = 0; i < 8; i++) j->d[i] = c->vfp.d[8 + i];
	j->fpscr = c->fpscr;
	return j;
}
static void recomp_longjmp(Cpu *c) {   /* (buf, val): returns only if no setjmp here filled that buffer */
	pthread_mutex_lock(&jmp_lock);
	RecompJmp *j = jmp_find(c->r[0], 0);
	pthread_mutex_unlock(&jmp_lock);
	if (!j) { static int warned; if (!warned++) fprintf(stderr, "longjmp to %#x: no setjmp of it seen here -- Sony's longjmp instead (lr %#x)\n", c->r[0], c->r[14]); return; }
	uint32_t val = c->r[1] ? c->r[1] : 1;
	for (int i = 4; i <= 11; i++) c->r[i] = j->r[i];
	c->r[13] = j->r[13]; c->r[14] = j->r[14];
	for (int i = 0; i < 8; i++) c->vfp.d[8 + i] = j->d[i];
	c->fpscr = j->fpscr;
	c->r[0] = val;
	_longjmp(j->host, 1);
}
/* RECOMP_WATCH=hexaddr[,hexaddr...] (up to 4): guest words only the game's own code should change, checked before and after
 * every call the host handles (a GXM observer, a host implementation); a change during one is logged with the import,
 * old and new values and the caller -- the first 200. Another guest thread can change the word meanwhile (a counter the
 * game's threads move by one): the values tell those from a stray host write. How the earlier Killzone freeze was found
 * (an NGS handler writing through a nonexistent stack argument); its job scheduler's busy counter that stayed nonzero
 * (0x81a61de0, a later freeze) is the next suspect. */
static uint32_t watch_at[4]; static int nwatch = -1;
static void watch_snap(uint32_t v[4]) {
	if (nwatch < 0) {
		nwatch = 0; const char *e = getenv("RECOMP_WATCH");
		for (const char *p = e; p && *p && nwatch < 4; ) { char *end; uint32_t a = (uint32_t)strtoul(p, &end, 16); if (end == p) break; watch_at[nwatch++] = a & ~3u; p = *end == ',' ? end + 1 : end; }
		if (nwatch) fprintf(stderr, "    watch: %d guest word(s) checked around every host call\n", nwatch);
	}
	for (int i = 0; i < nwatch; i++) v[i] = RD32(watch_at[i]);
}
static void watch_check(const uint32_t v[4], const char *name, Cpu *c) {
	for (int i = 0; i < nwatch; i++) {
		uint32_t now = RD32(watch_at[i]); if (now == v[i]) continue;
		static int n; if (n++ >= 200) return;
		fprintf(stderr, "    watch: %#x %#x -> %#x during %s (lr %#x, r0 %#x r1 %#x)\n", watch_at[i], v[i], now, name, c->r[14], c->r[0], c->r[1]);
	}
}
void recomp_call_external(Cpu *c, uint32_t addr) {
#ifdef RECOMP_VERIFY
	extern int verify_external(Cpu *c, uint32_t addr);   /* in a checker's child: ends the native run */
	if (verify_external(c, addr)) return;
#endif
	/* RECOMP_SCHED=vita: the system-call boundary is where a guest thread is preempted (kernel.c's
	 * scheduler) -- here before the call, and after a host system call returns (it may have woken a
	 * higher-priority thread, which the hardware would switch to at once) */
	if (recomp_sched_on) sched_yield_if_preempted();
	const char *name = import_name_any(addr);
	if (!name) {   /* another module's function (its table isn't the caller's) */
		GuestFn f = guest_lookup(addr & ~1u);
		if (f) { f(c); return; }
		fprintf(stderr, "call to %#x: neither recompiled code nor an import (lr %#x)\n", addr, c->r[14]);
		/* the guest call chain: recompiled functions are C functions, so the native stack IS the
		 * guest's (fn_<address> frames, innermost first); the registers say what was called through */
		fprintf(stderr, "  r0 %#x r1 %#x r2 %#x r3 %#x r4 %#x r5 %#x r6 %#x r7 %#x sp %#x\n",
		        c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7], c->r[13]);
		plat_backtrace(stderr);
		{ extern void (*recomp_on_bad_call)(Cpu *, uint32_t); if (recomp_on_bad_call) recomp_on_bad_call(c, addr); }   /* a game's diagnostics (hooks.c) */
		/* code the analysis missed (reached only through a pointer it never saw built, or the rest of a function it
		 * cut short, branched to from the part it kept): recorded for analyze.py --seeds, which takes it as a
		 * function on the next run. Thumb: an odd address, one ARM code can't start at (not 4-aligned), or one
		 * Thumb code called or branched to (a branch target carries no Thumb bit; np_matching2's 0x8cee was
		 * recorded as ARM); ARM: the rest */
		const char *sp = getenv("RECOMP_SEEDS");
		{	/* GAPS-02: counted with its image (a run stops here, so the count says where; the next bootstrap run seeds it) */
			const int si = seed_image(addr, seg_lo, seg_hi, nseg); char w[112];
			if (seed_addr(addr, seg_lo, seg_hi, nseg)) snprintf(w, sizeof w, "missed function in %s (lr %#x)", si >= 0 && seg_img[si] ? seg_img[si] : "the main program range", c->r[14]);
			else snprintf(w, sizeof w, "call outside every loaded image (null or poisoned pointer, lr %#x)", c->r[14]);
			gap(GAP_MISSED_FN, addr & ~1u, w);
		}
		FILE *sf = seed_addr(addr, seg_lo, seg_hi, nseg) ? fopen(sp ? sp : "missed_functions.txt", "a") : 0;   /* (seedaddr.h: in a loaded image; a call through a null or poisoned pointer is not a missed function) */
		if (sf) { fprintf(sf, "%#x %s\n", addr & ~1u, (addr & 3) || (c->r[14] & 1) ? "thumb" : "arm"); fclose(sf); }
		summary("stopped"); exit(2);
	}
	uint32_t target; unsigned fl;
	HleFn fn = hle_resolve(addr, name, &target, &fl);
	if (fl & RF_LONGJMP) recomp_longjmp(c);   /* (returns only to fall back) */
	if (fl & RF_SETJMP) { static int warned; if (!warned++) fprintf(stderr, "setjmp called through a pointer (lr %#x): a longjmp to it can't unwind\n", c->r[14]); }
	if (target) {   /* a loaded module's export: its recompiled code */
		GuestFn f = guest_lookup(target & ~1u);
		/* the formatting functions are watched as when the host did them (RECOMP_LOG_FORMAT, the
		 * last formatted string on a stop): the buffer is the first argument */
		const int is_fmt = (fl & RF_FMT) != 0;
		uint32_t buf = c->r[0];
		if (f) {
			uint32_t a0 = c->r[0], a1 = c->r[1], a2 = c->r[2], a3 = c->r[3];
			gfxcov_api(name, fn, c, 1);   /* RECOMP_GFX_COVERAGE: handled by a module */
			if (fn) {   /* the boundary's observer, then Sony's function */
				/* the observer renders (the GPU's work on the hardware, done asynchronously there): under
				 * RECOMP_SCHED=vita the thread gives its run slot up meanwhile -- holding it, Unit 13's
				 * render worker kept a core 100+ ms per software-rendered scene */
				int b = sched_block_begin();
				uint32_t wv[4]; watch_snap(wv);
				fn(c);
				watch_check(wv, name, c);
				if (b) sched_block_end();
				c->r[0] = a0; c->r[1] = a1; c->r[2] = a2; c->r[3] = a3;
			}
			f(c);
			if ((fl & RF_GXM) && log_gxm())
				{ extern uint32_t kernel_current_tid(void);
				  fprintf(stderr, "gxm %-44s %08x %08x %08x %08x -> %08x (Sony) t%#x lr %#x\n", name, a0, a1, a2, a3, c->r[0], kernel_current_tid(), c->r[14]); }
			{   /* RECOMP_TRACE_MODULE=<prefix>[,...]: calls to a loaded module's exports by those names, with the first
			     * four arguments and the result (RECOMP_LOG_IMPORTS sees only the host's) */
				static const char *tr; static int tr_init; if (!tr_init) { tr = getenv("RECOMP_TRACE_MODULE"); tr_init = 1; }
				if (tr) for (const char *p = tr; *p; ) {
					const size_t l = strcspn(p, ",");
					if (l && !strncmp(name, p, l)) {
						fprintf(stderr, "    module %s(%#x, %#x, %#x, %#x) -> %#x\n", name, a0, a1, a2, a3, c->r[0]);
						/* RECOMP_TRACE_MODULE_DUMP=<arg 0-3>:<words>: what that pointer argument holds after the call (an
						 * out-structure: the values a library handed back, which the four registers can't show) */
						static int dk = -2, dn; if (dk == -2) { const char *d = getenv("RECOMP_TRACE_MODULE_DUMP"); dk = -1;
							if (d && d[0] >= '0' && d[0] <= '3' && d[1] == ':') { dk = d[0] - '0'; dn = atoi(d + 2); if (dn > 32) dn = 32; } }
						const uint32_t pa = dk == 0 ? a0 : dk == 1 ? a1 : dk == 2 ? a2 : a3;
						if (dk >= 0 && pa >= 0x10000) { fprintf(stderr, "      [%#x]:", pa); for (int w = 0; w < dn; w++) fprintf(stderr, " %08x", RD32(pa + 4u * w)); fprintf(stderr, "\n"); }
						break; }
					p += l; if (*p) p++;
				}
			}
			if (is_fmt && buf) { extern void hle_note_format(const char *s); hle_note_format((char *)G(buf)); }
			return;
		}
	}
	gfxcov_api(name, fn, c, 0);   /* RECOMP_GFX_COVERAGE */
	pthread_mutex_lock(&host_lock);
	uint64_t k = ++ncalls;
	count(name);
	if (!my_last && nlasts < 64) my_last = &lasts[nlasts++];
	if (my_last) { my_last->name = name; my_last->lr = c->r[14]; my_last->r0 = c->r[0]; my_last->r1 = c->r[1]; my_last->n++; my_last->at = k; }
	if ((k <= nlog || (log_from && k >= log_from)) && log_frame_ok())
		fprintf(stderr, "%6llu t%d %-40s %08x %08x %08x %08x lr %08x%s\n", (unsigned long long)k, my_last ? (int)(my_last - lasts) : -1,
		        name, c->r[0], c->r[1], c->r[2], c->r[3], c->r[14], fn ? "" : "  (stub)");
	if (k >= budget) { summary("budget reached"); prof_report(); exit(0); }   /* N01b-11 diagnostic: report the profile on the budget exit too */
	pthread_mutex_unlock(&host_lock);
	uint32_t a0 = c->r[0], a1 = c->r[1], a2 = c->r[2], a3 = c->r[3];
	/* no implementation and no module export: 0, or the family's "not initialised" error (stubret.h) */
	uint32_t stub_ret = 0;
	if (!fn) {
		static int zero = -1; if (zero < 0) zero = getenv("RECOMP_STUB_ZERO") != 0;
		stub_ret = stub_family_result(name, zero);
		gap_str(gap_unnamed_nid(name) ? GAP_NID_UNKNOWN : GAP_HLE_STUB, name);   /* GAPS-01 (each call) */
		/* said once per import, because a game feature that quietly does nothing (a ragdoll that freezes, a door that never opens, a silent gun) is usually one of these */
		static const char *said[512]; static int nsaid; int seen = 0;
		pthread_mutex_lock(&host_lock);
		for (int i = 0; i < nsaid; i++) if (said[i] == name) { seen = 1; break; }
		if (!seen && nsaid < 512) said[nsaid++] = name;
		pthread_mutex_unlock(&host_lock);
		if (!seen) fprintf(stderr, "    import %s: not implemented -- returns %#x (first call from lr %#x, r0 %#x r1 %#x)\n", name, stub_ret, c->r[14], a0, a1);
		/* diagnostic: every unimplemented import call is counted per name; the 2nd/100th/10000th
		 * report shows repeat-call loops the once-only line above cannot see (probe: save check) */
		{	static const char *n2[64]; static int c2[64]; static int n_n2; int k = -1;
			pthread_mutex_lock(&host_lock);
			for (int i = 0; i < n_n2; i++) if (n2[i] == name) { k = i; break; }
			if (k < 0 && n_n2 < 64) { k = n_n2++; n2[k] = name; c2[k] = 0; }
			if (k >= 0) c2[k]++;
			int cc = k >= 0 ? c2[k] : 0;
			pthread_mutex_unlock(&host_lock);
			if (cc == 2 || cc == 100 || cc == 10000) fprintf(stderr, "    import %s: call #%d (repeat-call diagnostic)\n", name, cc); }
	}
	{	/* RECOMP_IMPORT_BT=name[,name...]: the host's stack at the first call of each (its fn_<addr> frames are the guest's
		 * calls) -- who reaches an import, through virtual calls a static search can't follow */
		static const char *bt = (const char *)1; if (bt == (const char *)1) bt = getenv("RECOMP_IMPORT_BT");
		const char *at = bt ? strstr(bt, name) : 0; size_t nl = at ? strlen(name) : 0;
		if (at && (at[nl] == 0 || at[nl] == ',') && (at == bt || at[-1] == ',')) {
			static const char *done[32]; static int nd; int seen = 0;
			pthread_mutex_lock(&host_lock);
			for (int i = 0; i < nd; i++) if (done[i] == name) seen = 1;
			if (!seen && nd < 32) done[nd++] = name;
			pthread_mutex_unlock(&host_lock);
			if (!seen) { fprintf(stderr, "    import %s: first called from\n", name); plat_backtrace(stderr); }
		}
	}
	uint32_t wv[4]; watch_snap(wv);
	uint64_t imp_t0 = 0, imp_t1 = 0;   /* FEAT-ALL-01 diag: RECOMP_LOG_IMPORTS lines carry the host time (us) and how long the call took */
	if (fn) {
		extern void kernel_hle_in(const char *);
		recomp_excl_valid = 0;   /* the import is an exception: on its return the firmware's exclusive monitor is cleared -- a strex across a call must fail and retry with fresh data */
		recomp_excl_drop_reason = 1;   /* P0b diag: this drop site's tag for rt.h's spurious-failure log */
		imp_t0 = plat_now_us(); kernel_hle_in(name); fn(c); kernel_hle_in(0); imp_t1 = plat_now_us();
	}
	else {
		static int zero_w = -1; if (zero_w < 0) zero_w = getenv("RECOMP_STUB_ZERO") != 0;
		const uint32_t wait = stub_wait_us(name, a3, zero_w);   /* stubret.h: a wait call waits out its timeout */
		if (wait) { int b = sched_block_begin(); usleep((useconds_t)wait); if (b) sched_block_end(); }
		c->r[0] = stub_ret;
	}
	watch_check(wv, name, c);
	{	/* RECOMP_LOG_IMPORTS=name[,name...]: those imports (any whose name contains one of them) logged as they return -- the
		 * thread, the arguments, the result, the caller and the frame -- without the millions of the rest (RECOMP_LOG) */
		static const char *list = (const char *)1; static char buf[1024];
		if (list == (const char *)1) { const char *e = getenv("RECOMP_LOG_IMPORTS"); list = e ? (snprintf(buf, sizeof buf, ",%s,", e), buf) : 0; }
		if (list) {
			int hit = 0; const char *p = list;
			while (!hit && (p = strchr(p, ',')) && p[1]) { const char *q = strchr(p + 1, ','); char item[96]; size_t n = q ? (size_t)(q - p - 1) : 0;
				if (n && n < sizeof item) { memcpy(item, p + 1, n); item[n] = 0; hit = strstr(name, item) != 0; } p = q; }
			if (hit) { char tn[32] = ""; plat_thread_name(tn, sizeof tn); extern uint64_t display_frames(void); extern uint32_t kernel_current_tid(void);
				fprintf(stderr, "    imp f%llu [%s %#x] %s(%#x, %#x, %#x, %#x) -> %#x lr %#x t%llu d%llu\n", (unsigned long long)display_frames(), tn, kernel_current_tid(), name, a0, a1, a2, a3, c->r[0], c->r[14], (unsigned long long)imp_t0, (unsigned long long)(imp_t1 - imp_t0)); }
		}
	}
	if (recomp_sched_on) sched_yield_if_preempted();
	if (log_gxm() && !strncmp(name, "sceGxm", 6))
		fprintf(stderr, "gxm %-44s %08x %08x %08x %08x -> %08x (host)\n", name, a0, a1, a2, a3, c->r[0]);
}

uint32_t recomp_unimpl_at;
void recomp_unimplemented(Cpu *c, uint32_t addr) {
	pthread_mutex_lock(&host_lock);
	fprintf(stderr, "unimplemented instruction at %#x (lr %#x)\n", addr, c->r[14]);
	gap(GAP_INSN, addr, "unimplemented instruction (run stops)");
	ev_log("assert", "unimplemented instruction at %#x (lr %#x)", addr, c->r[14]);
	/* where it came from: the registers, and the return addresses on the stack (an odd value in the code ranges: a thumb
	 * return) -- a game's own trap (bkpt after a failed check) tells nothing else. 0x300 bytes above sp, the thread's own stack. */
	fprintf(stderr, "  regs r0-r3 %08x %08x %08x %08x r4-r7 %08x %08x %08x %08x r8-r12 %08x %08x %08x %08x %08x sp %08x lr %08x\n",
		c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7], c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13], c->r[14]);
	if (c->r[13] >= 0x10000000u && c->r[13] < 0x80000000u && !(c->r[13] & 3)) {
		fprintf(stderr, "  stack returns:");
		for (uint32_t o = 0, n = 0; o < 0x300 && n < 24; o += 4) { uint32_t w = RD32(c->r[13] + o); if ((w & 1) && ((w >= 0x81000000u && w < 0x82000000u) || (w >= 0x88000000u && w < 0x89000000u) || (w >= 0xb8000000u && w < 0xc8000000u))) { fprintf(stderr, " sp+%x:%08x", o, w); n++; } }
		fprintf(stderr, "\n");
	}
	/* what the registers point at (a game's own assert: its objects at the time), and one step further from the words of the
	 * object r10 holds -- the failed check was on a pointer in an object copied there (save_ezinfo.c:126: pSaveTask->Proc) */
	static const char *rn[] = {"r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12"};
	for (int k = 0; k < 13; k++) {
		uint32_t a = c->r[k]; if (a < 0x10000000u || a >= 0x80000000u || (a & 3)) continue;
		fprintf(stderr, "  mem %s=%08x:", rn[k], a); for (int w = 0; w < 20; w++) fprintf(stderr, " %08x", RD32(a + 4u * w)); fprintf(stderr, "\n");
		if (k == 10 || k == 8) for (int w = 0; w < 20; w++) {
			uint32_t q = RD32(a + 4u * w); if (q < 0x10000000u || q >= 0x80000000u || (q & 3)) continue;
			fprintf(stderr, "  mem %s+%x->%08x:", rn[k], 4 * w, q); for (int x = 0; x < 12; x++) fprintf(stderr, " %08x", RD32(q + 4u * x)); fprintf(stderr, "\n");
		}
	}
	{	/* the task the failed check looked at (r8+0x30), its Proc (+0xc): if that is an import stub, the name it stands for and the
		 * export it reaches -- the Title passes the address of an imported function as the save task's Proc */
		uint32_t r8 = c->r[8];
		if (r8 >= 0x10000000u && r8 < 0x80000000u && !(r8 & 3)) {
			uint32_t t = RD32(r8 + 0x30);
			if (t >= 0x10000000u && t < 0x80000000u && !(t & 3)) {
				uint32_t proc = RD32(t + 0xc); const char *nm = import_name_any(proc & ~1u);
				fprintf(stderr, "  task %#x Proc %#x: %s", t, proc, nm ? "import stub" : "not an import stub");
				if (nm) { uint32_t ex = export_addr(nm); fprintf(stderr, " %s -> module export %#x", nm, ex); }
				fprintf(stderr, "\n");
			}
		}
	}
	if (*hle_last_format) fprintf(stderr, "last formatted string: %s\n", hle_last_format);
	summary("stopped"); exit(2);
}

static uint32_t module_starts[16];
static int nmodule_starts;
/* an image file's segments into guest memory and its records registered. ondemand (modload.c): a game module loaded
 * while the game runs -- errors are returned, not fatal, and its module_start / module_stop (0: none) and the range
 * its segments cover go to the caller instead of the start-up list */
static int load_image_ex(const char *path, int ondemand, uint32_t *start, uint32_t *stop, uint32_t *lo, uint32_t *hi) {
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); if (ondemand) return -1; exit(1); }
	char magic[4]; uint32_t n;
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "VRIM", 4) || fread(&n, 4, 1, f) != 1) { fprintf(stderr, "%s: not an image\n", path); if (ondemand) { fclose(f); return -1; } exit(1); }
	uint32_t l = 0xffffffffu, h_ = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t h[3];
		if (fread(h, 4, 3, f) != 3) { fprintf(stderr, "%s: truncated\n", path); if (ondemand) { fclose(f); return -1; } exit(1); }
		if (h[2] && fread(G(h[0]), 1, h[1], f) != h[1]) { fprintf(stderr, "%s: truncated\n", path); if (ondemand) { fclose(f); return -1; } exit(1); }
		if (!h[2] && ondemand) memset(G(h[0]), 0, h[1]);   /* (a reload starts from a zeroed .bss again) */
		fprintf(stderr, "loaded %#010x %#9x%s\n", h[0], h[1], h[2] ? "" : " (zero)");
		if (nseg < MAX_SEGS) {
			const char *b = path; for (const char *q = path; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
			seg_img[nseg] = i ? seg_img[nseg - 1] : strdup(b);   /* (one name per image file, shared by its segments) */
			seg_lo[nseg] = h[0]; seg_hi[nseg] = h[0] + h[1]; nseg++;
		}
		if (h[0] < 0xffe00000u) {   /* (the module's range: its segments, not its imported-variable slots up there) */
			if (h[0] < l) l = h[0];
			if (h[0] + h[1] > h_) h_ = h[0] + h[1];
		}
	}
	if (start) *start = 0;
	if (stop) *stop = 0;
	if (lo) *lo = l;
	if (hi) *hi = h_;
	char tag[4];
	while (fread(tag, 1, 4, f) == 4) {
		if (!memcmp(tag, "TLS", 4)) {
			if (fread(tls_template, 4, 3, f) == 3)
				fprintf(stderr, "TLS template %#x, %#x bytes, block %#x\n", tls_template[0], tls_template[1], tls_template[2]);
		} else if (!memcmp(tag, "EXPT", 4) || !memcmp(tag, "EXPL", 4)) {   /* a module's exports: (address, name) --
			 * EXPT: u32 address + a 60-byte name; EXPL: u32 address, u16 length, the name (the ones longer than 59
			 * characters: C++ names such as sce::Json::Initializer::initialize, which EXPT cut and nothing matched) */
			int lng = !memcmp(tag, "EXPL", 4);
			if (fread(&n, 4, 1, f) != 1) break;
			pthread_mutex_lock(&exports_lock);
			exports = realloc(exports, (nexports + n) * sizeof *exports);
			uint32_t got = 0;
			for (; got < n; got++) {
				uint32_t a; uint16_t len = 59;
				if (fread(&a, 4, 1, f) != 1 || (lng && fread(&len, 2, 1, f) != 1)) break;
				char nm[EXPORT_NAME] = {0}; size_t take = len < EXPORT_NAME - 1 ? len : EXPORT_NAME - 1;
				if (lng) { if (fread(nm, 1, take, f) != take || (len > take && fseek(f, len - take, SEEK_CUR))) break; }
				else { char raw[60]; if (fread(raw, 1, 60, f) != 60) break; memcpy(nm, raw, 59); }
				exports[nexports + got].addr = a; memcpy(exports[nexports + got].name, nm, EXPORT_NAME);
			}
			nexports += got;
			pthread_mutex_unlock(&exports_lock);
			if (got != n) break;
		} else if (!memcmp(tag, "PPRM", 4)) {   /* the program's process parameters */
			extern uint32_t process_param;
			if (fread(&process_param, 4, 1, f) != 1) break;
		} else if (!memcmp(tag, "MSTA", 4)) {   /* a module's module_start */
			uint32_t a; if (fread(&a, 4, 1, f) != 1) break;
			if (ondemand) { if (start) *start = a; }
			else if (nmodule_starts < 16) module_starts[nmodule_starts++] = a;
		} else if (!memcmp(tag, "MSTO", 4)) {   /* a module's module_stop (run by modload.c's unload) */
			uint32_t a; if (fread(&a, 4, 1, f) != 1) break;
			if (ondemand && stop) *stop = a;
		} else if (!memcmp(tag, "VARS", 4)) {   /* imported variables: (slot address, name) */
			if (fread(&n, 4, 1, f) != 1) break;
			for (uint32_t i = 0; i < n; i++) {
				struct { uint32_t addr; char name[60]; } v;
				if (fread(&v, sizeof v, 1, f) != 1) break;
				v.name[59] = 0;
				hle_import_var(v.name, v.addr);
			}
			hle_import_vars_done();
		} else break;
	}
	fclose(f);
	return 0;
}
static void load_image(const char *path) { load_image_ex(path, 0, 0, 0, 0, 0); }
/* modload.c's side of the loader (MODLOAD-01) */
int image_load_module(const char *img, uint32_t *start, uint32_t *stop, uint32_t *lo, uint32_t *hi) {
	return load_image_ex(img, 1, start, stop, lo, hi);
}
void image_drop_exports(uint32_t lo, uint32_t hi) {   /* an unloaded module's exports stop resolving (entries kept, address 0) */
	pthread_mutex_lock(&exports_lock);
	for (uint32_t i = 0; i < nexports; i++) if (exports[i].addr >= lo && exports[i].addr < hi) exports[i].addr = 0;
	pthread_mutex_unlock(&exports_lock);
}
void imports_reset(void) {   /* every import resolves again at its next call (a module came or went) */
	for (uint32_t h = 0; h < RESOLVE_SLOTS; h++) __atomic_store_n(&resolved[h].known, 0, __ATOMIC_RELEASE);
}

/* ---- guest profiler (a build with -DRECOMP_TRACE, see build.sh): every guest instruction
 * reports its address; with RECOMP_PROFILE=<seconds> set, the main thread's instructions are
 * counted per guest function from that time on, and printed when the run ends */
__thread uint32_t recomp_last_pc;   /* profile builds: the guest instruction this thread ran last (kernel.c's write watch names it) */
uint32_t recomp_last_pc_now(void) { return recomp_last_pc; }   /* (platform_win32.c's exception log: a weak reference to a function links everywhere) */
#ifdef RECOMP_TRACE
#include <time.h>
static __thread int prof_main;
static int prof_on;
static uint32_t *prof_hits;            /* per 2-byte instruction slot in the image */
#define PROF_BASE 0x81000000u
#define PROF_SIZE 0x01000000u   /* N01b-11 diagnostic: 4 -> 16 MB, the eboot spans up to 0x81c9xxxx */
void recomp_trace(Cpu *c, uint32_t addr) {
	recomp_last_pc = addr;
#ifdef RECOMP_VERIFY
	extern void verify_trace(Cpu *c, uint32_t addr);
	verify_trace(c, addr);
#endif
	if (prof_on && addr - PROF_BASE < PROF_SIZE) prof_hits[(addr - PROF_BASE) >> 1]++;   /* N01b-11 diagnostic: count every guest thread (the spinner is not the host main thread) */
	/* RECOMP_TRACE_TID=<guest thread uid>: every recompiled function that thread enters, with its lr and r0-r1, in
	 * order (the path a thread took to a failure -- which branch a function skipped needs this, an import log can't
	 * show it); RECOMP_TRACE_TID_FROM=<frame>: from that presented frame on */
	static uint32_t trace_tid = 1; static uint64_t trace_from;
	if (trace_tid == 1) { const char *e = getenv("RECOMP_TRACE_TID"), *f = getenv("RECOMP_TRACE_TID_FROM");
		trace_tid = e ? (uint32_t)strtoul(e, 0, 0) : 0; trace_from = f ? strtoull(f, 0, 0) : 0; }
	if (trace_tid) { extern uint32_t kernel_current_tid(void); extern uint64_t display_frames(void);
		if (kernel_current_tid() == trace_tid && display_frames() >= trace_from && recomp_lookup(addr))
			fprintf(stderr, "    trace t%#x f%llu: enter %#x lr %#x r0 %#x r1 %#x r2 %#x r3 %#x stk %#x %#x %#x %#x r4 %#x r5 %#x r6 %#x r7 %#x sp %#x\n", trace_tid,
			        (unsigned long long)display_frames(), addr, c->r[14], c->r[0], c->r[1], c->r[2], c->r[3],
			        RD32(c->r[13]), RD32(c->r[13] + 4), RD32(c->r[13] + 8), RD32(c->r[13] + 12), c->r[4], c->r[5], c->r[6], c->r[7], c->r[13]); }
}
static void *prof_timer(void *arg) {
	usleep((useconds_t)(atof((char *)arg) * 1e6));
	fprintf(stderr, "== profiling the main thread from now\n");
	prof_on = 1;
	return 0;
}
static void prof_report(void) {
	if (!prof_hits) return;
	/* attribute each slot to the nearest recompiled function entry at or below it */
	typedef struct { uint32_t entry; uint64_t n; } F;
	static F fs[20000]; int nf = 0; uint64_t total = 0;
	uint32_t cur = 0; uint64_t acc = 0;
	for (uint32_t i = 0; i < PROF_SIZE / 2; i++) {
		uint32_t a = PROF_BASE + 2 * i;
		if (recomp_lookup(a)) { if (cur && acc && nf < 20000) fs[nf++] = (F){cur, acc}; cur = a; acc = 0; }
		acc += prof_hits[i]; total += prof_hits[i];
	}
	if (cur && acc && nf < 20000) fs[nf++] = (F){cur, acc};
	for (int i = 0; i < nf; i++) for (int j = i + 1; j < nf; j++) if (fs[j].n > fs[i].n) { F t = fs[i]; fs[i] = fs[j]; fs[j] = t; }
	fprintf(stderr, "== main-thread profile: %llu instructions\n", (unsigned long long)total);
	for (int i = 0; i < nf && i < (getenv("RECOMP_PROFILE_TOP") ? atoi(getenv("RECOMP_PROFILE_TOP")) : 40); i++)
		fprintf(stderr, "  %5.1f%%  fn_%08x\n", 100.0 * fs[i].n / (total ? total : 1), fs[i].entry);
}
#else
static void prof_report(void) {}
#endif

/* call guest code from a system call: a copy of the caller's registers, the stack below its own */
uint32_t guest_call(Cpu *parent, uint32_t fn, uint32_t a0, uint32_t a1) {
	GuestFn f = guest_lookup(fn & ~1u);
	if (!f) { fprintf(stderr, "guest callback %#x is not recompiled code\n", fn); return 0; }
	Cpu n = *parent;
	n.r[0] = a0; n.r[1] = a1;
	n.r[13] = (parent->r[13] - 512) & ~7u;
	n.r[14] = 0xfffffffe;
	f(&n);
	return n.r[0];
}

/* each guest thread's thread-local storage block, what TPIDRURO points at */
uint32_t tls_block(void) {
	uint32_t size = tls_template[2] > 0x4000 ? tls_template[2] : 0x4000;
	uint32_t a = guest_alloc(size, 64);
	memset(G(a), 0, size);
	if (tls_template[1]) memcpy(G(a), G(tls_template[0]), tls_template[1]);
	return a;
}

/* RECOMP_STALL_DUMP=<seconds>: every interval, every guest thread's innermost call site and
 * registers (kernel.c's kernel_report_pcs, the main thread's own here) -- a thread spinning in
 * pure guest code makes no imports and is otherwise invisible in a build without -DRECOMP_TRACE.
 * (Not gated on the global call count: a stalled game thread hides behind other threads' calls.) */
static Cpu *game_cpu;
static void *stall_dump(void *arg) {
	double secs = atof((const char *)arg);
	extern void kernel_report_pcs(void);
	for (;;) {
		usleep((useconds_t)(secs * 1e6));
		fprintf(stderr, "== stall dump: %llu import calls\n", (unsigned long long)ncalls);
		/* the RECOMP_WATCH words every interval as well: its import-anchored snap/check has a
		 * blind window between one call's check and the next call's snap -- a guest-side write
		 * there only shows up here */
		{
			static uint32_t wa[4]; static int nw = -1;
			if (nw < 0) {
				nw = 0; const char *e = getenv("RECOMP_WATCH");
				for (const char *p = e; p && *p && nw < 4; ) {
					char *end; uint32_t av = (uint32_t)strtoul(p, &end, 16);
					if (end == p) break;
					wa[nw++] = av & ~3u; p = *end == ',' ? end + 1 : end;
				}
			}
			for (int i = 0; i < nw; i++) fprintf(stderr, "  stall: word %#x = %#x\n", wa[i], RD32(wa[i]));
		}
		if (game_cpu) fprintf(stderr, "  stall: main   lr %#x sp %#x r0 %#x r1 %#x r2 %#x r3 %#x r4 %#x r5 %#x r6 %#x r7 %#x r8 %#x r9 %#x r10 %#x r11 %#x r12 %#x\n",
		        game_cpu->r[14], game_cpu->r[13], game_cpu->r[0], game_cpu->r[1], game_cpu->r[2], game_cpu->r[3],
		        game_cpu->r[4], game_cpu->r[5], game_cpu->r[6], game_cpu->r[7],
		        game_cpu->r[8], game_cpu->r[9], game_cpu->r[10], game_cpu->r[11], game_cpu->r[12]);
		/* the tree nodes the walk is stuck between: their links and keys say which insert wrote what */
		for (int i = 0; game_cpu && i < 6; i++) {
			uint32_t a = i == 0 ? game_cpu->r[12] : i == 1 ? game_cpu->r[9] : 0;
			if (i >= 2) {   /* the stuck node's children and their children */
				uint32_t *p = (uint32_t *)G(game_cpu->r[12]);
				uint32_t kid = i == 2 ? p[0] : i == 3 ? p[1] : i == 4 ? ((uint32_t *)G(game_cpu->r[9]))[0] : ((uint32_t *)G(game_cpu->r[9]))[1];
				a = kid;
			}
			if (a && a + 0x28 < (1u << 30) && (a & 3) == 0) {
				uint32_t *w = (uint32_t *)G(a);
				fprintf(stderr, "  stall: node %#x: left %#x right %#x parent %#x f4 %#x keylo %#x keyhi %#x f6 %#x f7 %#x fl %02x %02x\n",
				        a, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], G(a)[32], G(a)[33]);
			}
		}
		kernel_report_pcs();
		{   /* the A/B watch counts (N01b-7): the loader's completion signal, the render-start event
			 * and the GXM scene calls counted so far, whatever the log level */
			static const char *const watch_names[] = {"sceKernelSignalSema", "sceKernelSetEvent", "sceGxmBeginScene", "sceGxmEndScene", 0};
			for (int w = 0; watch_names[w]; w++)
				for (int i = 0; i < ncounts; i++)
					if (counts[i].name == watch_names[w] || !strcmp(counts[i].name, watch_names[w])) {
						fprintf(stderr, "  stall: count %s %llu\n", watch_names[w], (unsigned long long)counts[i].n);
						break;
					}
		}
		{ extern void kernel_report_obj_counts(void); kernel_report_obj_counts(); }
		summary_threads();
	}
	return 0;
}

/* the game's main thread: module_start on a fresh stack */
static uint32_t game_entry;
static void *game(void *arg) {
	(void)arg;
	recomp_runfast();   /* FPCR is per host thread */
	recomp_tpidruro = tls_block();
	kernel_sched_init();   /* RECOMP_SCHED=vita: the main thread takes the first run slot */
	static Cpu c;
	game_cpu = &c;
	if (getenv("RECOMP_STALL_DUMP")) { pthread_t dt; pthread_create(&dt, 0, stall_dump, (void *)getenv("RECOMP_STALL_DUMP")); }
	uint32_t stack = guest_alloc(1u << 20, 4096);   /* main thread: 1 MiB */
	c.r[13] = stack + (1u << 20) - 16;
	c.r[14] = 0xfffffffe;
	c.fpscr = 0x03000000;                          /* RunFast, as the Vita runs user code */
	hle_init(&c);
	if (getenv("RECOMP_IMPORTS")) {   /* RECOMP_IMPORTS=1: every import of the game and its modules with how the host resolves it, then exit -- the proactive sweep for stubs (a stub returns 0 and says nothing else) */
		static const char *names[8192]; int n = 0;
		for (int r = 0; r < 2; r++) {
			uint32_t lo = r ? 0x88000000u : 0x81000000u, hi = r ? 0x98000000u : 0x82000000u;   /* the modules: 16 slots of 0x01000000 from 0x88000000 (libfios2 is the fourth, at 0x8b000000) */
			for (uint32_t a = lo; a < hi && n < 8192; a += 2) { const char *nm = import_name_any(a); if (!nm) continue; int k; for (k = 0; k < n; k++) if (names[k] == nm || !strcmp(names[k], nm)) break; if (k == n) names[n++] = nm; }
		}
		int nh = 0, nm = 0, ns = 0;
		for (int pass = 0; pass < 3; pass++) for (int i = 0; i < n; i++) {
			HleFn fn = hle_find(names[i]); uint32_t t = fn && host_preferred(names[i]) ? 0 : export_addr(names[i]);
			int st = t ? 1 : fn ? 0 : 2;
			if (st != pass) continue;
			if (pass == 0) nh++; else if (pass == 1) nm++; else ns++;
			printf("%-7s %s\n", st == 0 ? "host" : st == 1 ? "module" : "STUB", names[i]);
		}
		printf("== %d imports: %d host, %d module, %d STUB (return 0)\n", n, nh, nm, ns); fflush(stdout); exit(0);
	}
#ifdef RECOMP_TRACE
	prof_main = 1;
	prof_hits = calloc(PROF_SIZE / 2, sizeof *prof_hits);
	if (getenv("RECOMP_PROFILE")) { pthread_t t; pthread_create(&t, 0, prof_timer, getenv("RECOMP_PROFILE")); }
#endif
	for (int i = 0; i < nmodule_starts; i++) {   /* the modules first, each on a thread of its own, as the loader does */
		extern uint32_t kernel_run_thread(const char *name, uint32_t entry);
		fprintf(stderr, "module %#x start\n", module_starts[i]);
		fprintf(stderr, "module start returned %#x\n", kernel_run_thread("module_start", module_starts[i]));
	}
	fprintf(stderr, "module_start %#x\n", game_entry);
	guest_lookup(game_entry & ~1u)(&c);
	fprintf(stderr, "module_start returned %#x\n", c.r[0]);
	summary("module_start returned");
	exit(0);
}

/* a crash in the native program (recompiled code or the host): the host stack and every guest
 * thread's last system call, then the default action (a core / the debugger) */
static void on_crash(const char *what, void *addr) {   /* (platform.h: the default action follows) */
	fprintf(stderr, "\n== crash: %s at host address %p (guest %#llx)\n", what, addr,
	        (unsigned long long)((uint8_t *)addr - recomp_mem));
	plat_backtrace(stderr);
	summary_threads();
}
static void on_signal(int sig) {   /* a run stopped from outside still reports where it was */
	(void)sig;
	summary("interrupted");
	gaps_report();
	prof_report();
	_exit(130);
}

/* the game's play settings, its pack's RUN_ENV built in by `vitarecomp build` (RECOMP_BUILTIN_ENV): each applied where the
 * environment doesn't set it already, so the binary plays as its conf says however it's started -- `vitarecomp run` sets
 * them itself; a run from an old checkout with an empty RUN_ENV lost RECOMP_SRGB and the aspect hook, and Killzone came out
 * washed out on Linux (2026-10-21). Headless runs (the tests: TEST_ENV is theirs) and RECOMP_NO_RUN_ENV=1 skip them. */
static void builtin_env(void) {
#ifdef RECOMP_BUILTIN_ENV
	if (getenv("RECOMP_HEADLESS") || (getenv("RECOMP_NO_RUN_ENV") && *getenv("RECOMP_NO_RUN_ENV") != '0')) return;
	static char buf[sizeof RECOMP_BUILTIN_ENV]; memcpy(buf, RECOMP_BUILTIN_ENV, sizeof buf);
	int n = 0;
	for (char *tok = strtok(buf, " \t"); tok; tok = strtok(0, " \t")) {
		char *eq = strchr(tok, '='); if (!eq || eq == tok) continue;
		*eq = 0; if (getenv(tok)) continue;
#ifdef _WIN32
		_putenv_s(tok, eq + 1);
#else
		setenv(tok, eq + 1, 0);
#endif
		n++;
	}
	if (n) fprintf(stderr, "    the game's built-in settings applied: %d (its pack's RUN_ENV; RECOMP_NO_RUN_ENV=1 skips them)\n", n);
#endif
}
#ifdef RECOMP_IOS
/* iOS (runtime/build.py TARGET=ios): SDL starts the app and calls main (renamed SDL_main) on the UI thread; ios_boot finds the game's
 * files and stands in for the command line (runtime/platform/ios/ios_boot.c) */
#include <SDL3/SDL_main.h>
int ios_boot(int *argc, char ***argv);
#endif
int main(int argc, char **argv) {
#ifdef RECOMP_IOS
	if (ios_boot(&argc, &argv)) return 1;
#endif
	builtin_env();
	atexit(gaps_report);   /* GAPS-01: the GAPS line + table and gaps.json at the end of every run */
#ifdef __linux__
	/* RECOMP_DEBUGGABLE=1: let any process attach a debugger (Yama ptrace_scope 1 only allows the
	 * parent): `gdb -p` / `eu-stack -p` thread dumps of a stalled run without root */
	if (getenv("RECOMP_DEBUGGABLE")) prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
	signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);   /* (a write to a connection the other end closed is an error to handle, not the end of the game: the online service's own sockets) */
#endif
	plat_set_crash_handler(on_crash);
	if (argc < 3) { fprintf(stderr, "usage: %s <image.img> <module_start hex> [max import calls]\n", argv[0]); return 1; }
	settings_load(getenv("RECOMP_SETTINGS"));   /* settings.ini in the game's directory (runtime/core/settings.h); RECOMP_SETTINGS=path another file */
	/* under it, the shared settings every game reads (the launcher's Settings page): a played run's only -- a scripted one
	 * (RECOMP_HEADLESS: test, capture, verify) or one given its own file stays on its own, as the gate needs */
	if (!getenv("RECOMP_HEADLESS") && !getenv("RECOMP_SETTINGS")) {
		const char *sp = getenv("RECOMP_SETTINGS_SHARED");
		char *pref = sp ? NULL : SDL_GetPrefPath("ProjectNativ", "Nativ"), path[1024] = "";
		if (sp) snprintf(path, sizeof path, "%s", sp);
		else if (pref) {
			snprintf(path, sizeof path, "%ssettings.ini", pref);
			/* the name was Project Ativ: its settings are copied over once (as the launcher does) */
			const char *at = strstr(pref, "ProjectNativ");
			FILE *in = NULL, *out = NULL;
			if (at && strlen(at) == 19 && access(path, F_OK) != 0) {   /* "ProjectNativ/Nativ/" */
				char old[1024]; snprintf(old, sizeof old, "%.*sProjectAtiv%cAtiv%csettings.ini", (int)(at - pref), pref, at[12], at[18]);
				if ((in = fopen(old, "rb")) && (out = fopen(path, "wb"))) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, n, out); }
			}
			if (in) fclose(in);
			if (out) fclose(out);
		}
		SDL_free(pref);
		if (path[0]) settings_load_shared(path);
	}
	if (argc > 3) budget = strtoull(argv[3], 0, 0);
	if (getenv("RECOMP_LOG")) nlog = strtoull(getenv("RECOMP_LOG"), 0, 0);
	if (getenv("RECOMP_LOG_FROM")) log_from = strtoull(getenv("RECOMP_LOG_FROM"), 0, 0);   /* and every call from #n on */
	recomp_mem = plat_reserve(1ull << 32);
	if (!recomp_mem) { fprintf(stderr, "can't reserve the 4 GiB guest address space\n"); return 1; }
	load_image(argv[1]);
	if (getenv("RECOMP_MODULES")) {   /* "a.img,b.img": modules loaded with the program */
		char list[4096]; snprintf(list, sizeof list, "%s", getenv("RECOMP_MODULES"));
		for (char *p = strtok(list, ","); p; p = strtok(0, ",")) load_image(p);
	}
	{ extern void recomp_watch_fixed_arm(void); recomp_watch_fixed_arm(); }   /* RECOMP_WATCH_FIXED=<addr>,<len> (kernel.c) */
	game_entry = strtoul(argv[2], 0, 16);
	if (!guest_lookup(game_entry & ~1u)) { fprintf(stderr, "%#x is not a recompiled function\n", game_entry); return 1; }
	window_run(game, 0);   /* window.c: on its own thread with a window, or right here headless */
	return 0;
}
