/* Vita system calls on the host: SceAppUtil and common dialogs: save data and the save-data dialog (the
 * overlay draws its list and questions), dialogs that finish at once, trophies; system parameters, app
 * events, power, the console id, and the movie player (which refuses every movie). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "guestwatch.h"
#include "hle.h"
#include "net.h"
#include "overlay.h"   /* (overlay_toast_titled: the network check's notice) */
#include "platform.h"
#include "savedlg.h"
#include "settings.h"
#include "cdlg.h"
#include "imedlg.h"
#include "hle_small.h"
#include "hle_offline.h"

/* ---- common dialogs: one that was opened finishes at once with an OK result, as if the player
 * confirmed it; each kind has its own state (a dialog never opened reports NONE). The save-data
 * dialog is the exception: a real one, below. */
enum { DLG_TROPHY, DLG_NETCHECK, DLG_MSG, DLG_EULA, DLG_N };
static int dialog_open[DLG_N];
#define DIALOG(kind, name) \
	static void h_##name##_init(Cpu *c) { dialog_open[kind] = 1; RET(0); } \
	static void h_##name##_status(Cpu *c) { RET(dialog_open[kind] ? 2 : 0); } /* FINISHED : NONE */ \
	static void h_##name##_term(Cpu *c) { int in_use = dialog_open[kind]; dialog_open[kind] = 0; RET(in_use ? 0 : 0x80020411u); } /* NOT_IN_USE without an open dialog */
DIALOG(DLG_TROPHY, trophy_dlg)
DIALOG(DLG_NETCHECK, net_dlg)
DIALOG(DLG_MSG, msg_dlg_base)
/* the message dialog's result is the console's after the player confirmed it: SceMsgDialogResult {mode, result,
 * buttonId} -- mode as the game opened it (SceMsgDialogParam.mode, +80 after sdkVersion and the 76-byte
 * SceCommonDialogParam), result OK, and the button the player took: the first (OK / YES / BUTTON1, id 1) for a user
 * message with buttons or a system / error message; none (INVALID 0) for a user message without buttons or a progress
 * bar, which only the game closes. Writing only word 0 left mode 0 and buttonId INVALID: no answer to the question. */
static uint32_t msg_dlg_mode, msg_dlg_button;
static void h_msg_dlg_init(Cpu *c) {
	msg_dlg_mode = A0 ? RD32(A0 + 80) : 0; msg_dlg_button = 1;
	uint32_t btype = 0xffffffffu;
	if (msg_dlg_mode == 1 && A0 && RD32(A0 + 84)) { btype = RD32(RD32(A0 + 84)); if (btype == 2) msg_dlg_button = 0; }   /* USER_MSG; button type NONE */
	if (msg_dlg_mode == 4) msg_dlg_button = 0;   /* PROGRESS_BAR */
	static int logged; if (logged++ < 4) fprintf(stderr, "    msg dialog: mode %u button type %d -> confirmed, button %u\n", msg_dlg_mode, (int)btype, msg_dlg_button);
	h_msg_dlg_base_init(c);
}
static void h_msg_dlg_status(Cpu *c) { h_msg_dlg_base_status(c); }
static void h_msg_dlg_term(Cpu *c) { h_msg_dlg_base_term(c); }
static void h_msg_dlg_result(Cpu *c) { if (A0) { WR32(A0, msg_dlg_mode); WR32(A0 + 4, 0); WR32(A0 + 8, msg_dlg_button); } RET(0); }
DIALOG(DLG_EULA, eula_dlg)
static void h_dialog_result(Cpu *c) { if (A0) WR32(A0, 0); RET(0); }
/* (HLE-LIB-01) Abort / Close for the kinds above: one that is in use finishes (its result stays the host's OK);
 * one that is not is NOT_RUNNING */
static void h_old_dialog_abort_kind(Cpu *c, int kind) { RET(dialog_open[kind] ? 0 : 0x80020404u); }
static void h_msg_dlg_abort(Cpu *c) { h_old_dialog_abort_kind(c, DLG_MSG); }
static void h_net_dlg_abort(Cpu *c) { h_old_dialog_abort_kind(c, DLG_NETCHECK); }
static void h_trophy_dlg_abort(Cpu *c) { h_old_dialog_abort_kind(c, DLG_TROPHY); }

/* ---- (HLE-LIB-01) the rest of SceCommonDialog on cdlg.h's state machine: the IME dialog (its initial text entered
 * as typed -- the host has no on-screen keyboard yet) and the PSN / social / store dialogs (canceled: nothing behind
 * them). Each kind's result struct starts with its SceInt32 result; the IME's has the button after it. */
static CdlgState cdlg[CDLG_N];
static pthread_mutex_t cdlg_lock = PTHREAD_MUTEX_INITIALIZER;
static void cdlg_call(Cpu *c, int kind, int op) {   /* op: 0 init, 1 status, 2 result, 3 term, 4 abort */
	pthread_mutex_lock(&cdlg_lock);
	uint32_t r = 0; int32_t res = 0;
	switch (op) {
	case 0: r = cdlg_init(cdlg, kind); break;
	case 1: r = (uint32_t)cdlg_status(cdlg, kind); break;
	case 2: r = cdlg_result(cdlg, kind, &res);
		if (!r && A0) { WR32(A0, (uint32_t)res); if (kind == CDLG_IME) WR32(A0 + 4, res == CDLG_RESULT_OK ? 2u : 0u); }   /* button: ENTER / NONE */
		break;
	case 3: r = cdlg_term(cdlg, kind); break;
	case 4: r = cdlg_abort(cdlg, kind); break;
	}
	pthread_mutex_unlock(&cdlg_lock);
	RET(r);
}
/* sceImeDialogInit(SceImeDialogParam *): title +0x28, maxTextLength +0x2c, initialText +0x30, inputTextBuffer +0x34
 * (UTF-16). With a window (HLE-LIB-05) the host's card asks the player (imedlg.h, runtime/ui/imedlg_ui.cpp): the dialog
 * runs until they answer, GetStatus takes the answer and writes the text. Without one (headless, tests) the initial text
 * is entered as typed (HLE-LIB-01). */
static uint32_t ime_out, ime_max;
static void ime_write(const char *utf8) {   /* the answer into the game's inputTextBuffer (max units + terminator) */
	if (!ime_out) return;
	static uint16_t u[4097]; const size_t m = ime_max && ime_max < 4096 ? ime_max : 4096;
	const size_t n = imedlg_utf8_to_utf16(utf8, u, m);
	for (size_t i = 0; i <= n; i++) WR16(ime_out + 2 * (uint32_t)i, u[i]);
}
static void h_ime_dlg_init(Cpu *c) {
	if (!A0) { RET(0x80020402u); return; }   /* SCE_COMMON_DIALOG_ERROR_NULL */
	const uint32_t tptr = RD32(A0 + 0x28), max = RD32(A0 + 0x2c), init = RD32(A0 + 0x30), out = RD32(A0 + 0x34);
	static uint16_t t16[513], i16[2049]; char title[1024], text[IMEDLG_TEXT_MAX];
	size_t k = 0; if (tptr) while (k < 512 && RD16(tptr + 2 * (uint32_t)k)) { t16[k] = RD16(tptr + 2 * (uint32_t)k); k++; } t16[k] = 0;
	imedlg_utf16_to_utf8(t16, k, title, sizeof title);
	k = 0; if (init) while (k < 2048 && k < (max ? max : 2048) && RD16(init + 2 * (uint32_t)k)) { i16[k] = RD16(init + 2 * (uint32_t)k); k++; } i16[k] = 0;
	imedlg_utf16_to_utf8(i16, k, text, sizeof text);
	pthread_mutex_lock(&cdlg_lock);
	const int card = cdlg_status(cdlg, CDLG_IME) == CDLG_STATUS_NONE && imedlg_ui_ready();
	const uint32_t r = card ? cdlg_init_running(cdlg, CDLG_IME) : cdlg_init(cdlg, CDLG_IME);
	if (!r) { ime_out = out; ime_max = max; }
	pthread_mutex_unlock(&cdlg_lock);
	if (!r) {
		if (card) { imedlg_open(title, text, max); fprintf(stderr, "    ime dialog: \"%s\" -- the card asks the player (max %u)\n", title, max); }
		else { ime_write(text); fprintf(stderr, "    ime dialog: no window -- the initial text is entered as typed\n"); }
	}
	RET(r);
}
/* GetStatus: a running card's answer is taken here -- the text written, the dialog FINISHED */
static void h_ime_dlg_status(Cpu *c) {
	char t[IMEDLG_TEXT_MAX]; int ok;
	pthread_mutex_lock(&cdlg_lock);
	if (cdlg_status(cdlg, CDLG_IME) == CDLG_STATUS_RUNNING && imedlg_take(t, sizeof t, &ok)) {
		if (ok) ime_write(t);
		cdlg_finish(cdlg, CDLG_IME, ok ? CDLG_RESULT_OK : CDLG_RESULT_USER_CANCELED);
	}
	const int st = cdlg_status(cdlg, CDLG_IME);
	pthread_mutex_unlock(&cdlg_lock);
	RET(st);
}
static void h_ime_dlg_abort(Cpu *c) { imedlg_close(); cdlg_call(c, CDLG_IME, 4); }
#define CDLG_HANDLERS(kind, name) \
	static __attribute__((unused)) void h_##name##_status(Cpu *c) { cdlg_call(c, kind, 1); } \
	static __attribute__((unused)) void h_##name##_result(Cpu *c) { cdlg_call(c, kind, 2); } \
	static __attribute__((unused)) void h_##name##_term(Cpu *c) { cdlg_call(c, kind, 3); } \
	static __attribute__((unused)) void h_##name##_abort(Cpu *c) { cdlg_call(c, kind, 4); }
#define CDLG_INIT(kind, name) static void h_##name##_init(Cpu *c) { cdlg_call(c, kind, 0); }
CDLG_HANDLERS(CDLG_IME, cime)
CDLG_HANDLERS(CDLG_NP_MESSAGE, cnpmsg) CDLG_INIT(CDLG_NP_MESSAGE, cnpmsg)
CDLG_HANDLERS(CDLG_NP_PROFILE, cnpprof) CDLG_INIT(CDLG_NP_PROFILE, cnpprof)
CDLG_HANDLERS(CDLG_NP_SNS_FACEBOOK, cfb) CDLG_INIT(CDLG_NP_SNS_FACEBOOK, cfb)
CDLG_HANDLERS(CDLG_TW, ctw) CDLG_INIT(CDLG_TW, ctw)
CDLG_HANDLERS(CDLG_STORE_CHECKOUT, cstore) CDLG_INIT(CDLG_STORE_CHECKOUT, cstore)
/* SceNetCheckDialogResult {result, psnModeSucceeded, ...}: online (net.h), connected and, in the PSN modes, signed in (np.c's
 * local account) -- with psnModeSucceeded left 0 Killzone took its Multiplayer connect as a failed sign-in and went quietly
 * back to the menu. Offline the check cannot succeed: a Vita without a connection shows its "could not connect" page and the
 * user backs out -- SCE_COMMON_DIALOG_RESULT_USER_CANCELED (1). Answered OK offline, an online-only title (MHF-G) took the
 * check as passed and waited for a PSN sign-in for ever behind its dimmed title screen. */
static void h_net_dlg_result(Cpu *c) {
	if (A0) { WR32(A0, net_online() ? 0 : 1); WR32(A0 + 4, net_online() ? 1 : 0); }
	if (!net_online() && net_online_needs()) {   /* the game went for its online modes on a version they aren't for: say why it can't (net.h) */
		static double told; const double now = (double)time(0);
		if (now - told > 20) { told = now; char b[200]; snprintf(b, sizeof b, "This copy of the game isn't the version online play is for. Add update %s in the launcher, then play again.", net_online_needs());
			char t[64]; snprintf(t, sizeof t, "Online play needs update %s", net_online_needs()); overlay_toast_titled(t, b, 10); }
	}
	RET(0);
}
/* Common dialog 0x12, which no NID list names (Init 33626E2C, Term BEBAEF25, Abort B55CF0E3, GetStatus B7D4C911, GetResult
 * D1858A78 in libcdlg, between the Facebook and cross-controller dialogs; SDK 2.10+). Read from the firmware and Killzone: the param
 * after the common one is a SceNpCommunicationId (+0x50) and a 32-bit version the game keeps in its profile (+0x5c), the 0x2c-byte
 * result {result, kind, version}. Killzone opens it after the network check on the way into Multiplayer and stores the version
 * of kind 1, clears it on kind 0, fails on 3 and goes on unchanged on 2 -- PSN's terms of service, as the PS3's sceNpEula checks
 * did. Answered "nothing new to accept" (kind 2): there is no PSN to show terms for. */
static void h_eula_dlg_result(Cpu *c) { if (A0) { for (int i = 0; i < 0x2c; i += 4) WR32(A0 + i, 0); WR32(A0 + 4, 2); } RET(0); }
static void h_zero(Cpu *c) { RET(0); }
static void h_drm_open(Cpu *c) {   /* every add-on licensed; RECOMP_NP_LOG names the ones the game asks for (Killzone's Botzone: P000000000001957) */
	static int log = -1; if (log < 0) log = getenv("RECOMP_NP_LOG") != 0;
	/* RECOMP_ADDCONT_NONE=1: no add-on is licensed (the error the console gives for content not installed) -- a test that
	 * doesn't have a game's add-on files, or wants to start without them (Persona 4: Dancing All Night shows a notice per
	 * add-on at every boot, ~1500 frames of a scripted run) */
	static int none = -1; if (none < 0) none = getenv("RECOMP_ADDCONT_NONE") != 0;
	if (log && A0) { char id[18]; memcpy(id, G(A0), 17); id[17] = 0; fprintf(stderr, "    np: add-on licence %s -> %s\n", id, none ? "not installed" : "licensed"); }
	RET(none ? 0x80100604u : 0);
}
static void h_is_game(Cpu *c) { RET(1); }

/* ---- save data (SceAppUtil): savedata0: is <RECOMP_FS>/savedata0/. A slot exists once created or
 * saved: a file .slot<N> there holding its SceAppUtilSaveDataSlotParam, 0x34c bytes -- status +0,
 * title +0x4 (64 bytes), subTitle +0x44 (128), detail +0xc4 (512), iconPath +0x2c4, userParam +0x304,
 * sizeKB +0x308, modifiedTime +0x30c (SceDateTime: u16 year, month, day, hour, minute, second; u32
 * us). The texts are UTF-8 -- vitasdk's apputil.h declares them SceWChar16, but Uncharted writes
 * "Uncharted: Golden Abyss\u2122" / its chapter / "Difficulty: Normal..." as UTF-8 (2026-10-10).
 * The console stamps the modified time at every write; the host stamps its local clock. A marker
 * from before 2026-10-10 is empty: a slot with a blank param (the list dates it by the file). */
#define SLOT_PARAM_SIZE 0x34c
static const char *fs_root(void) { return getenv("RECOMP_FS") ? getenv("RECOMP_FS") : "fs"; }
static void slot_marker(uint32_t id, char *buf, size_t n) { snprintf(buf, n, "%s/savedata0/.slot%u", fs_root(), id); }
static int slot_read(uint32_t id, uint8_t *p) {   /* the slot's param into p (zeros past what the file holds); 0: no such slot */
	char m[1024]; slot_marker(id, m, sizeof m);
	memset(p, 0, SLOT_PARAM_SIZE);
	FILE *f = fopen(m, "rb"); if (!f) return 0;
	if (fread(p, 1, SLOT_PARAM_SIZE, f)) {}
	fclose(f); return 1;
}
static int slot_write(uint32_t id, uint32_t param) {   /* param: a guest SceAppUtilSaveDataSlotParam, or 0 to keep the slot's own; stamps the time */
	uint8_t p[SLOT_PARAM_SIZE];
	if (param) memcpy(p, G(param), SLOT_PARAM_SIZE); else slot_read(id, p);
	time_t now = time(0); struct tm tm; localtime_r(&now, &tm);
	uint16_t dt[6] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_mday, (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec};
	memcpy(p + 0x30c, dt, sizeof dt); memset(p + 0x318, 0, 4);
	char dir[1024]; snprintf(dir, sizeof dir, "%s/savedata0", fs_root()); mkdir(dir, 0755);
	char m[1024]; slot_marker(id, m, sizeof m);
	FILE *f = fopen(m, "wb"); if (!f) return -1;
	fwrite(p, 1, SLOT_PARAM_SIZE, f); fclose(f);
	return 0;
}
/* sceAppUtilSaveDataMount(SceAppUtilSaveDataMount * (title id +0, +0x10 scratch), const char *passphrase,
 * ...) mounts ANOTHER title's saved data -- the saved-data bonus check (SSD probes four other Soul
 * Sacrifice titles). A host holding no such saved data must report SLOT_NOT_FOUND, the console's answer
 * for missing saved data: the game then skips the bonus. The former unimplemented stub returned success
 * for every call, so the game treated a mounted-but-empty directory as corrupt saved data and its save
 * flow dead-locked behind the error window's task. Mounting real foreign saved data stays unimplemented
 * and fails loudly here. */
static void h_save_data_mount(Cpu *c) {
	if (A0) { char id[10]; memcpy(id, G(A0), 9); id[9] = 0; fprintf(stderr, "    apputil: SaveDataMount foreign title %s -> SLOT_NOT_FOUND (no such saved data)\n", id); }
	RET(0x80100641u);   /* SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND */
}
static void h_save_data_umount(Cpu *c) { (void)c; RET(0); }   /* (mount point): nothing was mounted */
static void h_save_slot_create(Cpu *c) { RET(slot_write(A0, A1) == 0 ? 0 : 0x80100600u); }   /* (slot id, SceAppUtilSaveDataSlotParam *, mount point) */
static void h_save_slot_delete(Cpu *c) { char m[1024]; slot_marker(A0, m, sizeof m); RET(unlink(m) == 0 ? 0 : 0x80100641u); }   /* SLOT_NOT_FOUND */
static void h_save_slot_param(Cpu *c) {   /* (slot id, SceAppUtilSaveDataSlotParam *, mount point) */
	uint8_t p[SLOT_PARAM_SIZE];
	if (!slot_read(A0, p)) { RET(0x80100641u); return; }   /* SLOT_NOT_FOUND */
	if (A1) memcpy(G(A1), p, SLOT_PARAM_SIZE);
	RET(0);
}
/* sceAppUtilSaveDataSlotSearch(SceAppUtilWorkBuffer *work (buf +0, size +4), const SceAppUtilSaveDataSlotSearchCond *cond,
 * SceAppUtilSaveDataSlotSearchResult *result, mount point). Layout as the SDK declares it (from memory of its headers, not checked
 * against a console): cond = from +0, type +4 (0: slots that exist, 1: slots that don't), count +8, order +0xc (0 ascending, 1
 * descending); result = hitNum +0, slotList +4 (an array of slot ids, the game's own, else the work buffer). The first calls are
 * still dumped (0x60 bytes behind each argument) so that a wrong guess shows in the log. Answers from the real slot files (.slot<N>). */
#define SLOT_SEARCH_MAX 256
static int guest_ptr_ok(uint32_t a) { return a >= 0x10000000u && a < 0x80000000u && !(a & 3); }
static void h_save_slot_search(Cpu *c) {
	static int n;
	uint32_t work = A0, cond = A1, result = A2;
	if (n++ < 6) {
		uint32_t a[3] = { A0, A1, A2 };
		for (int k = 0; k < 3; k++) {
			if (!guest_ptr_ok(a[k])) { fprintf(stderr, "    apputil: SaveDataSlotSearch #%d r%d = %#x\n", n, k, a[k]); continue; }
			fprintf(stderr, "    apputil: SaveDataSlotSearch #%d r%d = %#x:", n, k, a[k]);
			for (int w = 0; w < 24; w++) fprintf(stderr, " %08x", RD32(a[k] + 4u * w));
			fprintf(stderr, "\n");
		}
	}
	if (!guest_ptr_ok(cond) || !guest_ptr_ok(result)) { RET(0x80100604u); return; }   /* (no cond / result to answer into) */
	/* what the game really sends (its log): 0 0 10 0 -- from +0, type +4 (0), count +8 (10 slots), order +0xc (0). The first guess had range at +4 */
	uint32_t from = RD32(cond), type = RD32(cond + 4), range = RD32(cond + 8), order = RD32(cond + 0xc);
	if (range > SLOT_SEARCH_MAX) range = SLOT_SEARCH_MAX;
	uint32_t list = RD32(result + 4);
	if (!guest_ptr_ok(list)) { list = guest_ptr_ok(RD32(work)) ? RD32(work) : 0; if (list) WR32(result + 4, list); }
	uint32_t hits = 0; uint8_t tmp[SLOT_PARAM_SIZE];
	for (uint32_t i = 0; i < range && list; i++) {
		uint32_t id = order == 1 ? from + range - 1 - i : from + i;
		int exists = slot_read(id, tmp);
		if (type == 1 ? !exists : exists) WR32(list + 4 * hits++, id);
	}
	WR32(result, hits);
	if (n <= 6) fprintf(stderr, "    apputil: SaveDataSlotSearch #%d from %u range %u type %u order %u -> %u hit(s)\n", n, from, range, type, order, hits);
	RET(0);
}
static void h_save_slot_set_param(Cpu *c) { RET(slot_write(A0, A1) == 0 ? 0 : 0x80100600u); }   /* (slot id, param, mount point): a slot not yet created is created */
/* (SceAppUtilSaveDataFileSlot * (id +0, slotParam +4), SceAppUtilSaveDataFile[], count, mount point, *required KB)
 * file: path +0, buf +4, size +8, offset +16 (64-bit), mode +24; 64 bytes each */
/* (NIGHT-01) the save keeps the console's minimum duration (hle_offline.h hle_save_latency_ms): a game whose worker
 * finished before the starter marked it busy waited for it forever ("Preparing save data..."); the run slot is free
 * meanwhile, as during any blocking call */
static void save_io_time(const struct timespec *t0) {
	static int ms = -1;
	if (ms < 0) ms = hle_save_latency_ms(getenv("RECOMP_SAVE_LATENCY_MS"));
	struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t left = hle_save_wait_ns(ms, (int64_t)(now.tv_sec - t0->tv_sec) * 1000000000 + (now.tv_nsec - t0->tv_nsec));
	if (!left) return;
	int b = sched_block_begin();
	struct timespec d = { (time_t)(left / 1000000000), (long)(left % 1000000000) };
	nanosleep(&d, 0);
	if (b) sched_block_end();
}
static void h_save_data_save(Cpu *c) {
	struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
	char dir[1024]; snprintf(dir, sizeof dir, "%s/savedata0", fs_root()); mkdir(dir, 0755);
	for (uint32_t i = 0; i < A2; i++) {
		uint32_t f = A1 + 64 * i, path = RD32(f), buf = RD32(f + 4), size = RD32(f + 8), mode = RD32(f + 24);
		uint64_t off = RD64(f + 16);
		char hp[1024]; snprintf(hp, sizeof hp, "%s/%s", dir, path ? (char *)G(path) : "");
		/* '\' separates as in io.c; the console creates the parent folders (Uncharted saves
		 * "slot_000/saves\checkpoint.bin" without a directory entry: every save failed until 2026-10-09) */
		for (char *q = hp + strlen(dir); *q; q++) if (*q == '\\') *q = '/';
		for (char *q = hp + strlen(dir) + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(hp, 0755); *q = '/'; }
		if (mode == 2) { mkdir(hp, 0755); continue; }   /* a directory */
		int fd = open(hp, O_WRONLY | O_CREAT | (mode == 1 ? O_TRUNC : 0), 0644);
		if (fd < 0) { fprintf(stderr, "    save %s (mode %u, %u bytes): host open failed: %s\n", path ? (char *)G(path) : "", mode, size, strerror(errno)); continue; }
		if (buf && size) { gwatch_read_open(buf, size); pwrite(fd, G(buf), size, (off_t)off); }
		close(fd);
		fprintf(stderr, "    save %s: %u bytes at %llu\n", path ? (char *)G(path) : "", size, (unsigned long long)off);
	}
	if (A0) slot_write(RD32(A0), RD32(A0 + 4));   /* the slot: created or updated, its param replaced when one is given */
	if (STK(0)) WR32(STK(0), 0);
	save_io_time(&t0);
	RET(0);
}
static void h_save_data_remove(Cpu *c) {   /* (slot, SceAppUtilSaveDataRemoveItem[] (44 bytes: path +0), count, mount) */
	for (uint32_t i = 0; i < A2; i++) {
		char hp[1024]; snprintf(hp, sizeof hp, "%s/savedata0/%s", fs_root(), (char *)G(RD32(A1 + 0x2c * i)));
		for (char *q = hp + strlen(fs_root()); *q; q++) if (*q == '\\') *q = '/';
		unlink(hp);
	}
	RET(0);
}

/* ---- the save-data dialog (SceSaveDataDialog): the system's own save / load / delete screens -- the
 * game's slots as a list to pick from, and the cards around it (Overwrite?, Saving..., Saving
 * complete). The console's state machine (Vita3K's SceCommonDialog.cpp is the reference): Init and
 * Continue open a step and its sub-status runs -- a list until a pick, a yes/no card until an
 * answer, a card with no buttons until the game's SubClose -- the game reads the answer with
 * GetResult once the sub-status reads FINISHED, Continues with the next step, then Finish and Term.
 * The overlay draws the step and answers it (savedlg.h, overlay.cpp's build_save_dialog). A card with
 * only an OK button never waits: it answers OK and its message shows as a toast. With no window
 * to answer in (a headless run, the regression gate), RECOMP_SAVE_DIALOG=auto or the setting
 * save_dialog=auto, every question answers itself: the list's focused row (RECOMP_SAVE_DIALOG_ROW=n: row n), YES -- the old behaviour,
 * which knew no list (until 2026-10-10 every dialog finished at once with YES and slot 0).
 * The host is lenient where the console returns an error for a call out of order (a Continue or
 * Finish before the step finished, a second Init): the step it asks for happens. */
enum { ST_NONE, ST_RUNNING, ST_FINISHED };   /* SceCommonDialogStatus */
static pthread_mutex_t sd_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
	int open, status, sub;              /* Init..Term; the dialog's status and its step's */
	int mode, disp, result, button;     /* GetResult: the step's mode, its result (0 OK, 1 canceled, 2 aborted, or an error), the button */
	uint32_t slot, userdata;            /* the slot acted on (the list's pick, a card's target slot) */
	int kind, seq, focus, percent;      /* what the overlay shows (savedlg.h) */
	char title[128], msg[512];
	int nrows; SaveDlgRow rows[256];
} sd;
/* a slot's text field (`bytes` long, or to a NUL) as display text: UTF-8 as the games write it (UTF-16LE
 * read as such should a game follow the SDK's header: a non-NUL first byte then a NUL), a line break as
 * a separator, and the trademark signs dropped (the overlay's bitmap font has no glyph for them) */
static void slot_text(const uint8_t *s, size_t bytes, char *out, size_t n) {
	size_t o = 0;
	const int wide = s && bytes >= 2 && s[0] && !s[1];
	for (size_t i = 0; s && i < bytes && o + 4 < n; ) {
		uint32_t u;
		if (wide) { if (i + 1 >= bytes || !(u = s[i] | s[i + 1] << 8)) break; i += 2; }
		else {
			if (!s[i]) break;
			const uint8_t b = s[i]; int k = b < 0x80 ? 1 : b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : 2;
			if (i + k > bytes) break;
			u = k == 1 ? b : k == 2 ? (b & 0x1fu) << 6 | (s[i + 1] & 0x3fu) : k == 3 ? (b & 0x0fu) << 12 | (s[i + 1] & 0x3fu) << 6 | (s[i + 2] & 0x3fu)
			  : (b & 0x07u) << 18 | (s[i + 1] & 0x3fu) << 12 | (s[i + 2] & 0x3fu) << 6 | (s[i + 3] & 0x3fu);
			i += (size_t)k;
		}
		if (u == 0x2122 || u == 0xae || u == 0xa9) continue;   /* (TM) (R) (C) */
		if (u == '\n' || u == '\r') { if (o && out[o - 1] != ' ') { memcpy(out + o, "  -  ", 5); o += 5; } continue; }
		if (u < 0x80) out[o++] = (char)u;
		else if (u < 0x800) { out[o++] = (char)(0xc0 | u >> 6); out[o++] = (char)(0x80 | (u & 0x3f)); }
		else if (u < 0x10000) { out[o++] = (char)(0xe0 | u >> 12); out[o++] = (char)(0x80 | (u >> 6 & 0x3f)); out[o++] = (char)(0x80 | (u & 0x3f)); }
		else { out[o++] = (char)(0xf0 | u >> 18); out[o++] = (char)(0x80 | (u >> 12 & 0x3f)); out[o++] = (char)(0x80 | (u >> 6 & 0x3f)); out[o++] = (char)(0x80 | (u & 0x3f)); }
	}
	while (o >= 5 && !memcmp(out + o - 5, "  -  ", 5)) o -= 5;
	if (n) out[o] = 0;
}
/* one row of the list: the slot's saved data, or the game's "new saved data" row (empty: the slot's
 * SceAppUtilSaveDataSlotEmptyParam, title +0). An empty slot the game offers no such row for isn't
 * listed (the console's list) */
static void sd_add_row(uint32_t id, uint32_t empty) {
	uint8_t p[SLOT_PARAM_SIZE]; SaveDlgRow *r = &sd.rows[sd.nrows];
	memset(r, 0, sizeof *r); r->id = id;
	if ((r->exists = slot_read(id, p))) {
		slot_text(p + 0x4, 64, r->title, sizeof r->title);
		slot_text(p + 0x44, 128, r->sub, sizeof r->sub);
		const uint8_t *d = p + 0xc4; while (*d == '\n' || *d == '\r') d++;   /* (Uncharted's detail starts with a line break) */
		slot_text(d, 512 - (size_t)(d - (p + 0xc4)), r->detail, sizeof r->detail);
		if (!r->title[0]) snprintf(r->title, sizeof r->title, "Saved data %u", id);
		uint16_t t[6]; memcpy(t, p + 0x30c, sizeof t);
		if (t[0]) snprintf(r->date, sizeof r->date, "%04u-%02u-%02u %02u:%02u", t[0], t[1], t[2], t[3], t[4]);
		else {   /* a marker from before slots kept their param: the file's own time */
			char m[1024]; struct stat st; slot_marker(id, m, sizeof m);
			if (!stat(m, &st)) { struct tm tm; localtime_r(&st.st_mtime, &tm); strftime(r->date, sizeof r->date, "%Y-%m-%d %H:%M", &tm); }
		}
	} else {
		if (!empty) return;
		uint32_t t = RD32(empty);
		if (t) slot_text(G(t), 64, r->title, sizeof r->title);
		if (!r->title[0]) snprintf(r->title, sizeof r->title, "New Saved Data");
	}
	sd.nrows++;
}
/* the row the list opens on (SceSaveDataDialogListParam focusPos +8, focusId +0xc): 0 the list's head,
 * 1 its tail, 2/3 the first/last empty row, 4/5 the first/last saved one, 6/7 the newest/oldest
 * saved one, 8 the row of focusId */
static int sd_focus(uint32_t lp) {
	uint32_t pos = RD32(lp + 8), fid = RD32(lp + 0xc); int f = -1;
	for (int i = 0; i < sd.nrows; i++) {
		const SaveDlgRow *r = &sd.rows[i]; int take = 0;
		switch (pos) {
		case 0: take = f < 0; break;
		case 1: take = 1; break;
		case 2: take = !r->exists && f < 0; break;
		case 3: take = !r->exists; break;
		case 4: take = r->exists && f < 0; break;
		case 5: take = r->exists; break;
		case 6: take = r->exists && (f < 0 || strcmp(r->date, sd.rows[f].date) > 0); break;
		case 7: take = r->exists && (f < 0 || strcmp(r->date, sd.rows[f].date) < 0); break;
		case 8: take = r->id == fid; break;
		}
		if (take) f = i;
	}
	return f < 0 ? 0 : f;
}
static const char *sd_verb(const char *save, const char *load, const char *del) { return sd.disp == 2 ? load : sd.disp == 3 ? del : save; }
static int sd_auto(void) {
	extern int overlay_draws_hud(void);
	if (!overlay_draws_hud()) return 1;   /* no window to answer in */
	const char *e = getenv("RECOMP_SAVE_DIALOG");
	if (e) return !strcmp(e, "auto");
	char b[16]; return !strcmp(settings_str("save_dialog", "ask", b, sizeof b), "auto");
}
static void sd_answer(int choice) {   /* (locked) the player's answer to the running step; savedlg.h */
	if (sd.kind == SAVEDLG_LIST && choice >= 0 && choice < sd.nrows) { sd.slot = sd.rows[choice].id; sd.result = 0; sd.button = 0; }
	else if (sd.kind == SAVEDLG_YESNO && (choice == SAVEDLG_YES || choice == SAVEDLG_NO)) { sd.result = 0; sd.button = choice; }
	else if (sd.kind == SAVEDLG_LIST || sd.kind == SAVEDLG_YESNO) { sd.result = 1; sd.button = 0; }   /* USER_CANCELED, button INVALID */
	else return;
	fprintf(stderr, "    save dialog: %s -> %s (slot %u)\n", sd.kind == SAVEDLG_LIST ? "list" : "yes/no", sd.result ? "canceled" : sd.kind == SAVEDLG_LIST ? "picked" : sd.button == SAVEDLG_YES ? "yes" : "no", sd.slot);
	sd.sub = ST_FINISHED; sd.kind = SAVEDLG_NONE;
}
/* (locked) open the step SceSaveDataDialogParam `prm` asks for: mode +0x50 (1 FIXED, 2 LIST, 3 USER_MSG,
 * 4 SYSTEM_MSG, 5 ERROR_CODE, 6 PROGRESS_BAR), dispType +0x54 (1 save, 2 load, 3 delete), the mode's
 * own param +0x58..+0x6c, userdata +0x78. Each card names its slot (a SceAppUtilSaveDataSlot: id +0). */
static void sd_step(uint32_t prm, int init) {
	int mode = (int)RD32(prm + 0x50), disp = (int)RD32(prm + 0x54);
	sd.mode = mode; if (disp || init) sd.disp = disp;
	if (init) sd.userdata = RD32(prm + 0x78);
	sd.status = sd.sub = ST_RUNNING; sd.result = 0; sd.button = 0;
	sd.kind = SAVEDLG_NONE; sd.percent = -1; sd.title[0] = sd.msg[0] = 0; sd.seq++;
	uint32_t target = 0; int buttons = -1;   /* a card's slot; its buttons (0 none, 1 OK, 2 yes/no) */
	static const char *const sysmsg[] = {0, "There is no saved data.", 0, "Do you want to overwrite this saved data?", 0, 0, 0, 0, "The saved data is corrupted.", "There is not enough free space to save.", 0, 0, "There is no saved data that can be used with this application."};
	switch (mode) {
	case 1: target = RD32(prm + 0x58); sd.sub = ST_FINISHED; break;   /* FIXED (targetSlot +0): nothing to browse, done at once */
	case 2: {   /* LIST: slotList +0, slotListSize +4, focus +8/+0xc, listTitle +0x10 */
		uint32_t lp = RD32(prm + 0x5c); if (!lp) break;
		uint32_t list = RD32(lp), n = RD32(lp + 4), lt = RD32(lp + 0x10);
		if (n > 256) n = 256;
		snprintf(sd.title, sizeof sd.title, "%s", lt ? (const char *)G(lt) : sd_verb("Save", "Load", "Delete"));
		sd.nrows = 0;
		for (uint32_t i = 0; i < n; i++) if (RD32(list + 16 * i) >= 256) {   /* an id past the 256 slots: the step fails at once, slot -1 */
			sd.slot = ~0u; sd.result = (int)0x80100b01; sd.sub = ST_FINISHED; return;   /* SCE_SAVEDATA_DIALOG_ERROR_PARAM */
		}
		for (uint32_t i = 0; i < n; i++) sd_add_row(RD32(list + 16 * i), RD32(list + 16 * i + 12));
		sd.focus = sd_focus(lp);
		sd.kind = SAVEDLG_LIST;
		break;
	}
	case 3: {   /* USER_MSG: buttonType +0 (0 OK, 1 yes/no, 2 none), msg +4 (UTF-8), targetSlot +8 */
		uint32_t up = RD32(prm + 0x60); if (!up) break;
		uint32_t bt = RD32(up), m = RD32(up + 4); target = up + 8;
		snprintf(sd.msg, sizeof sd.msg, "%s", m ? (const char *)G(m) : "");
		buttons = bt == 0 ? 1 : bt == 1 ? 2 : 0;
		break;
	}
	case 4: {   /* SYSTEM_MSG: sysMsgType +0, value +4, targetSlot +8 */
		uint32_t sp = RD32(prm + 0x64); if (!sp) break;
		if (init && !disp) { sd.slot = ~0u; sd.result = (int)0x80100b01; sd.status = ST_FINISHED; return; }   /* (the console: an Init with no dispType ends the dialog with ERROR_PARAM; a Continue's 0 keeps the dialog's) */
		uint32_t type = RD32(sp); target = sp + 8;
		switch (type) {
		case 2: snprintf(sd.msg, sizeof sd.msg, "%s", sd_verb("Do you want to save?", "Do you want to load this saved data?", "Do you want to delete this saved data?")); buttons = 2; break;   /* CONFIRM */
		case 3: buttons = 2; break;   /* OVERWRITE */
		case 5: snprintf(sd.msg, sizeof sd.msg, "%s", sd_verb("Saving... Do not power off the system.", "Loading...", "Please wait...")); buttons = 0; break;   /* PROGRESS */
		case 6: snprintf(sd.msg, sizeof sd.msg, "%s", sd_verb("Saving complete.", "Loading complete.", "Deletion complete.")); buttons = 1; break;   /* FINISHED */
		case 7: snprintf(sd.msg, sizeof sd.msg, "%s", sd_verb("Do you want to cancel saving?", "Do you want to cancel loading?", "Do you want to cancel deleting?")); buttons = 2; break;   /* CONFIRM_CANCEL */
		case 4: buttons = 0; break;   /* NOSPACE: an information card */
		default: buttons = 1; break;  /* NODATA, FILE_CORRUPTED, NOSPACE_CONTINUABLE, NODATA_IMPORT */
		}
		if (!sd.msg[0] && type < sizeof sysmsg / sizeof *sysmsg && sysmsg[type]) snprintf(sd.msg, sizeof sd.msg, "%s", sysmsg[type]);
		break;
	}
	case 5: {   /* ERROR_CODE: errorCode +0, targetSlot +4 */
		uint32_t ep = RD32(prm + 0x68); if (!ep) break;
		snprintf(sd.msg, sizeof sd.msg, "An error occurred. (%08X)", RD32(ep)); target = ep + 4; buttons = 1;
		break;
	}
	case 6: {   /* PROGRESS_BAR: msg +0x3c, targetSlot +0x40 */
		uint32_t pp = RD32(prm + 0x6c); if (!pp) break;
		uint32_t m = RD32(pp + 0x3c); target = pp + 0x40;
		snprintf(sd.msg, sizeof sd.msg, "%s", m ? (const char *)G(m) : sd_verb("Saving...", "Loading...", "Please wait..."));
		sd.percent = 0; buttons = 0;
		break;
	}
	}
	if (target) sd.slot = RD32(target);
	if (buttons == 2) sd.kind = SAVEDLG_YESNO;
	else if (buttons == 0) sd.kind = SAVEDLG_WAIT;
	else if (buttons == 1) {   /* OK alone: answered at once, the message as a toast */
		extern void overlay_toast_titled(const char *title, const char *body, double seconds);
		sd.button = 1; sd.sub = ST_FINISHED;
		if (sd.msg[0]) overlay_toast_titled("Saves", sd.msg, 3.0);
	}
	fprintf(stderr, "    save dialog: %s mode %d (%s), slot %d%s%s%s\n", init ? "init" : "continue", mode, sd_verb("save", "load", "delete"), (int)sd.slot,
		sd.kind == SAVEDLG_LIST ? ", a list" : "", sd.msg[0] ? ": " : "", sd.msg);
	if (sd.kind == SAVEDLG_LIST) for (int i = 0; i < sd.nrows; i++)
		fprintf(stderr, "      %c slot %u: %s%s%s%s%s  %s\n", i == sd.focus ? '>' : ' ', sd.rows[i].id, sd.rows[i].title, sd.rows[i].sub[0] ? " / " : "", sd.rows[i].sub,
			sd.rows[i].detail[0] ? " / " : "", sd.rows[i].detail, sd.rows[i].exists ? sd.rows[i].date : "(empty)");
	if ((sd.kind == SAVEDLG_LIST || sd.kind == SAVEDLG_YESNO) && sd_auto())
		sd_answer(sd.kind == SAVEDLG_YESNO ? SAVEDLG_YES : !sd.nrows ? SAVEDLG_CANCEL
		          : getenv("RECOMP_SAVE_DIALOG_ROW") ? atoi(getenv("RECOMP_SAVE_DIALOG_ROW")) % sd.nrows : sd.focus);   /* (a test's pick: that row) */
}
static void h_save_dlg_init(Cpu *c) {
	if (!A0) { RET(0x80020402u); return; }   /* SCE_COMMON_DIALOG_ERROR_NULL */
	pthread_mutex_lock(&sd_mu); sd.open = 1; sd_step(A0, 1); pthread_mutex_unlock(&sd_mu);
	RET(0);
}
static void h_save_dlg_continue(Cpu *c) {
	if (!A0) { RET(0x80020402u); return; }
	pthread_mutex_lock(&sd_mu);
	int ok = sd.open && sd.status == ST_RUNNING;
	if (ok) sd_step(A0, 0);
	pthread_mutex_unlock(&sd_mu);
	RET(ok ? 0 : 0x80020404u);   /* NOT_RUNNING */
}
/* the dialog's status: RUNNING until Finish, then FINISHED until Term */
static void h_save_dlg_status(Cpu *c) {
	pthread_mutex_lock(&sd_mu); int s = !sd.open || (sd.status != ST_RUNNING && sd.sub != ST_RUNNING) ? ST_NONE : sd.status; pthread_mutex_unlock(&sd_mu);
	RET(s);
}
static void h_save_dlg_substatus(Cpu *c) {   /* the step's: RUNNING while the player (or the game's card) has it, FINISHED once answered */
	pthread_mutex_lock(&sd_mu); int s = !sd.open || sd.status != ST_RUNNING ? ST_NONE : sd.sub; pthread_mutex_unlock(&sd_mu);
	RET(s);
}
/* (SceSaveDataDialogResult *): mode +0, result +4, buttonId +8, slotId +0xc, slotInfo +0x10 (isExist +0,
 * slotParam * +4), userdata +0x14 -- once the step or the dialog has finished */
static void h_save_dlg_result(Cpu *c) {
	if (!A0) { RET(0x80020402u); return; }
	pthread_mutex_lock(&sd_mu);
	int done = sd.open && (sd.status == ST_FINISHED || sd.sub == ST_FINISHED);
	if (done) {
		WR32(A0, sd.mode); WR32(A0 + 4, sd.result); WR32(A0 + 8, sd.button); WR32(A0 + 0xc, sd.slot); WR32(A0 + 0x14, sd.userdata);
		uint32_t si = RD32(A0 + 0x10);
		if (si) {
			uint8_t p[SLOT_PARAM_SIZE]; int ex = sd.slot < 256 && slot_read(sd.slot, p);
			WR32(si, ex); uint32_t sp = RD32(si + 4);
			if (sp && ex) memcpy(G(sp), p, SLOT_PARAM_SIZE);
		}
	}
	pthread_mutex_unlock(&sd_mu);
	RET(done ? 0 : 0x80020410u);   /* NOT_FINISHED */
}
static void h_save_dlg_finish(Cpu *c) {   /* the dialog's last step is done: FINISHED until Term */
	pthread_mutex_lock(&sd_mu);
	int ok = sd.open && sd.status == ST_RUNNING;
	if (ok) { sd.status = ST_FINISHED; sd.sub = ST_RUNNING; sd.kind = SAVEDLG_NONE; sd.percent = -1; }
	pthread_mutex_unlock(&sd_mu);
	RET(ok ? 0 : 0x80020404u);
}
/* the common-dialog Term contract: a Term with no dialog in use returns
 * SCE_COMMON_DIALOG_ERROR_NOT_IN_USE, as the console does (the game's dialog flow reads it); a
 * real Init..Term still terminates and returns 0 */
static void h_save_dlg_term(Cpu *c) {
	pthread_mutex_lock(&sd_mu);
	int in_use = sd.open;
	sd.open = 0; sd.status = sd.sub = ST_NONE; sd.kind = SAVEDLG_NONE;
	pthread_mutex_unlock(&sd_mu);
	RET(in_use ? 0 : 0x80020411u);   /* NOT_IN_USE */
}
static void h_save_dlg_abort(Cpu *c) {
	pthread_mutex_lock(&sd_mu); sd.result = 2; sd.button = 0; sd.open = 0; sd.status = ST_FINISHED; sd.kind = SAVEDLG_NONE; pthread_mutex_unlock(&sd_mu);   /* ABORTED */
	RET(0);
}
static void h_save_dlg_subclose(Cpu *c) {   /* the game ends a step itself (a card with no buttons) */
	pthread_mutex_lock(&sd_mu);
	int ok = sd.open && sd.sub == ST_RUNNING;
	if (ok) { sd.sub = ST_FINISHED; sd.result = 0; sd.button = 0; sd.kind = SAVEDLG_NONE; }
	pthread_mutex_unlock(&sd_mu);
	RET(ok ? 0 : 0x80020404u);
}
static void h_save_dlg_bar_inc(Cpu *c) { pthread_mutex_lock(&sd_mu); if (sd.percent >= 0) { sd.percent += (int)A1; if (sd.percent > 100) sd.percent = 100; } pthread_mutex_unlock(&sd_mu); RET(0); }   /* (target, delta) */
static void h_save_dlg_bar_set(Cpu *c) { pthread_mutex_lock(&sd_mu); if (sd.percent >= 0) sd.percent = A1 > 100 ? 100 : (int)A1; pthread_mutex_unlock(&sd_mu); RET(0); }   /* (target, rate) */
int savedlg_view(SaveDlgView *v) {
	pthread_mutex_lock(&sd_mu);
	int live = sd.open && sd.status == ST_RUNNING && sd.sub == ST_RUNNING;
	v->kind = live ? sd.kind : SAVEDLG_NONE; v->seq = sd.seq; v->disp = sd.disp; v->focus = sd.focus; v->percent = sd.percent;
	memcpy(v->title, sd.title, sizeof v->title); memcpy(v->msg, sd.msg, sizeof v->msg);
	v->nrows = sd.nrows; v->rows = sd.rows;
	return v->kind;
}
void savedlg_release(void) { pthread_mutex_unlock(&sd_mu); }
void savedlg_answer(int seq, int choice) {
	pthread_mutex_lock(&sd_mu);
	if (sd.open && seq == sd.seq && sd.status == ST_RUNNING && sd.sub == ST_RUNNING) sd_answer(choice);
	pthread_mutex_unlock(&sd_mu);
}

/* ---- trophies: trophy.c (HLE-LIB-02) */

/* (HLE-LIB-03) sceAppUtilSaveDataGetQuota(SceSize *quotaKiB, SceSize *usedKiB, mount point): the quota is the game's
 * PARAM.SFO SAVEDATA_MAX_SIZE (KiB), the use the files in savedata0 (each rounded up to a KiB) */
static uint64_t dir_kib(const char *dir) {
	uint64_t k = 0; DIR *d = opendir(dir); if (!d) return 0;
	for (struct dirent *e; (e = readdir(d)); ) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); struct stat st;
		if (stat(p, &st)) continue;
		k += S_ISDIR(st.st_mode) ? dir_kib(p) : ((uint64_t)st.st_size + 1023) / 1024;
	}
	closedir(d); return k;
}
static void h_save_quota(Cpu *c) {
	if (!A0 && !A1) { RET(0x80100600u); return; }   /* SCE_APPUTIL_ERROR_PARAMETER */
	char p[1024]; snprintf(p, sizeof p, "%s/savedata0", fs_root()); const uint64_t used = dir_kib(p);
	uint32_t quota = 0;
	snprintf(p, sizeof p, "%s/app0/sce_sys/param.sfo", fs_root());
	FILE *f = fopen(p, "rb");
	if (f) { uint8_t b[16384]; const size_t n = fread(b, 1, sizeof b, f); fclose(f); hle_sfo_int(b, n, "SAVEDATA_MAX_SIZE", &quota); }
	if (quota < used) quota = (uint32_t)used;   /* (no SFO value: what is there) */
	if (A0) WR32(A0, quota);
	if (A1) WR32(A1, (uint32_t)used);
	RET(0);
}
/* (HLE-LIB-03) the system's screenshot settings (the host takes its own, F12) and the LiveArea frame sync: accepted */

/* system events (suspend/resume and the like): none ever arrives; the real call returns an error
 * when the queue is empty, which ends a game's drain loop */
static void h_receive_system_event(Cpu *c) { RET(0x80802016u); }

/* system settings (id, *value): 1 language (1 English US), 2 enter button (1 cross), 4 date format
 * (0 YYYY/MM/DD), 5 time format (0 12-hour), 6 time zone (0 minutes: UTC, which is what the host's
 * RTC reports as local time too), 7 summer time (0 off); 3 is the user name, a string. Anything else
 * is SCE_APPUTIL_ERROR_PARAMETER, as on the console. */
static void h_system_param_int(Cpu *c) {
	if (A0 < 1 || A0 > 7 || A0 == 3 || !A1) { RET(0x80100600u); return; }
	WR32(A1, A0 == 1 || A0 == 2 ? 1 : 0); RET(0);
}
/* (id, buffer, size): only the user name (3) is a string -- "Player" (RECOMP_USERNAME overrides),
 * truncated to the buffer like strncpy plus a terminator; Killzone asks for it at boot (17 bytes:
 * the SDK's 16-character limit) */
static void h_system_param_string(Cpu *c) {
	if (A0 != 3 || !A1 || !A2) { RET(0x80100600u); return; }
	const char *name = getenv("RECOMP_USERNAME") ? getenv("RECOMP_USERNAME") : "Player";
	size_t n = strlen(name); if (n > 16) n = 16; if (n > A2 - 1) n = A2 - 1;
	memcpy(G(A1), name, n); WR8(A1 + (uint32_t)n, 0);
	RET(0);
}
/* app events (a LiveArea launch argument, an invitation, a Near gift): none ever arrives on the host.
 * ReceiveAppEvent with an empty queue fails (the AppMgr's "no parameter in this state"), so a game's
 * drain loop ends and it takes nothing as an event; parsing an event it can never have received is
 * the parse error */
static void h_receive_app_event(Cpu *c) { if (A0) memset(G(A0), 0, 0x404); RET(0x80802013u); }   /* SceAppUtilAppEventParam: type + 1 KiB of data */
static void h_app_event_parse(Cpu *c) { RET(0x80100620u); }   /* SCE_APPUTIL_ERROR_APPEVENT_PARSE_INVALID_DATA */
/* SceKernelOpenPsId: the console's 16-byte id exposed to games (save ownership, online keys -- Mirage's moderation bans
 * by a machine id made from it, sent with the login). One per install: random, made once and kept (the shared settings'
 * "open_psid"), so a ban or a server's record is one player's, never every Nativ player's -- it was one fixed id for all.
 * A game that already has saves keeps the old fixed id (written to its own settings), in case it tied them to the
 * console; so does a scripted run (RECOMP_HEADLESS), for the gate. */
static void open_psid(uint8_t id[16]) {
	static const uint8_t fixed[16] = {0x56, 0x52, 0x45, 0x43, 0x4f, 0x4d, 0x50, 0x2d, 0x48, 0x4f, 0x53, 0x54, 0x00, 0x00, 0x00, 0x01};   /* "VRECOMP-HOST" */
	static uint8_t cur[16]; static int have;
	if (have) { memcpy(id, cur, 16); return; }
	char b[48]; settings_str("open_psid", "", b, sizeof b);
	int ok = strlen(b) == 32;
	for (int i = 0; ok && i < 16; i++) { unsigned v; ok = sscanf(b + 2 * i, "%2x", &v) == 1; cur[i] = (uint8_t)v; }
	if (!ok) {
		char dir[1024]; snprintf(dir, sizeof dir, "%s/savedata0", fs_root());
		int saves = 0;
		DIR *d = opendir(dir);
		if (d) { struct dirent *e; while ((e = readdir(d))) saves |= e->d_name[0] != '.'; closedir(d); }
		int keep = saves || getenv("RECOMP_HEADLESS");
		if (keep) memcpy(cur, fixed, 16); else plat_random(cur, 16);
		for (int i = 0; i < 16; i++) snprintf(b + 2 * i, 3, "%02x", cur[i]);
		if (!getenv("RECOMP_HEADLESS")) { if (keep) settings_set_own("open_psid", b); else settings_set_str("open_psid", b); settings_save(); }
	}
	have = 1; memcpy(id, cur, 16);
}
static void h_open_psid(Cpu *c) {
	if (!A0) { RET(0x80020006u); return; }   /* ILLEGAL_ADDR */
	uint8_t id[16]; open_psid(id);
	memcpy(G(A0), id, 16); RET(0);
}

/* ---- SceAvPlayer (movies: Killzone's intro): no video decoder exists on the host, so the player
 * fails honestly -- Init gives a handle (it only allocates on the console), adding a source fails
 * with INVALID_PARAM, the player is never active and never has a frame. A game then skips the movie
 * as it would a missing file (Killzone's player update returns "done" when AddSource fails). */
static void h_avplayer_init(Cpu *c) { uint32_t h = guest_alloc(16, 16); memset(G(h), 0, 16); RET(A0 ? h : 0); }   /* (SceAvPlayerInitData *) */
static void h_avplayer_add_source(Cpu *c) {
	static int said; if (!said++) fprintf(stderr, "    avplayer: %s: no video decoder -- the source is refused\n", A1 ? (char *)G(A1) : "(null)");
	RET(0x806a0001u);   /* SCE_AVPLAYER_ERROR_INVALID_PARAM */
}
static void h_avplayer_frame(Cpu *c) { if (A1) memset(G(A1), 0, 0x20); RET(0); }   /* (handle, SceAvPlayerFrameInfo *): false, nothing decoded */
static void h_avplayer_no_stream(Cpu *c) { RET(0x806a0001u); }   /* GetStreamInfo / EnableStream / JumpToTime / Start with no source */
static void h_avplayer_time(Cpu *c) { c->r[0] = 0; c->r[1] = 0; }   /* CurrentTime: a 64-bit 0 ms */
static void h_app_state(Cpu *c) { memset(G(A0), 0, A1); RET(0); }   /* (SceAppMgrAppState *, size, version): nothing pending */
/* SceSysmodule: the host has every library built in, so loading is bookkeeping -- but IsLoaded answers only for the
 * modules the game loaded (SCE_SYSMODULE_ERROR_UNLOADED 0x805a1001 otherwise), as the system does: Soul Sacrifice
 * Delta asks whether the IME module (0x11) it never loads is loaded before calling sceImeUpdate, and "loaded"
 * plus the update stub's 0 read as "an on-screen keyboard is open", so its save-data dialog was never opened */
static uint8_t sysmodule_loaded[0x200];
static void h_sysmodule_load(Cpu *c) { if (A0 < sizeof sysmodule_loaded) __atomic_store_n(&sysmodule_loaded[A0], 1, __ATOMIC_RELAXED); RET(0); }
static void h_sysmodule_unload(Cpu *c) { if (A0 < sizeof sysmodule_loaded) __atomic_store_n(&sysmodule_loaded[A0], 0, __ATOMIC_RELAXED); RET(0); }
/* (HLE-PORT-01, HLE-LIB-06) SceIme, the in-game keyboard (libime.h; the IME dialog runs on cdlg.h, HLE-LIB-01): until
 * sceImeOpen no keyboard is open and sceImeUpdate says so (SCE_IME_ERROR_NOT_OPENED) -- Soul Sacrifice Delta polls
 * it before its save-data dialog. Open checks the SceImeParam (hle_offline.h) and shows HLE-LIB-05's host text card
 * (no window: the initial text counts as typed, as for the dialog); each Update hands the game's handler its events
 * on the calling thread: OPEN once, then the answer -- the text written into inputTextBuffer (UTF-16, at most
 * maxTextLength units and a 0) with UPDATE_TEXT and PRESS_ENTER, or PRESS_CLOSE when cancelled. The game closes it
 * with sceImeClose. Events go out after the lock is released: a handler may call sceImeClose itself */
static pthread_mutex_t ime_lock = PTHREAD_MUTEX_INITIALIZER;
static HleIme ime_lib;
static uint32_t ime_handler, ime_arg, ime_buf, ime_maxlen, ime_prev_len, ime_ev;
static int ime_card;
static char ime_init[IMEDLG_TEXT_MAX];
static void h_ime_open(Cpu *c) {
	HleImeParam p = {0};
	if (A0) { p.type = RD32(A0 + 0x14); p.option = RD32(A0 + 0x18); p.work = RD32(A0 + 0x1c); p.handler = RD32(A0 + 0x24); p.max_len = RD32(A0 + 0x30); p.input_buf = RD32(A0 + 0x34); p.enter_label = RD8(A0 + 0x38); }
	pthread_mutex_lock(&ime_lock);
	const uint32_t r = hle_ime_open(&ime_lib, A0 ? &p : 0);
	if (!r) {
		static uint16_t i16[HLE_IME_MAX_TEXT_LENGTH + 1];
		const uint32_t init = RD32(A0 + 0x2c); size_t k = 0;
		if (init) while (k < p.max_len && RD16(init + 2 * (uint32_t)k)) { i16[k] = RD16(init + 2 * (uint32_t)k); k++; }
		i16[k] = 0;
		imedlg_utf16_to_utf8(i16, k, ime_init, sizeof ime_init);
		ime_handler = p.handler; ime_arg = RD32(A0 + 0x20); ime_buf = p.input_buf; ime_maxlen = p.max_len; ime_prev_len = (uint32_t)k;
		ime_card = imedlg_open("", ime_init, p.max_len);
	}
	const int card = ime_card;
	pthread_mutex_unlock(&ime_lock);
	if (!r) fprintf(stderr, "    ime: keyboard opened (max %u)%s\n", p.max_len, card ? " -- the card asks the player" : " -- no window: the initial text is entered as typed");
	RET(r);
}
static void h_ime_update(Cpu *c) {
	pthread_mutex_lock(&ime_lock);
	if (!ime_lib.open) { pthread_mutex_unlock(&ime_lock); RET(HLE_IME_ERR_NOT_OPENED); return; }
	int answer = -1; char t[IMEDLG_TEXT_MAX];
	if (ime_lib.sent_open && !ime_lib.done) {
		int ok;
		if (!ime_card) { snprintf(t, sizeof t, "%s", ime_init); answer = 1; }
		else if (imedlg_take(t, sizeof t, &ok)) answer = ok;
	}
	uint32_t ids[3]; const int n = hle_ime_events(&ime_lib, answer, ids);
	uint32_t len = 0; int32_t change = 0;
	if (answer == 1) {
		static uint16_t t16[HLE_IME_MAX_TEXT_LENGTH + 1];
		len = (uint32_t)imedlg_utf8_to_utf16(t, t16, ime_maxlen);
		for (uint32_t k = 0; k <= len; k++) WR16(ime_buf + 2 * k, t16[k]);
		change = (int32_t)len - (int32_t)ime_prev_len; ime_prev_len = len;
	}
	if (!ime_ev) ime_ev = guest_alloc(HLE_IME_EVENT_SIZE, 8);
	const uint32_t fn = ime_handler, arg = ime_arg, buf = ime_buf, ev = ime_ev;
	pthread_mutex_unlock(&ime_lock);
	if (answer >= 0) fprintf(stderr, "    ime: %s\n", answer ? "text entered" : "cancelled");
	for (int i = 0; i < n; i++) {
		uint8_t b[HLE_IME_EVENT_SIZE]; hle_ime_event_bytes(b, ids[i], len, buf, change);
		memcpy(G(ev), b, sizeof b);
		guest_call(c, fn, arg, ev);
	}
	RET(0);
}
static void h_ime_close(Cpu *c) {
	pthread_mutex_lock(&ime_lock);
	const uint32_t r = hle_ime_close(&ime_lib);
	const int card = ime_card; if (!r) ime_card = 0;
	pthread_mutex_unlock(&ime_lock);
	if (!r && card) imedlg_close();
	RET(r);
}

/* ---- (HLE-LIB-06) SceLiveAreaUtil: no LiveArea screen on the host; the console's argument checks and a per-frame
 * revision table (hle_offline.h). Nothing asynchronous is ever pending */
static pthread_mutex_t la_lock = PTHREAD_MUTEX_INITIALIZER;
static HleLiveArea la_state;
static void h_la_status(Cpu *c) { RET(HLE_LA_ERR_NO_REQUEST); }
static void h_la_replace_all(Cpu *c) {
	pthread_mutex_lock(&la_lock); const uint32_t r = hle_la_replace_all(&la_state, A0 ? (const char *)G(A0) : 0); pthread_mutex_unlock(&la_lock);
	RET(r);
}
static void h_la_update_frame(Cpu *c) {   /* (format_version, frame_xml, frame_xml_length, contents_path, target_type) */
	const uint32_t target = STK(0);
	pthread_mutex_lock(&la_lock);
	const uint32_t r = hle_la_update_frame(&la_state, A0 ? (const char *)G(A0) : 0, A1 ? (const char *)G(A1) : 0, (int32_t)A2, A3 ? (const char *)G(A3) : 0, target);
	pthread_mutex_unlock(&la_lock);
	if (r) fprintf(stderr, "    livearea: UpdateFrameSync refused (%#x)\n", r);
	RET(r);
}
static void h_la_frame_revision(Cpu *c) {   /* (frame_id, SceUInt64 *revision) */
	uint64_t rev = 0;
	pthread_mutex_lock(&la_lock); const uint32_t r = hle_la_frame_revision(&la_state, A0 ? (const char *)G(A0) : 0, A1 != 0, &rev); pthread_mutex_unlock(&la_lock);
	if (!r) WR64(A1, rev);
	RET(r);
}
/* (HLE-LIB-06) sceAppUtilBgdlGetStatus: no background download ever ran -- a zeroed SceAppUtilBgdlStatus */
static void h_bgdl_status(Cpu *c) { if (!A0) { RET(HLE_APPUTIL_ERR_PARAMETER); return; } memset(G(A0), 0, HLE_BGDL_STATUS_SIZE); RET(0); }
/* (HLE-LIB-06) sceAppUtilStoreBrowse(SceAppUtilStoreBrowseParam *): on the console a success only hands the screen to
 * the PS Store, and the game carries on when the player comes back. There is no store offline: the hand-over is
 * logged and the game carries on at once, as after leaving the store */
static void h_store_browse(Cpu *c) {
	if (!A0) { RET(HLE_APPUTIL_ERR_PARAMETER); return; }
	const uint32_t id = RD32(A0 + 4);
	fprintf(stderr, "    store: browse (type %u%s%s) -- no PS Store offline, the game carries on\n", RD32(A0), id ? ", " : "", id ? (const char *)G(id) : "");
	RET(0);
}
/* (HLE-LIB-06) sceAppMgrAcquireBgmPort: the BGM port is contested only by the system's music player, which the host
 * doesn't have -- the game always gets it */
static void h_acquire_bgm_port(Cpu *c) { RET(0); }
/* (HLE-LIB-06) sceAppMgrLoadExec(path, argv, optParam): the console replaces this program by another SELF in app0.
 * A pack is one recompiled program, so a SELF that exists fails loudly (NOEXEC); a missing one gets the console's
 * INVALID_SELF_PATH (Uncharted EU asks for a card game SELF its app0 doesn't ship) */
static void h_load_exec(Cpu *c) {
	const char *path = A0 ? (const char *)G(A0) : 0;
	size_t argv_bytes = 0;
	if (A1) for (uint32_t i = 0, a; i < 64 && (a = RD32(A1 + 4 * i)); i++) argv_bytes += strlen((const char *)G(a));
	int exists = 0;
	if (path && !strncmp(path, "app0:", 5)) {
		const char *rest = path + 5; while (*rest == '/') rest++;
		char hp[1024]; struct stat st; snprintf(hp, sizeof hp, "%s/app0/%s", fs_root(), rest);
		exists = stat(hp, &st) == 0 && S_ISREG(st.st_mode);
	}
	const uint32_t r = hle_loadexec_check(path, A2 != 0, argv_bytes, exists);
	fprintf(stderr, "    sceAppMgrLoadExec(%s): %s (%#x)\n", path ? path : "(null)", r == HLE_APPMGR_ERR_NOEXEC ? "another program -- not supported, one recompiled program per pack" : "refused", r);
	RET(r);
}
static void h_sysmodule_is_loaded(Cpu *c) {
	RET(A0 < sizeof sysmodule_loaded && __atomic_load_n(&sysmodule_loaded[A0], __ATOMIC_RELAXED) ? 0 : 0x805a1001u);
}
/* sceAppMgrAppParamGetString(pid, param, buf, size): param 12 is the title id (np_matching2 asks for it); the others unknown */
/* sceAppUtilAppParamGetInt(paramId, int *value): id 0 is SKU_FLAG (SDK header, from memory): 1 trial, 3 full -- a game that is installed in full. */
static void h_app_param_int(Cpu *c) {
	if (!A1) { RET(0x80100600u); return; }
	if (A0 != 0) { static int said; if (!said++) fprintf(stderr, "    sceAppUtilAppParamGetInt: param %u unknown\n", A0); RET(0x80100600u); return; }
	WR32(A1, 3); RET(0);
}
static void h_app_param_string(Cpu *c) {
	if (!A2 || !A3) { RET(0x80800001u); return; }
	if (A1 != 12) { static int said; if (!said++) fprintf(stderr, "    sceAppMgrAppParamGetString: param %u unknown\n", A1); RET(0x80800001u); return; }
	const char *t = getenv("RECOMP_TITLE"); snprintf((char *)G(A2), A3, "%s", t ? t : "");
	RET(0);
}
const HleEntry hle_apputil[] = {
	{"sceImeUpdate", h_ime_update},   /* (HLE-PORT-01) */
	{"sceImeOpen", h_ime_open}, {"sceImeClose", h_ime_close},   /* (HLE-LIB-06) */
	{"sceLiveAreaGetStatus", h_la_status}, {"sceLiveAreaReplaceAllSync", h_la_replace_all}, {"sceLiveAreaGetFrameRevision", h_la_frame_revision},
	{"sceAppUtilBgdlGetStatus", h_bgdl_status}, {"sceAppUtilStoreBrowse", h_store_browse},
	{"sceAppMgrAcquireBgmPort", h_acquire_bgm_port}, {"sceAppMgrLoadExec", h_load_exec},
	{"sceAppUtilSaveDataGetQuota", h_save_quota},   /* (HLE-LIB-03) */
	{"sceScreenShotEnable", h_zero}, {"sceScreenShotDisable", h_zero}, {"sceScreenShotSetParam", h_zero}, {"sceScreenShotSetOverlayImage", h_zero},
	{"sceLiveAreaUpdateFrameSync", h_la_update_frame},   /* (HLE-LIB-06; HLE-LIB-03 accepted anything) */
	/* (HLE-LIB-01) SceCommonDialog on cdlg.h */
	{"sceImeDialogInit", h_ime_dlg_init}, {"sceImeDialogGetStatus", h_ime_dlg_status}, {"sceImeDialogGetResult", h_cime_result},
	{"sceImeDialogTerm", h_cime_term}, {"sceImeDialogAbort", h_ime_dlg_abort},
	{"sceNpMessageDialogInit", h_cnpmsg_init}, {"sceNpMessageDialogGetStatus", h_cnpmsg_status}, {"sceNpMessageDialogGetResult", h_cnpmsg_result},
	{"sceNpMessageDialogTerm", h_cnpmsg_term}, {"sceNpMessageDialogAbort", h_cnpmsg_abort},
	{"sceNpProfileDialogInit", h_cnpprof_init}, {"sceNpProfileDialogGetStatus", h_cnpprof_status}, {"sceNpProfileDialogGetResult", h_cnpprof_result},
	{"sceNpProfileDialogTerm", h_cnpprof_term}, {"sceNpProfileDialogAbort", h_cnpprof_abort},
	{"sceNpSnsFacebookDialogInit", h_cfb_init}, {"sceNpSnsFacebookDialogGetStatus", h_cfb_status}, {"sceNpSnsFacebookDialogGetResultLongToken", h_cfb_result},
	{"sceNpSnsFacebookDialogTerm", h_cfb_term}, {"sceNpSnsFacebookDialogAbort", h_cfb_abort},
	{"sceTwDialogInit", h_ctw_init}, {"sceTwDialogGetStatus", h_ctw_status}, {"sceTwDialogGetResult", h_ctw_result},
	{"sceTwDialogTerm", h_ctw_term}, {"sceTwDialogAbort", h_ctw_abort},
	{"sceStoreCheckoutDialogInit", h_cstore_init}, {"sceStoreCheckoutDialogGetStatus", h_cstore_status}, {"sceStoreCheckoutDialogGetResult", h_cstore_result},
	{"sceStoreCheckoutDialogTerm", h_cstore_term}, {"sceStoreCheckoutDialogAbort", h_cstore_abort},
	{"sceMsgDialogAbort", h_msg_dlg_abort}, {"sceMsgDialogClose", h_msg_dlg_abort}, {"sceNetCheckDialogAbort", h_net_dlg_abort},
	{"sceNpTrophySetupDialogAbort", h_trophy_dlg_abort}, {"sceCommonDialogSetConfigParam", h_zero},
	{"sceAppMgrAppParamGetString", h_app_param_string}, {"sceAppUtilAppParamGetInt", h_app_param_int},
	{"sceAppMgrReceiveSystemEvent", h_receive_system_event},
	{"sceAppUtilSystemParamGetInt", h_system_param_int}, {"_sceAppMgrGetAppState", h_app_state}, {"sceSysmoduleIsLoaded", h_sysmodule_is_loaded},
	{"sceSysmoduleLoadModule", h_sysmodule_load}, {"sceSysmoduleUnloadModule", h_sysmodule_unload},
	{"sceAppUtilSystemParamGetString", h_system_param_string},
	{"sceAppUtilInit", h_zero}, {"sceAppUtilShutdown", h_zero}, {"sceAppUtilDrmOpen", h_drm_open}, {"sceAppUtilDrmClose", h_zero},   /* (SceAppUtilDrmAddcontId *, mount point): the additional content's licence -- the host has no DRM, every add-on is licensed */   /* (SceAppUtilInitParam *, SceAppUtilBootParam *): nothing to set up */
	{"sceAppUtilReceiveAppEvent", h_receive_app_event},
	{"sceAppUtilAppEventParseLiveArea", h_app_event_parse}, {"sceAppUtilAppEventParseNearGift", h_app_event_parse},
	{"sceAppUtilAppEventParseNpInviteMessage", h_app_event_parse}, {"sceAppUtilAppEventParseNpBasicJoinablePresence", h_app_event_parse},
	{"sceAppUtilAppEventParseNpActivity", h_app_event_parse}, {"sceAppUtilAppEventParseNpAppDataMessage", h_app_event_parse},
	{"sceAppUtilAppEventParseSessionInvitation", h_app_event_parse}, {"sceAppUtilAppEventParseTeleport", h_app_event_parse},
	{"sceAppUtilAppEventParseGameCustomData", h_app_event_parse}, {"sceAppUtilAppEventParseWebBrowser", h_app_event_parse},
	{"sceKernelGetOpenPsId", h_open_psid},
	{"sceAppMgrSetNetworkDisconnectionWarningDialogState", h_zero},   /* the system's "network lost" dialog: never shown */
	/* power: always on mains, plenty of charge; the idle timers PowerTick resets don't exist */
	{"scePowerIsLowBattery", h_zero}, {"sceKernelPowerTick", h_zero}, {"scePowerSetConfigurationMode", h_zero},
	{"sceAvPlayerInit", h_avplayer_init}, {"sceAvPlayerAddSource", h_avplayer_add_source},
	{"sceAvPlayerIsActive", h_zero}, {"sceAvPlayerGetVideoData", h_avplayer_frame}, {"sceAvPlayerGetAudioData", h_avplayer_frame},
	{"sceAvPlayerStreamCount", h_zero}, {"sceAvPlayerGetStreamInfo", h_avplayer_no_stream}, {"sceAvPlayerEnableStream", h_avplayer_no_stream},
	{"sceAvPlayerStart", h_avplayer_no_stream}, {"sceAvPlayerJumpToTime", h_avplayer_no_stream}, {"sceAvPlayerCurrentTime", h_avplayer_time},
	{"sceAvPlayerClose", h_zero}, {"sceAvPlayerStop", h_zero}, {"sceAvPlayerPause", h_zero}, {"sceAvPlayerResume", h_zero},
	{"sceNpTrophySetupDialogInit", h_trophy_dlg_init},
	{"sceNpTrophySetupDialogGetStatus", h_trophy_dlg_status},
	{"sceNpTrophySetupDialogGetResult", h_dialog_result},
	{"sceNpTrophySetupDialogTerm", h_trophy_dlg_term},
	{"sceSaveDataDialogInit", h_save_dlg_init},
	{"sceSaveDataDialogGetStatus", h_save_dlg_status},
	{"sceSaveDataDialogGetResult", h_save_dlg_result},
	{"sceSaveDataDialogTerm", h_save_dlg_term}, {"sceSaveDataDialogContinue", h_save_dlg_continue}, {"sceSaveDataDialogGetSubStatus", h_save_dlg_substatus},
	{"sceSaveDataDialogFinish", h_save_dlg_finish}, {"sceSaveDataDialogSubClose", h_save_dlg_subclose}, {"sceSaveDataDialogAbort", h_save_dlg_abort},
	{"sceSaveDataDialogProgressBarInc", h_save_dlg_bar_inc}, {"sceSaveDataDialogProgressBarSetValue", h_save_dlg_bar_set},
	{"sceNetCheckDialogInit", h_net_dlg_init},
	{"sceNetCheckDialogGetStatus", h_net_dlg_status},
	{"sceNetCheckDialogGetResult", h_net_dlg_result},
	{"sceNetCheckDialogTerm", h_net_dlg_term},
	{"sceMsgDialogInit", h_msg_dlg_init},
	{"sceMsgDialogGetStatus", h_msg_dlg_status},
	{"sceMsgDialogGetResult", h_msg_dlg_result},
	{"sceMsgDialogTerm", h_msg_dlg_term},
	{"SceCommonDialog_33626E2C", h_eula_dlg_init}, {"SceCommonDialog_B7D4C911", h_eula_dlg_status},
	{"SceCommonDialog_D1858A78", h_eula_dlg_result}, {"SceCommonDialog_BEBAEF25", h_eula_dlg_term}, {"SceCommonDialog_B55CF0E3", h_eula_dlg_term},
	{"sceCommonDialogUpdate", h_zero},
	{"sceAppMgrIsGameProgram", h_is_game},   /* 1: a game -- libfios2 picks its default file mode by it */
	{"sceAppUtilSaveDataSlotGetParam", h_save_slot_param}, {"sceAppUtilSaveDataSlotCreate", h_save_slot_create}, {"sceAppUtilSaveDataSlotDelete", h_save_slot_delete}, {"sceAppUtilSaveDataSlotSetParam", h_save_slot_set_param}, {"sceAppUtilSaveDataSlotSearch", h_save_slot_search},
	{"sceAppUtilSaveDataDataSave", h_save_data_save},
	{"sceAppUtilSaveDataMount", h_save_data_mount}, {"sceAppUtilSaveDataUmount", h_save_data_umount},
	{"sceAppUtilSaveDataDataRemove", h_save_data_remove},
	{0, 0},
};
