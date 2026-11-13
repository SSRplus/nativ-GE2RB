/*
 * The in-game overlay: the one C++ translation unit in this project, so Dear ImGui
 * (third_party/imgui/) stays out of every other file. overlay.h is the plain-C contract the rest of
 * the host (window.c, mainly) calls through; this file never reaches back into window.c's own
 * statics -- it calls the small getters/setters window.c, gfx_gpu.c, metrics.c and settings.c each
 * expose for it, declared locally below, the same way every other file in this project declares an
 * `extern` for a function it needs instead of sharing a header (see e.g. window.c's own use of
 * gfx_gpu_render_scale_cycle).
 *
 * Two states, one ImGui frame per present (window.c's present loop calls overlay_frame, then
 * overlay_marks; exactly one of them builds, keyed on overlay_is_open()):
 *   - the full menu (build_ui; F10, Escape, Back+Start or Guide in window.c): a tabbed window with
 *     every setting, the controls reference and the help page. The game is paused while it's up
 *     (display.c), every event is ImGui's (window.c), keyboard and gamepad navigation are on.
 *   - closed (build_closed): the HUD -- the performance overlay's lines and the gamepad-touch
 *     markers -- plus the MENU BAR: a strip that slides down from the window's top edge when the
 *     pointer rests within a few pixels of it (or the pull tab at the top centre is tapped: touch
 *     has no hover), stays while the pointer is over it or one of its menus is open, and slides
 *     away about a second after the pointer leaves. The game keeps running under it; only the full
 *     menu (which the bar's reference pages open) pauses. Its menus edit the same settings the
 *     full menu's pages do, through the same setters, and every item with a hotkey shows it, so
 *     the bar teaches the keys. Pointer-only by design (no keyboard/gamepad navigation while
 *     closed: a game's keys and pad are never ImGui's then); window.c asks overlay_mouse_captured()
 *     before it turns a mouse event into a touch, so a click or drag on the bar never reaches the
 *     game. Also here: a one-time hint toast (settings key menu_hint_shown), Game > Pause without
 *     the menu, and the OS cursor hidden after a few seconds still over the picture.
 *   - the shader prep card (build_prep): a start whose shader warm-up would take more than a few seconds (gfx_gpu.c's
 *     shader prep: a first start, an update's new recipes, a cleared OS cache) pauses the game under it with the
 *     progress and Skip; it closes by itself when the builds are done. Skipped (or closed any way: Escape, F10, the
 *     quit card's Keep playing), the builds go on behind the game with a line in the corner until they're done.
 *   - the touch card (build_intro): the first game a player starts shows it over the game, paused as Game > Pause
 *     pauses it, so input keeps its in-game path (the hidden, captured mouse, Alt's touch pointer, a bumper's
 *     crosshair, the touchpad's marks): how to touch the Vita's screen with a keyboard and mouse, a controller or a
 *     touch screen, the device in use highlighted; done by a real touch on either panel (window_touch_contacts), or
 *     skipped with Escape or F10. Once per player: shared settings key touch_intro_shown.
 *   - the quit card (build_quit): a request to close the game window (Cmd+W, Cmd+Q, the close button, Alt+F4) opens it
 *     first, the game paused: Keep playing, or Quit; a second request while it's up quits (window.c).
 *   - headless, one-shot (overlay_render_forced, RECOMP_OVERLAY_DUMP: window.c's own comment) --
 *     never touches overlay_is_open()/open/close, so it can never leave the real pause/input/audio
 *     state the rest of the host keys off that open flag for stuck on. No platform backend (no
 *     window to attach one to): the caller's own w/h stand in for a display size, there is no
 *     pointer, and RECOMP_OVERLAY_DUMP_UI picks what to force on screen (see overlay.h).
 */
#include "imgui.h"
#include "imgui_internal.h"   /* ClosePopupsExceptModals (Escape with a bar menu open: overlay_bar_dismiss below); ImFontAtlasTextureBlockConvert (prompts_load) */
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "gpulock.h"   /* the SDL submit below is the locked one, and ImGui's own submits run under the same lock (render_frame): Homebrew SDL 3.4.16's Metal fence race, see that header */
#include "overlay.h"
#include "ui_text.h"
#include "savedlg_ui.h"
#include "imedlg_ui.h"
#include "imedlg.h"
#include "savedlg.h"
#include "ui_theme.h"
#include "setting_defaults.h"   /* (U7) the shared defaults for the rows' "default" marks */
#include "frame_rates.h"        /* the allowed rates the frame-rate rows offer */
#include "ui_scale.h"
#include "pad_family.h"   /* (UI-01e) the pad's own button names, shared with the launcher */
#include "touch_controls.h"   /* (UI-01n) the on-screen buttons' layout, shared with window.c */
#include "menu_shell.h"   /* (UI-03) the sections as tabs across the top, one pad flow (menu_walk_test) */
#include "menu_hint.h"   /* (UI-02a) the start hint, per device */
#include "intro_skip.h"   /* (INTRO-01) the touch card ends by itself */
#include "device_pick.h"   /* (UI-02b) the chosen controller / camera, "not connected" */
#include <string>
#include <vector>
#include "sysinfo.h"   /* the "Copy system info" text (U5) */
#define STBI_NO_STDIO
#include "third_party/stb/stb_image.h"   /* (its implementation: runtime/media/jpeg.c) */
#include "prompts_png.h"                 /* third_party/kenney-prompts/prompts.png as bytes (runtime/build.py writes it) */
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <ctype.h>
#include <mutex>

extern "C" {
#include "online/m2.h"   /* (online_status: the Game page's Online now) */
int net_online(void); const char *net_online_needs(void);
/* display.c */
void display_set_paused(int on);
/* window.c */
int window_get_aspect(void); void window_set_aspect(int mode);
int window_get_fullscreen(void); void window_set_fullscreen(int on); void window_set_size(int mult);
int window_raw_look(void); void window_set_raw_look(int on);
int window_controls_count(void);
int window_gyro_present(void);
void motion_recenter(void);
void window_controls_row(int i, const char **action, const char **key, const char **pad);
/* gfx_gpu.c: the GPU backend's internal resolution (1-gfx_gpu_render_scale_max) and F2's own cycle */
int gfx_gpu_render_scale(void); int gfx_gpu_render_scale_wanted(void); void gfx_gpu_render_scale_request(int n); void gfx_gpu_render_scale_cycle(void); int gfx_gpu_render_scale_max(void);
int gfx_gpu_hud_supported(void);   /* the game names its HUD draws (RECOMP_HUD_DRAWS): the HUD row applies */
int gfx_gpu_ssao_supported(void);   /* the game names its main 3D scene (RECOMP_SSAO): the ambient occlusion rows apply */
int gfx_gpu_fsr_supported(void);   /* the game's pack verified FSR 3.1 (RECOMP_FSR_SUPPORTED): the anti-aliasing row offers it */
int gfx_gpu_target_scale_supported(void);   /* the game names buffers it draws below the screen's resolution (RECOMP_TARGET_SCALE) */
int gfx_gpu_shader_prep(int *done, int *total, double *eta_s); void gfx_gpu_shader_prep_skip(void);   /* the shader prep card's state (build_prep) */
int display_pstv_looked(void);   /* display.c: the game asked for a PS TV's pad (the controller layout row applies) */
/* window.c: the swapchain's size in pixels (0 x 0 before the first frame); wider aspect support */
void window_pixel_size(uint32_t *w, uint32_t *h);
int window_aspect_supported(void);
float window_aspect_widen(void);
/* metrics.c: the F1 overlay's level, direct (not just cycling) and F1's cycle, and the live fps for the Performance page */
int metrics_osd_level(void); void metrics_osd_set(int level); void metrics_osd_cycle(void);
double metrics_fps(void);
int metrics_osd_lines(char (*out)[40], int max);   /* the F1 overlay's text, drawn here as the HUD (overlay_marks) */
int metrics_frame_times(float *ms, int max);       /* the full level's frame-time graph: recent game-frame durations, oldest first */
}
#include "metrics.h"   /* (MetricsSnapshot: the full level's HUD) */
extern "C" {
const char *window_present_mode(void);   /* window.c: vsync, mailbox or immediate */
int display_frame_rate_60(void);         /* display.c: the frame-rate setting */
int display_frame_rate(void);            /* display.c: the rate the game runs at now (30 | 60 | 120) */
int display_frame_rates_allowed(void);   /* display.c: the pack's allowed rates, a mask of frame_rates.h FR_* */
int gfx_gpu_fsr_active(void);            /* gfx_gpu.c: FSR upscales the frame */
/* settings.c */
int settings_has(const char *key);
int settings_int(const char *key, int def);
double settings_float(const char *key, double def);
void settings_set_int(const char *key, int v);
void settings_set_float(const char *key, double v);
void settings_set_str(const char *key, const char *v);
const char *settings_str(const char *key, const char *def, char *buf, int bufsize);
int settings_save(void);
int settings_reset_all(const char *const *keep);
extern const char *const settings_default_keep[];
}

/* (UI-02b) a device choice by name: "Automatic" (the first one), each connected device, and the chosen one marked "not
 * connected" while it isn't (the game then uses the first one) -- key: "pad_device" or "camera_device" */
static void save_settings(void);   /* (below) */
static volatile bool g_value_edit;   /* (UI-04: a value is being edited with the pad / the keys -- Esc ends that, not the menu: overlay_modal_open) */
static void device_choice(const char *id, const char *key, const std::vector<std::string> &names) {
	char want[128]; settings_str(key, "", want, sizeof want);
	std::vector<std::string> opts{ui_tr("ctrl.in_auto")}; for (const std::string &n : names) opts.push_back(n);
	int cur = 0;
	if (want[0]) {
		cur = -1;
		for (size_t i = 0; i < names.size(); i++) if (device_same(names[i].c_str(), want, 1)) cur = (int)i + 1;
		if (cur < 0) { opts.push_back(std::string(want) + " " + ui_tr("ctrl.not_connected")); cur = (int)opts.size() - 1; }
	}
	std::vector<std::string> ls; for (size_t i = 0; i < opts.size(); i++) ls.push_back(opts[i] + "##" + id + std::to_string(i));
	std::vector<const char *> cs; for (const std::string &l : ls) cs.push_back(l.c_str());
	int v = cur;
	if (ui::segmented(cs.data(), (int)cs.size(), &v) && v != cur) {
		if (v == 0) settings_set_str(key, "");
		else if (v <= (int)names.size()) settings_set_str(key, names[v - 1].c_str());
		save_settings();
	}
}
static std::vector<std::string> pad_list(void) {
	std::vector<std::string> v; int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);
	for (int i = 0; ids && i < n; i++) { const char *nm = SDL_GetGamepadNameForID(ids[i]); v.push_back(nm ? nm : "Controller"); }
	SDL_free(ids); return v;
}
static std::vector<std::string> camera_list(void) {
	std::vector<std::string> v;
	if (!SDL_WasInit(SDL_INIT_CAMERA) && !SDL_InitSubSystem(SDL_INIT_CAMERA)) return v;
	int n = 0; SDL_CameraID *ids = SDL_GetCameras(&n);
	for (int i = 0; ids && i < n; i++) { const char *nm = SDL_GetCameraName(ids[i]); v.push_back(nm ? nm : "Camera"); }
	SDL_free(ids); return v;
}
/* (U7) a settings row's mark: "default" while the value equals the shared default (setting_defaults.h), else the
 * row's timing note -- "next scene" (render scale applies at the game's next scene) or "after restart" (values the
 * game reads only at start) */
static const char *note_str(const char *key, const char *cur, const char *when = nullptr) {
	const char *d = setting_default(key);
	if (d && cur && !strcmp(d, cur)) return ui_tr("mark.default");
	return when;
}
static const char *note_s(const char *key, const char *when = nullptr) {
	const char *d = setting_default(key);
	char b[64]; settings_str(key, d ? d : "", b, sizeof b);
	return note_str(key, b, when);
}
static const char *note_int(const char *key, long long v, const char *when = nullptr) {
	char b[24]; std::snprintf(b, sizeof b, "%lld", v);
	return note_str(key, b, when);
}
static const char *note_flt(const char *key, double v, const char *when = nullptr) {
	char b[32]; std::snprintf(b, sizeof b, "%g", v);
	return note_str(key, b, when);
}
/* (U12b) a translated label with its ImGui id suffix ("Off" + "##ar" -> "Aus##ar"), in the caller's buffer */
static const char *tr_suf(const char *txt, const char *suf, char *buf, size_t n) { std::snprintf(buf, n, "%s%s", txt, suf); return buf; }

/* (U8) the last settings save's result: the foot says "Settings could not be saved" in ui::error until a later save
 * succeeds */
static bool g_save_failed;
static void save_settings(void) { g_save_failed = settings_save() == 0; }

static ImGuiContext *g_ctx;
static bool g_has_window;      /* overlay_init was given a real SDL_Window: the platform backend is live */
static SDL_Window *g_window;   /* that window (its display's content scale: render_frame's scale) */
static ImGuiStyle g_base_style; /* the look at scale 1 (ui::apply(15, 1)): every scale is applied from it, never compounded */
static char g_sys_locale[16] = "";   /* (U12b) the system locale for menu_lang "auto"; the language row re-applies it live */
static float g_ui_scale = 1.0f; /* the scale in effect (ui_scale.h) */
/* written by the window thread, read by the game thread (display.c's pause, window.c's audio): atomic,
 * like every other flag that crosses those threads (rscale_req, osd_level_v). g_paused: Game > Pause
 * in the menu bar -- the game held (display.c, as for the menu) with nothing but a banner on screen */
static std::atomic<bool> g_open, g_want_quit, g_want_capture, g_paused;
static std::atomic<bool> g_intro;                 /* the touch card is up (the game paused under it) */
static void intro_end(void); static void build_intro(void);
static std::atomic<bool> g_prep_card;             /* the shader prep card is up (in the menu's open state: the game paused, input to the card) */
static int g_prep_demo[3] = {-1, 0, 0};           /* RECOMP_OVERLAY_DUMP_UI=prep: the card with these numbers (done, total, seconds left) */
static void build_prep(void); static void build_prep_corner(float w, float hgt, int done, int total);
static std::atomic<bool> g_quit_card;             /* the quit card is (overlay_quit_confirm) */
static std::atomic<int> g_layout_ask, g_layout_answer;   /* the layout card: asked, and its answer (1 touch, 2 PS TV) */
static bool g_layout_card, g_layout_remember;

int overlay_init(SDL_GPUDevice *device, SDL_Window *window, SDL_GPUTextureFormat color_fmt) {
	if (!device) return 0;
	if (g_ctx) return 1;   /* window.c calls this once at start-up; a repeat (shouldn't happen) is harmless */
	IMGUI_CHECKVERSION();
	g_ctx = ImGui::CreateContext();
	ImGui::SetCurrentContext(g_ctx);
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;   /* never write imgui.ini into the game's own run directory */
	/* (keyboard/gamepad navigation: set per frame in render_frame -- on with the full menu only) */
	ui::load_system_font(io);   /* an explicit face: without one, ImGui 1.92's own automatic choice (vector vs. bitmap)
		depends on a DPI scale the headless path never sets, and picks a degenerate (zero-size) one */
	ui::apply(15.0f, 1.0f);     /* the launcher's look (ui_theme.h), a point smaller: the menu sits over a 960x544 game */
	/* (U12a) the menu's language: menu_lang, "auto" = the system's preference */
	char ml[8]; const char *pref = settings_str("menu_lang", "auto", ml, sizeof ml);
	int nloc = 0; SDL_Locale **locs = SDL_GetPreferredLocales(&nloc);
	const char *lc = locs && nloc > 0 && locs[0] && locs[0]->language ? locs[0]->language : NULL;   /* (U12b) kept for the language row's live switch */
	if (lc) snprintf(g_sys_locale, sizeof g_sys_locale, "%s", lc);
	ui_text_lang_load(pref, g_sys_locale[0] ? g_sys_locale : NULL);
	g_base_style = ImGui::GetStyle(); g_ui_scale = 1.0f;   /* (scaled per window size in render_frame: ui_scale.h) */
	g_has_window = window != nullptr; g_window = window;
	if (g_has_window && !ImGui_ImplSDL3_InitForSDLGPU(window)) { ImGui::DestroyContext(g_ctx); g_ctx = nullptr; return 0; }
	imedlg_set_ui_ready(g_has_window);   /* (HLE-LIB-05: a window can show the IME card) */
	/* (U12c) the pad is the host's: window.c routes its events here and savedlg_ui's feed makes them ImGui keys;
	 * the backend opens no pads of its own (its list went stale for a pad connected after this point, and its
	 * empty list cleared HasGamepad every frame -- the whole menu and the save dialog lost the controller) */
	if (g_has_window) ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
	ImGui_ImplSDLGPU3_InitInfo info = {};
	info.Device = device; info.ColorTargetFormat = color_fmt; info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
	if (!ImGui_ImplSDLGPU3_Init(&info)) {
		if (g_has_window) ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext(g_ctx); g_ctx = nullptr;
		return 0;
	}
	return 1;
}
void overlay_shutdown(void) {
	if (!g_ctx) return;
	ImGui::SetCurrentContext(g_ctx);
	ImGui_ImplSDLGPU3_Shutdown();
	if (g_has_window) ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext(g_ctx);
	g_ctx = nullptr; g_has_window = false; g_open = false; g_want_quit = false; g_want_capture = false; g_paused = false;
}

int overlay_is_open(void) { return g_ctx && g_open; }
/* every pause goes through here: display.c freezes the game's clock for exactly the time the menu is up
 * or Game > Pause is on, whichever lasts longer (overlay_game_paused is what display.c and window.c's
 * audio actually poll) */
static void apply_pause(void) { display_set_paused(g_open || g_paused); }
void overlay_open(void) { if (g_ctx) { intro_end(); g_open = true; apply_pause(); } }   /* (F10 over the touch card: it's skipped) */
static void intro_end(void) {   /* the touch card, done or skipped: never again */
	if (!g_intro) return;
	g_intro = false; settings_set_int("touch_intro_shown", 1); save_settings();
	g_paused = false; apply_pause();
}
int overlay_intro_up(void) { return g_ctx && g_intro; }   /* (INTRO-01) */
void overlay_intro_skip(void) { if (g_ctx) intro_end(); }
void overlay_close(void) {   /* closing the menu resumes: a Game > Pause doesn't outlive it */
	if (g_layout_card) { g_layout_card = false; g_layout_ask = 0; g_layout_answer = 1; }   /* (Escape on the layout card: touch, the default) */
	intro_end();
	if (g_prep_card) { g_prep_card = false; gfx_gpu_shader_prep_skip(); }   /* (the card closed before the builds were done: Skip) */
	g_quit_card = false;
	g_open = false; g_paused = false; apply_pause();
}
void overlay_toggle(void) { if (g_open) overlay_close(); else overlay_open(); }
void overlay_set_paused(int on) { if (g_ctx) { g_paused = on != 0; apply_pause(); } }
int overlay_game_paused(void) { return g_ctx && (g_open || g_paused); }
int overlay_wants_quit(void) { return g_want_quit.exchange(false); }
int overlay_quit_confirm(void) {   /* (window.c, on a request to close the window) 1: the card is up now; 0: it already was, or can't be: quit */
	if (!g_ctx || !g_has_window || g_quit_card) return 0;
	g_quit_card = true; overlay_open(); return 1;
}
/* quitting a game the launcher started (ATIV_LAUNCHER, hub.cpp) returns to the launcher's library, which comes to the
 * front: the button says so */
static const char *quit_label(void) { return std::getenv("ATIV_LAUNCHER") ? ui_tr("foot.quit_launcher") : ui_tr("foot.quit_game"); }
int overlay_wants_capture(void) { return g_want_capture.exchange(false); }

extern "C" { int window_kb_count(void); const char *window_kb_label(int i); const char *window_kb_key_name(int i);
             void window_kb_bind(int i, SDL_Keycode k); void window_kb_reset(void); int window_kbm_active(void); }
extern "C" { int window_pad_count(void); const char *window_pad_label(int i); const char *window_pad_button_name(int i);
             int window_pad_bind(int i, int button); void window_pad_reset(void); }
static int g_rebind = -1;   /* the Controls page's Rebind: the next key pressed goes to this action */
static int g_pad_rebind = -1;   /* (U11) the Controller buttons' Change: the next pad button goes to this action */
void overlay_process_event(const SDL_Event *event) {
	if (!g_ctx || !g_has_window) return;
	if (g_rebind >= 0 && event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) { window_kb_bind(g_rebind, event->key.key); g_rebind = -1; return; }   /* (Escape and F10 close the menu first: window.c) */
	if (g_pad_rebind >= 0) {   /* (U11) the pad capture: the next pad button becomes the binding and reaches nothing;
	                            * a nameless button (Guide, the shoulders) is refused and keeps the capture; Escape
	                            * or B cancels (a gamepad press would be a binding, so cancel is the keyboard's) */
		if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) { if (window_pad_bind(g_pad_rebind, event->gbutton.button)) g_pad_rebind = -1; return; }
		if (event->type == SDL_EVENT_GAMEPAD_BUTTON_UP) return;
		if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat && (event->key.key == SDLK_ESCAPE || event->key.key == SDLK_B)) { g_pad_rebind = -1; return; }
	}
	ImGui::SetCurrentContext(g_ctx);
	/* the save dialog's pad (SAVE-DLG-01): the pad events routed here become the dialog's ImGui keys --
	 * the SDL3 backend's own gamepad list goes stale when a pad connects after the overlay's first frame,
	 * so the dialog cannot rely on it for key state (savedlg_ui.cpp's header comment) */
	if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
		savedlg_ui_gamepad_button(event->gbutton.button, event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
		return;
	}
	if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) { savedlg_ui_gamepad_axis(event->gaxis.axis, event->gaxis.value); return; }
	ImGui_ImplSDL3_ProcessEvent(event);
}
/* (U6) g_prefs_epoch: a reset changes settings under the pages' feet, so the once-loaded page values re-read it.
 * g_modal_open: the reset dialog is up -- window.c must leave Esc to it (the dialog cancels, not the menu). */
static int g_prefs_epoch;
static bool g_modal_open;

/* ---- the settings both the full menu's pages and the menu bar's menus edit: read once from settings.c
 * (it has no "changed since" signal, and a slider mid-drag must not snap back to the file's value on the
 * next frame), then this copy is the live value and every edit saves at once. One setter per key, so
 * the page and the menu can't drift apart. The values with a live owner elsewhere (render scale,
 * aspect, fullscreen, the overlay level) are read from that owner every frame instead. */
static struct { bool loaded; float fov, view, scen, chr, tcs, gyro_sens, vol; int fr, gyro, gyro_flat; } P;
static void prefs_load(void) {
	if (P.loaded) return;
	char b[16];
	P.fov = (float)settings_float("fov", 1.0); P.fr = settings_int("frame_rate", 30);
	P.view = (float)settings_float("view_distance", 1.0);
	P.scen = (float)settings_float("scenery_detail", 1.0);
	P.chr = (float)settings_float("character_detail", 1.0);
	P.tcs = (float)settings_float("touch_cursor_speed", 1.0);
	P.gyro = settings_int("gyro", 1); P.gyro_flat = !strcmp(settings_str("gyro_pose", "upright", b, sizeof b), "flat");
	P.gyro_sens = (float)settings_float("gyro_sens", 1.0);
	P.vol = (float)settings_float("volume", 1.0);
	P.loaded = true;
}
static void set_fov(float v) { P.fov = v; settings_set_float("fov", v); save_settings(); }
static void set_view(float v) { P.view = v; settings_set_float("view_distance", v); save_settings(); }
static void set_scen(float v) { P.scen = v; settings_set_float("scenery_detail", v); save_settings(); }
static void set_chr(float v) { P.chr = v; settings_set_float("character_detail", v); save_settings(); }
/* the per-game options this game's hooks honour, from its conf (RECOMP_GAME_OPTIONS in RUN_ENV, e.g. "fov,view_distance"):
 * a row is shown only where it does something -- Uncharted's view distance and Killzone's scenery detail are different
 * hooks, and Shovel Knight has none */
static bool game_option(const char *name) {
	const char *e = std::getenv("RECOMP_GAME_OPTIONS"); if (!e) return false;
	const size_t n = std::strlen(name);
	for (const char *p = e; (p = std::strstr(p, name)); p += n)
		if ((p == e || p[-1] == ',' || p[-1] == ' ') && (p[n] == 0 || p[n] == ',' || p[n] == ' ')) return true;
	return false;
}
static void set_frame_rate(int fr) { P.fr = fr; settings_set_int("frame_rate", fr); save_settings(); }
static void set_tcs(float v) { P.tcs = v; settings_set_float("touch_cursor_speed", v); save_settings(); }
static void set_gyro(int on) { P.gyro = on != 0; settings_set_int("gyro", P.gyro); save_settings(); }
static void set_gyro_pose(int flat) { P.gyro_flat = flat != 0; settings_set_str("gyro_pose", flat ? "flat" : "upright"); save_settings(); }
static void set_gyro_sens(float v) { P.gyro_sens = v; settings_set_float("gyro_sens", v); save_settings(); }
static void set_volume(float v) { P.vol = v; settings_set_float("volume", v); save_settings(); }   /* window.c applies it to every output port */
static void set_osd(int level) { metrics_osd_set(level); settings_set_int("osd", level); save_settings(); }

/* ---- the host's own hotkeys, one table: the Help page's Shortcuts list and the Controls page's second
 * table both draw it (the game's controls are window.c's ctrl_rows; these are the keys window.c binds
 * beside them -- keep in sync with its event loop). The bar's own shortcut texts name the same keys. */
static const char *const g_shortcuts[][3] = {
	{"sb.bar", "sb.bar_kbd", "sb.bar_pad"},
	{"sb.menu", "sb.menu_kbd", "sb.menu_pad"},
	{"sb.osd", "F1", "-"},
	{"sb.res", "F2", "-"},
	{"sb.lamp", "F7", "-"},
	{"sb.capture", "F12", "-"},
	{"sb.mark", "F5", "-"},
	{"sb.gyro", "-", "R3"},
};
static void volume_slider(void) {   /* (the bar's Game menu) */
	prefs_load();
	float pct = P.vol * 100.0f;
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
	if (ui::slider_float("##vol", &pct, 0.0f, 100.0f, "%.0f%%")) set_volume(pct / 100.0f);
}
/* render scale labels: "3x  2880 x 1632" (the 960x544 frame's size at n), and the scale that matches the
 * window: the smallest whose frame is at least the window's pixel height (Vita frames are 16:9-ish, so
 * height decides in the fit aspect), capped at the backend's maximum */
static void scale_label(char *b, size_t n, int k) { std::snprintf(b, n, "%dx   %d x %d%s", k, 960 * k, 544 * k, k == 4 ? "  (4K)" : ""); }
static int scale_for_window(void) {
	uint32_t w, h; window_pixel_size(&w, &h);
	if (!h) return 0;
	int k = (int)((h + 543) / 544); if (k < 1) k = 1; if (k > gfx_gpu_render_scale_max()) k = gfx_gpu_render_scale_max();
	return k;
}
static const char *const g_aspect_names[3] = {"aspect.fit", "aspect.stretch", "aspect.fill"};   /* (U12b: catalog ids -- the call sites run ui_tr) */

/* ---- the full menu's pages: settings rows (ui_theme.h) -- what a setting is, a line on what it does, its
 * control on the right. Each reads its live value from its owner every frame (gfx_gpu.c, window.c,
 * metrics.c, settings.c, or P above) and calls that owner's setter the moment the control changes. */
static void slider_row(const char *id, float *v, float lo, float hi, const char *fmt, ImGuiSliderFlags fl = 0) {
	ImGui::SetNextItemWidth(-FLT_MIN);
	ui::slider_float(id, v, lo, hi, fmt, fl);
}
static void page_display(void) {
	if (!ui::rows_begin("display")) return;
	ui::row(ui_tr("display.resolution"), ui_tr("display.resolution_help"), note_int("render_scale", gfx_gpu_render_scale_wanted(), ui_tr("mark.next_scene")));
	{	int rs = gfx_gpu_render_scale_wanted(), mx = gfx_gpu_render_scale_max(), v = rs - 1;   /* (a pick shows at once: it applies at the game's next scene) */
		static char names[8][16]; const char *labels[8];
		for (int n = 1; n <= mx && n <= 8; n++) { std::snprintf(names[n - 1], sizeof names[0], "%d×##rs", n); labels[n - 1] = names[n - 1]; }
		if (ui::segmented(labels, mx < 8 ? mx : 8, &v)) gfx_gpu_render_scale_request(v + 1);
		char b[48]; scale_label(b, sizeof b, rs);
		ImGui::TextDisabled("%s", b + 5);
		int mw = scale_for_window();
		if (mw && mw != rs) { ImGui::SameLine(); char l[64]; std::snprintf(l, sizeof l, ui_tr("display.match_window"), mw); if (ImGui::TextLink(l)) gfx_gpu_render_scale_request(mw); }
	}
	ui::row(ui_tr("display.fit"), ui_tr("display.fit_help"), note_s("aspect"));
	{	int am = window_get_aspect();
		const char *names[3] = {ui_tr("aspect.fit"), ui_tr("aspect.stretch"), ui_tr("aspect.fill")};
		if (ui::segmented(names, 3, &am)) window_set_aspect(am);
	}
	ui::row(ui_tr("display.widescreen"), ui_tr(window_aspect_supported() ? "display.widescreen_help" : "display.widescreen_none"), note_s("aspect_ratio"));
	{	static const char *const ids[6] = {"native", "16:10", "3:2", "21:9", "32:9", "window"};
		char cur[16]; settings_str("aspect_ratio", "native", cur, sizeof cur);
		int v = 0; for (int i = 0; i < 6; i++) if (!strcmp(cur, ids[i])) v = i;
		char t0[48], t1[48], t2[48], t3[48], t4[48], t5[48];
		const char *names[6] = {tr_suf(ui_tr("common.off"), "##ar", t0, sizeof t0), tr_suf("16:10", "##ar", t1, sizeof t1),
		                        tr_suf("3:2", "##ar", t2, sizeof t2), tr_suf("21:9", "##ar", t3, sizeof t3),
		                        tr_suf("32:9", "##ar", t4, sizeof t4), tr_suf(ui_tr("display.ar_window"), "##ar", t5, sizeof t5)};
		ImGui::BeginDisabled(!window_aspect_supported());
		if (ui::segmented(names, 6, &v)) { settings_set_str("aspect_ratio", ids[v]); save_settings(); }
		ImGui::EndDisabled();
	}
	if (gfx_gpu_hud_supported()) {
		ui::row(ui_tr("display.hud"), ui_tr("display.hud_help"), note_s("hud_layout"));
		static const char *const ids[3] = {"edges", "16:9", "stretch"};
		const char *hud_def = getenv("RECOMP_HUD_LAYOUT");   /* the game's default (RUN_ENV), as gfx_gpu.c hud_note_present */
		char cur[16]; settings_str("hud_layout", hud_def && *hud_def ? hud_def : "edges", cur, sizeof cur);
		int v = 0; for (int i = 0; i < 3; i++) if (!strcmp(cur, ids[i])) v = i;
		if (!strcmp(cur, "frame")) v = 1;   /* the pack default "frame" (all centred) shown as its nearest choice, 16:9 */
		char t0[48], t1[48], t2[48];
		const char *names[3] = {tr_suf(ui_tr("display.hud_edges"), "##hud", t0, sizeof t0), tr_suf("16:9", "##hud", t1, sizeof t1),
		                        tr_suf(ui_tr("display.hud_stretched"), "##hud", t2, sizeof t2)};
		ImGui::BeginDisabled(!window_aspect_supported() || window_aspect_widen() < 1.01f);
		if (ui::segmented(names, 3, &v)) { settings_set_str("hud_layout", ids[v]); save_settings(); }
		ImGui::EndDisabled();
	}
	if (gfx_gpu_target_scale_supported()) {
		ui::row(ui_tr("display.full_res"), ui_tr("display.full_res_help"), note_int("full_res_world", settings_int("full_res_world", 1)));
		{ bool on = settings_int("full_res_world", 1) != 0; if (ui::toggle("frw", &on)) { settings_set_int("full_res_world", on ? 1 : 0); save_settings(); } }
	}
	if (gfx_gpu_ssao_supported()) {
		ui::row(ui_tr("display.ssao"), ui_tr("display.ssao_help"), note_int("ssao", settings_int("ssao", 1)));
		{ bool on = settings_int("ssao", 1) != 0; if (ui::toggle("ssao", &on)) { settings_set_int("ssao", on ? 1 : 0); save_settings(); } }
		ui::row(ui_tr("display.ssao_strength"), nullptr, note_flt("ssao_strength", settings_float("ssao_strength", 1.0)));
		{	float st = (float)settings_float("ssao_strength", 1.0) * 100.0f;
			ImGui::BeginDisabled(settings_int("ssao", 1) == 0);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ui::slider_float("##ssaost", &st, 0.0f, 200.0f, "%.0f%%")) { settings_set_float("ssao_strength", st / 100.0f); save_settings(); }
			ImGui::EndDisabled();
		}
	}
	ui::row(ui_tr("display.fullscreen"), nullptr, note_int("fullscreen", window_get_fullscreen()));
	{ bool fs = window_get_fullscreen() != 0; if (ui::toggle("fs", &fs)) window_set_fullscreen(fs ? 1 : 0); }
	ui::row(ui_tr("display.presentation"), ui_tr("display.presentation_help"), ui_tr("mark.after_restart"));
	{	static const char *const ids[3] = {"vsync", "mailbox", "immediate"};
		char cur[16]; settings_str("present_mode", "", cur, sizeof cur);
		int v = !strcmp(cur, "mailbox") ? 1 : !strcmp(cur, "immediate") ? 2 : 0;
		char t0[48], t1[48], t2[48];
		const char *names[3] = {tr_suf("Vsync", "##pm", t0, sizeof t0), tr_suf("Mailbox", "##pm", t1, sizeof t1),
		                        tr_suf(ui_tr("display.pm_immediate"), "##pm", t2, sizeof t2)};
		if (ui::segmented(names, 3, &v)) { settings_set_str("present_mode", ids[v]); save_settings(); }
	}
	if (!window_get_fullscreen()) {
		ui::row(ui_tr("display.window_size"), ui_tr("display.window_size_help"));
		for (int n = 1; n <= 4; n++) {
			char l[10]; std::snprintf(l, sizeof l, "%d×##ws", n);
			if (n > 1) ImGui::SameLine();
			if (ImGui::Button(l)) window_set_size(n);
		}
	}
	if (gfx_gpu_fsr_supported()) {   /* (this game's FSR is verified: the anti-aliasing is a choice) */
		ui::row(ui_tr("display.aa"), ui_tr("display.aa_fsr_help"),
			note_str("post_aa", (settings_int("fsr", 0) || !settings_int("post_aa", 1)) ? "0" : "1"));
		int v = settings_int("fsr", 0) ? 2 : settings_int("post_aa", 1) ? 1 : 0;
		char t0[48], t1[48], t2[64];
		const char *names[3] = {tr_suf(ui_tr("common.off"), "##aam", t0, sizeof t0), tr_suf("FXAA", "##aam", t1, sizeof t1),
		                        tr_suf(ui_tr("display.aa_fsr"), "##aam", t2, sizeof t2)};
		if (ui::segmented(names, 3, &v)) { settings_set_int("fsr", v == 2); settings_set_int("post_aa", v == 1); save_settings(); }
	} else {
		ui::row(ui_tr("display.aa"), ui_tr("display.aa_help"), note_int("post_aa", settings_int("post_aa", 1)));
		{ bool aa = settings_int("post_aa", 1) != 0; if (ui::toggle("aa", &aa)) { settings_set_int("post_aa", aa ? 1 : 0); save_settings(); } }
	}
	ui::row(ui_tr("display.sharpening"), ui_tr("display.sharpening_help"), note_flt("sharpen", settings_float("sharpen", 0.35)));
	{	float sh = (float)settings_float("sharpen", 0.35) * 100.0f;
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ui::slider_float("##sharp", &sh, 0.0f, 100.0f, sh <= 0.0f ? ui_tr("common.off") : "%.0f%%")) { settings_set_float("sharpen", sh / 100.0f); save_settings(); }
	}
	ui::row(ui_tr("display.aniso"), ui_tr("display.aniso_help"), note_int("anisotropy", settings_int("anisotropy", 16)));
	{	static const int an[5] = {1, 2, 4, 8, 16};
		char t0[48], t1[48], t2[48], t3[48], t4[48];
		const char *names[5] = {tr_suf(ui_tr("common.off"), "##af", t0, sizeof t0), tr_suf("2×", "##af", t1, sizeof t1),
		                        tr_suf("4×", "##af", t2, sizeof t2), tr_suf("8×", "##af", t3, sizeof t3), tr_suf("16×", "##af", t4, sizeof t4)};
		int cur = settings_int("anisotropy", 16), v = 4; for (int i = 0; i < 5; i++) if (an[i] == cur) v = i;
		if (ui::segmented(names, 5, &v)) { settings_set_int("anisotropy", an[v]); save_settings(); }
	}
	ui::row(ui_tr("display.shadow"), ui_tr("display.shadow_help"), note_int("shadow_filter", settings_int("shadow_filter", 1)));
	{	char t0[48], t1[48], t2[48];
		const char *names[3] = {tr_suf(ui_tr("display.sf_original"), "##sf", t0, sizeof t0), tr_suf(ui_tr("display.sf_soft"), "##sf", t1, sizeof t1),
		                        tr_suf(ui_tr("display.sf_softer"), "##sf", t2, sizeof t2)};
		int v = settings_int("shadow_filter", 1);
		if (ui::segmented(names, 3, &v)) { settings_set_int("shadow_filter", v); save_settings(); }
	}
	ui::rows_end();
}
static void page_performance(void) {
	if (!ui::rows_begin("perf")) return;
	ui::row(ui_tr("perf.osd"), ui_tr("perf.osd_help"), note_int("osd", metrics_osd_level()));
	{	char t0[48], t1[48], t2[48];
		const char *names[3] = {tr_suf(ui_tr("common.off"), "##osd", t0, sizeof t0), tr_suf("FPS", "##osd", t1, sizeof t1),
		                        tr_suf(ui_tr("perf.osd_full"), "##osd", t2, sizeof t2)};
		int lvl = metrics_osd_level(); if (ui::segmented(names, 3, &lvl)) set_osd(lvl); }
	ui::row(ui_tr("perf.fps_now"));
	{ char b[32]; std::snprintf(b, sizeof b, "%.0f fps", metrics_fps()); ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.6f); ImGui::TextUnformatted(b); ImGui::PopFont(); }
	ui::rows_end();
}
static void page_game(void) {
	prefs_load();
	if (!ui::rows_begin("game")) return;
	ui::row(ui_tr("game.fps"), ui_tr("game.fps_help"), note_flt("frame_rate", P.fr));
	{	/* only the rates this game's pack allows (RECOMP_FRAME_RATES; frame_rates.h), the one it runs at selected */
		static const char *const all[3] = {"30##fr", "60##fr", "120##fr"};
		int l[3], n = fr_list(display_frame_rates_allowed(), l), cur = fr_effective(P.fr, display_frame_rates_allowed()), v = 0;
		const char *names[3]; for (int i = 0; i < n; i++) { names[i] = all[l[i] >= 120 ? 2 : l[i] >= 60 ? 1 : 0]; if (l[i] == cur) v = i; }
		if (ui::segmented(names, n, &v)) set_frame_rate(l[v]); }
	if (game_option("fov")) {
		ui::row(ui_tr("game.fov"), ui_tr("game.fov_help"));
		ImGui::SetNextItemWidth(-FLT_MIN); if (ui::slider_float("##fov", &P.fov, 0.75f, 1.5f, "%.2f×")) set_fov(P.fov);
	}
	if (game_option("view_distance")) {
		ui::row(ui_tr("game.view"), ui_tr("game.view_help"));
		ImGui::SetNextItemWidth(-FLT_MIN); if (ui::slider_float("##view", &P.view, 1.0f, 3.0f, "%.2f×")) set_view(P.view);
	}
	if (game_option("scenery_detail")) {
		ui::row(ui_tr("game.scen"), ui_tr("game.scen_help"));
		ImGui::SetNextItemWidth(-FLT_MIN); if (ui::slider_float("##scen", &P.scen, 1.0f, 8.0f, "%.1f×")) set_scen(P.scen);
	}
	if (game_option("character_detail")) {
		ui::row(ui_tr("game.chr"), ui_tr("game.chr_help"));
		ImGui::SetNextItemWidth(-FLT_MIN); if (ui::slider_float("##chr", &P.chr, 1.0f, 8.0f, "%.1f×")) set_chr(P.chr);
	}
	if (display_pstv_looked()) {   /* (display.c's PS TV mode: a game with a PS TV layout of its own) */
		ui::row(ui_tr("game.layout"), ui_tr("game.layout_help"), note_int("controller_layout", settings_int("controller_layout", 0), ui_tr("mark.after_restart")));
		{	char t0[48], t1[48], t2[48];
			const char *names[3] = {tr_suf(ui_tr("game.layout_ask"), "##cl", t0, sizeof t0), tr_suf(ui_tr("common.std"), "##cl", t1, sizeof t1),
			                        tr_suf("PS TV", "##cl", t2, sizeof t2)};
			int v = settings_int("controller_layout", 0); if (v < 0 || v > 2) v = 0;   /* (the setting's values: 0 ask, 1 standard (touch), 2 PS TV) */
			if (ui::segmented(names, 3, &v)) { settings_set_int("controller_layout", v); save_settings(); } }
	}
	if (game_option("online")) {
		ui::row(ui_tr("game.online"), ui_tr("game.online_help"), note_int("online", settings_int("online", 1), ui_tr("mark.after_restart")));
		if (net_online_needs()) { char t[160]; snprintf(t, sizeof t, ui_tr("game.needs_update"), net_online_needs()); ImGui::TextWrapped("%s", t); }
		else { bool on = settings_int("online", 1) != 0; if (ui::toggle("online", &on)) { settings_set_int("online", on ? 1 : 0); save_settings(); } }
		if (net_online()) {   /* how it stands (status.c), every half second */
			static OnlineStatus st; static double at = -1; const double now = ImGui::GetTime();
			if (at < 0 || now - at > 0.5) { online_status(&st); at = now; }
			char a[160], b[160], c2[160];
			if (!st.started) { snprintf(a, sizeof a, "%s", ui_tr("game.status_starting")); b[0] = c2[0] = 0; }
			else {
				if (st.linked) snprintf(a, sizeof a, ui_tr(st.linked == 1 ? "game.status_linked_1" : "game.status_linked_n"), st.linked);
				else snprintf(a, sizeof a, "%s", ui_tr("game.status_looking"));
				{	const int n = st.rooms_heard + st.rooms_mine;
					snprintf(b, sizeof b, ui_tr(n == 1 ? "game.rooms_1" : "game.rooms_n"), n, st.rooms_mine ? ui_tr("game.rooms_yours") : ""); }
				if (!st.port) snprintf(c2, sizeof c2, "%s", ui_tr("game.status_port_none"));
				else if (st.pub[0]) snprintf(c2, sizeof c2, ui_tr("game.status_internet"), st.pub, st.dht_nodes);
				else snprintf(c2, sizeof c2, st.dht_nodes ? ui_tr("game.status_reaching") : ui_tr("game.status_reaching0"), st.dht_nodes);
			}
			ui::row(ui_tr("game.online_now"), st.clash ? ui_tr("game.clash") : nullptr);
			ImGui::TextUnformatted(a); if (b[0]) { ImGui::PushStyleColor(ImGuiCol_Text, ui::muted); ImGui::TextUnformatted(b); ImGui::TextUnformatted(c2); ImGui::PopStyleColor(); }
		}
		static char oid[32], srv[128]; static int loaded, epoch;
		if (!loaded || epoch != g_prefs_epoch) { loaded = 1; epoch = g_prefs_epoch; settings_str("online_id", "", oid, sizeof oid); for (const char *tag : {"ntv_", "nativ_", "ativ_"}) if (!strncasecmp(oid, tag, strlen(tag))) { memmove(oid, oid + strlen(tag), strlen(oid) - strlen(tag) + 1); break; } oid[10] = 0; settings_str("private_server", "", srv, sizeof srv); }
		ui::row(ui_tr("game.online_name"), ui_tr("game.online_name_help"), note_s("online_id", ui_tr("mark.after_restart")));
		{	ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("ntv_"); ImGui::SameLine(0, 0);
			ImGui::SetNextItemWidth(-FLT_MIN); ImGui::InputText("##oid", oid, 11);
			int ok = 1, n = (int)strlen(oid);
			for (int i = 0; i < n; i++) if (!isalnum((unsigned char)oid[i]) && oid[i] != '-' && oid[i] != '_') ok = 0;
			if (ImGui::IsItemDeactivatedAfterEdit() && ok) { settings_set_str("online_id", oid); save_settings(); }
			if (!ok) { ImGui::PushStyleColor(ImGuiCol_Text, ui::muted); ImGui::TextUnformatted(ui_tr("game.name_invalid")); ImGui::PopStyleColor(); }
		}
		if (game_option("private_server")) {   /* (a game whose online needs its makers' servers) */
			ui::row(ui_tr("game.private_server"), ui_tr("game.private_server_help"));
			ImGui::SetNextItemWidth(-FLT_MIN); ImGui::InputText("##srv", srv, sizeof srv); if (ImGui::IsItemDeactivatedAfterEdit()) { settings_set_str("private_server", srv); save_settings(); }
		}
	}
	ui::row(ui_tr("game.volume"), nullptr, note_flt("volume", P.vol));
	{ float pct = P.vol * 100.0f; ImGui::SetNextItemWidth(-FLT_MIN); if (ui::slider_float("##vol", &pct, 0.0f, 100.0f, "%.0f%%")) set_volume(pct / 100.0f); }
	ui::rows_end();
}
extern "C" int camera_light(void); extern "C" void camera_light_set(int on);
static void section_label(const char *s) { ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.4f)); ImGui::PushStyleColor(ImGuiCol_Text, ui::accent); ImGui::TextUnformatted(s); ImGui::PopStyleColor(); }
static void reference_table(const char *id, int (*count)(void), void (*row)(int, const char **, const char **, const char **)) {
	if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
	ImGui::TableSetupColumn(ui_tr("ctrl.action")); ImGui::TableSetupColumn(ui_tr("ctrl.kbm")); ImGui::TableSetupColumn(ui_tr("common.controller"));
	ImGui::PushStyleColor(ImGuiCol_Text, ui::muted); ImGui::TableHeadersRow(); ImGui::PopStyleColor();
	for (int i = 0, n = count(); i < n; i++) {
		const char *a, *k, *p; row(i, &a, &k, &p);
		ImGui::TableNextRow();
		ImGui::TableNextColumn(); ImGui::TextWrapped("%s", a);
		ImGui::TableNextColumn(); ImGui::TextWrapped("%s", k);
		ImGui::TableNextColumn(); ImGui::TextWrapped("%s", p);
	}
	ImGui::EndTable();
}
static int shortcuts_count(void) { return (int)(sizeof g_shortcuts / sizeof g_shortcuts[0]); }
static void shortcuts_row(int i, const char **a, const char **k, const char **p) { *a = ui_tr(g_shortcuts[i][0]); *k = ui_tr(g_shortcuts[i][1]); *p = ui_tr(g_shortcuts[i][2]); }   /* (U12b) key names fall through ui_tr */
static void page_controls(void) {
	prefs_load();
	section_label(ui_tr("ctrl.input"));
	if (ui::rows_begin("input")) {
		ui::row(ui_tr("ctrl.play_with"), window_kbm_active() ? ui_tr("ctrl.play_with_kbm") : ui_tr("ctrl.play_with_pad"), note_s("input_mode"));
		{	static const char *const ids[3] = {"auto", "pad", "kbm"}; char t0[48], t1[48], t2[48];
			const char *names[3] = {tr_suf(ui_tr("ctrl.in_auto"), "##in", t0, sizeof t0), tr_suf(ui_tr("common.controller"), "##in", t1, sizeof t1),
			                        tr_suf(ui_tr("ctrl.in_kbm"), "##in", t2, sizeof t2)};
			char cur[8]; settings_str("input_mode", "auto", cur, sizeof cur);
			int v = 0; for (int i = 0; i < 3; i++) if (!strcmp(cur, ids[i])) v = i;
			if (ui::segmented(names, 3, &v)) { settings_set_str("input_mode", ids[v]); save_settings(); }
		}
		ui::row(ui_tr("ctrl.pad_device"), ui_tr("ctrl.pad_device_help"), note_s("pad_device"));   /* (UI-02b: the game switches as the menu closes) */
		device_choice("pd", "pad_device", pad_list());
		ui::row(ui_tr("ctrl.mouse_look"), ui_tr("ctrl.mouse_look_help"), note_int("mouse_look", settings_int("mouse_look", 1)));
		{ bool look = settings_int("mouse_look", 1) != 0; if (ui::toggle("ml", &look)) { settings_set_int("mouse_look", look ? 1 : 0); save_settings(); } }
		ui::row(ui_tr("ctrl.mouse_aim"), ui_tr("ctrl.mouse_aim_help"), note_int("raw_look", window_raw_look()));
		{ char t0[48], t1[48];
			const char *names[2] = {tr_suf(ui_tr("ctrl.aim_direct"), "##ma", t0, sizeof t0), tr_suf(ui_tr("ctrl.aim_stick"), "##ma", t1, sizeof t1)};
			int v = window_raw_look() ? 0 : 1; if (ui::segmented(names, 2, &v)) window_set_raw_look(v == 0); }
		ui::row(ui_tr("ctrl.mouse_sens"), nullptr, note_flt("mouse_sens", settings_float("mouse_sens", 1.0)));
		{ static float sens = -1; static int epoch; if (sens < 0 || epoch != g_prefs_epoch) { sens = (float)settings_float("mouse_sens", 1.0); epoch = g_prefs_epoch; } ImGui::SetNextItemWidth(-FLT_MIN);
		  if (ui::slider_float("##ms", &sens, 0.1f, 5.0f, "%.2f×", ImGuiSliderFlags_Logarithmic)) { settings_set_float("mouse_sens", sens); save_settings(); } }
		ui::row(ui_tr("ctrl.mouse_invert"), nullptr, note_int("mouse_invert", settings_int("mouse_invert", 0)));
		{ bool inv = settings_int("mouse_invert", 0) != 0; if (ui::toggle("mi", &inv)) { settings_set_int("mouse_invert", inv ? 1 : 0); save_settings(); } }
		ui::row(ui_tr("ctrl.shoulder"), ui_tr("ctrl.shoulder_help"), note_s("shoulder_buttons"));
		{ char t0[48], t1[48];
			const char *names[2] = {tr_suf(ui_tr("ctrl.shoulder_trig"), "##sb", t0, sizeof t0), tr_suf(ui_tr("ctrl.shoulder_bump"), "##sb", t1, sizeof t1)};
			char cur[16]; int v = !strcmp(settings_str("shoulder_buttons", "triggers", cur, sizeof cur), "bumpers");
			if (ui::segmented(names, 2, &v)) { settings_set_str("shoulder_buttons", v ? "bumpers" : "triggers"); save_settings(); } }
		ui::row(ui_tr("ctrl.touch_speed"), ui_tr("ctrl.touch_speed_help"), note_flt("touch_cursor_speed", settings_float("touch_cursor_speed", 1.0)));
		ImGui::SetNextItemWidth(-FLT_MIN); if (ui::slider_float("##tcs", &P.tcs, 0.25f, 3.0f, "%.2f×")) set_tcs(P.tcs);
		ui::row(ui_tr("ctrl.osb"), ui_tr("ctrl.osb_help"), note_s("osb"));   /* (UI-01n) */
		{ char t0[48], t1[48], t2[48];
			const char *names[3] = {tr_suf(ui_tr("ctrl.osb_auto"), "##osb", t0, sizeof t0), tr_suf(ui_tr("ctrl.osb_on"), "##osb", t1, sizeof t1), tr_suf(ui_tr("ctrl.osb_off"), "##osb", t2, sizeof t2)};
			char cur[8]; settings_str("osb", "auto", cur, sizeof cur); int v = !strcmp(cur, "on") ? 1 : !strcmp(cur, "off") ? 2 : 0;
			if (ui::segmented(names, 3, &v)) { settings_set_str("osb", v == 1 ? "on" : v == 2 ? "off" : "auto"); save_settings(); } }
		ui::row(ui_tr("ctrl.osb_size"), nullptr, note_flt("osb_size", settings_float("osb_size", 1.0)));
		{ static float sz = -1; static int epoch; if (sz < 0 || epoch != g_prefs_epoch) { sz = (float)settings_float("osb_size", 1.0); epoch = g_prefs_epoch; } ImGui::SetNextItemWidth(-FLT_MIN);
		  if (ui::slider_float("##osbs", &sz, 0.75f, 1.5f, "%.2f×")) { settings_set_float("osb_size", sz); save_settings(); } }
		ui::rows_end();
	}
	section_label(ui_tr("ctrl.sticks"));   /* (UI-04b: each stick's sensitivity and dead zone -- window.c reads them as the menu closes) */
	if (ui::rows_begin("sticks")) {
		static const char *const keys[4] = {"stick_l_sens", "stick_l_dead", "stick_r_sens", "stick_r_dead"};
		static const char *const names[4] = {"ctrl.stick_l_sens", "ctrl.stick_l_dead", "ctrl.stick_r_sens", "ctrl.stick_r_dead"};
		static float val[4]; static int epoch = -1;
		if (epoch != g_prefs_epoch) { for (int i = 0; i < 4; i++) val[i] = (float)settings_float(keys[i], i & 1 ? 0.0 : 1.0) * (i & 1 ? 100.0f : 1.0f); epoch = g_prefs_epoch; }
		for (int i = 0; i < 4; i++) {
			const bool dead = i & 1;
			ui::row(ui_tr(names[i]), ui_tr(dead ? "ctrl.stick_dead_help" : "ctrl.stick_sens_help"), note_flt(keys[i], settings_float(keys[i], dead ? 0.0 : 1.0)));
			char id[16]; std::snprintf(id, sizeof id, "##stk%d", i);
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (dead ? ui::slider_float(id, &val[i], 0.0f, 30.0f, "%.0f%%") : ui::slider_float(id, &val[i], 0.5f, 2.0f, "%.2f×")) {
				settings_set_float(keys[i], dead ? val[i] / 100.0f : val[i]); save_settings();
			}
		}
		ui::rows_end();
	}
	section_label(ui_tr("ctrl.motion"));
	if (ui::rows_begin("motion")) {
		ui::row(ui_tr("ctrl.gyro"), window_gyro_present() ? ui_tr("ctrl.gyro_present") : ui_tr("ctrl.gyro_absent"), note_int("gyro", P.gyro));
		{ bool on = P.gyro != 0; if (ui::toggle("gy", &on)) set_gyro(on); }
		ui::row(ui_tr("ctrl.gyro_pose"), ui_tr("ctrl.gyro_pose_help"), note_s("gyro_pose"));
		{ char t0[48], t1[48];
			const char *names[2] = {tr_suf(ui_tr("ctrl.gyro_upright"), "##gp", t0, sizeof t0), tr_suf(ui_tr("ctrl.gyro_flat"), "##gp", t1, sizeof t1)};
			int v = P.gyro_flat ? 1 : 0; if (ui::segmented(names, 2, &v)) set_gyro_pose(v); }
		ui::row(ui_tr("ctrl.gyro_sens"), nullptr, note_flt("gyro_sens", settings_float("gyro_sens", 1.0)));
		ImGui::SetNextItemWidth(-FLT_MIN); if (ui::slider_float("##gs", &P.gyro_sens, 0.25f, 3.0f, "%.2f×")) set_gyro_sens(P.gyro_sens);
		ui::row(ui_tr("ctrl.recentre"), ui_tr("ctrl.recentre_help"));
		if (ImGui::Button(ui_tr("ctrl.recentre_btn"))) motion_recenter();
		ui::row(ui_tr("ctrl.camera"), ui_tr("ctrl.camera_help"), note_s("camera"));
		{	static const char *const ids[3] = {"light", "webcam", "dark"}; char t0[48], t1[48], t2[48];
			const char *names[3] = {tr_suf(ui_tr("ctrl.cam_lamp"), "##cam", t0, sizeof t0), tr_suf(ui_tr("ctrl.cam_webcam"), "##cam", t1, sizeof t1),
			                        tr_suf(ui_tr("ctrl.cam_dark"), "##cam", t2, sizeof t2)};
			char cur[16]; settings_str("camera", "light", cur, sizeof cur);
			int v = 0; for (int i = 0; i < 3; i++) if (!strcmp(cur, ids[i])) v = i;
			if (ui::segmented(names, 3, &v)) { settings_set_str("camera", ids[v]); save_settings(); }
			if (v == 0) { bool lit = camera_light() != 0; ImGui::SameLine(); ImGui::TextDisabled("%s", ui_tr("ctrl.cam_lamp_lbl")); ImGui::SameLine(); if (ui::toggle("lamp", &lit)) camera_light_set(lit ? 1 : 0); }
			if (v == 1) { ImGui::TextDisabled("%s", ui_tr("ctrl.cam_device")); device_choice("cd", "camera_device", camera_list()); }   /* (UI-02b) */
		}
		ui::rows_end();
	}
	section_label(ui_tr("ctrl.keys"));
	if (ImGui::BeginTable("kbbinds", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn(ui_tr("ctrl.action")); ImGui::TableSetupColumn(ui_tr("ctrl.key")); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
		ImGui::PushStyleColor(ImGuiCol_Text, ui::muted); ImGui::TableHeadersRow(); ImGui::PopStyleColor();
		for (int i = 0, n = window_kb_count(); i < n; i++) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(window_kb_label(i));
			ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
			const char *k = window_kb_key_name(i);
			if (g_rebind == i) ImGui::TextColored(ui::accent, "%s", ui_tr("ctrl.press_key")); else ui::chip(k && *k ? k : ui_tr("ctrl.none"), ui::text);
			ImGui::TableNextColumn(); ImGui::PushID(i); if (ImGui::Button(ui_tr("ctrl.change"))) g_rebind = i; ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (ImGui::Button(ui_tr("ctrl.reset_keys"))) { window_kb_reset(); g_rebind = -1; }
	section_label(ui_tr("ctrl.pad"));   /* (U11) the pad -> Vita bindings, a Change/Reset like the keys */
	if (ImGui::BeginTable("padbinds", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn(ui_tr("ctrl.action")); ImGui::TableSetupColumn(ui_tr("ctrl.button")); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
		ImGui::PushStyleColor(ImGuiCol_Text, ui::muted); ImGui::TableHeadersRow(); ImGui::PopStyleColor();
		for (int i = 0, n = window_pad_count(); i < n; i++) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(window_pad_label(i));
			ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
			const char *k = window_pad_button_name(i);
			if (g_pad_rebind == i) ImGui::TextColored(ui::accent, "%s", ui_tr("ctrl.press_pad")); else ui::chip(k && *k ? k : ui_tr("ctrl.none"), ui::text);
			ImGui::TableNextColumn(); ImGui::PushID(1000 + i); if (ImGui::Button(ui_tr("ctrl.change"))) g_pad_rebind = i; ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (ImGui::Button(ui_tr("ctrl.reset_pad"))) { window_pad_reset(); g_pad_rebind = -1; }
	section_label(ui_tr("ctrl.game_controls"));
	reference_table("controls", window_controls_count, window_controls_row);
	section_label(ui_tr("ctrl.shortcuts"));
	reference_table("shortcuts", shortcuts_count, shortcuts_row);
}
/* the version line the menu's header and About draw: "Nativ <tools version> · <title id> <game version>" with every
 * empty part skipped (missing env -> nothing, never "unknown") */
static void version_line(char *out, size_t n) {
	const char *tv = std::getenv("RECOMP_TOOLS_VERSION"), *tid = std::getenv("RECOMP_TITLE"), *gv = std::getenv("RECOMP_GAME_VERSION");
	char right[96] = "";
	if (tid && *tid) snprintf(right, sizeof right, "%s", tid);
	if (gv && *gv) snprintf(right + strlen(right), sizeof right - strlen(right), "%s%s", right[0] ? " " : "", gv);
	out[0] = 0;
	if (tv && *tv) snprintf(out, n, "Nativ %s", tv);
	if (right[0]) snprintf(out + strlen(out), n - strlen(out), "%s%s", out[0] ? " · " : "", right);
}
static void page_help(void) {
	if (ui::rows_begin("lang")) {   /* (U12b) the language row: menu_lang, applied live (flag icons would need glyphs the font range excludes) */
		ui::row(ui_tr("set.lang"), nullptr, note_s("menu_lang", ui_tr("mark.default")));
		{	char t0[48], cur[8]; static const char *const ids[3] = {"auto", "en", "de"};
			const char *names[3] = {tr_suf(ui_tr("set.lang_auto"), "##lang", t0, sizeof t0), "English##lang", "Deutsch##lang"};
			settings_str("menu_lang", "auto", cur, sizeof cur);
			int v = 0; for (int i = 0; i < 3; i++) if (!strcmp(cur, ids[i])) v = i;
			if (ui::segmented(names, 3, &v)) { settings_set_str("menu_lang", ids[v]); save_settings(); ui_text_lang_load(ids[v], g_sys_locale[0] ? g_sys_locale : NULL); }
		}
		ui::rows_end();
	}
	section_label(ui_tr("ctrl.shortcuts"));
	reference_table("shortcuts", shortcuts_count, shortcuts_row);
	section_label(ui_tr("help.about"));
	{	ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.2f));
		ImVec2 p = ImGui::GetCursorScreenPos(); const float h = ImGui::GetFontSize() * 1.5f;
		ImGui::Dummy(ImVec2(ui::wordmark(ImGui::GetWindowDrawList(), p, h), h));
	}
	ImGui::TextWrapped("%s", ui_tr("help.about.body"));
	const char *title = std::getenv("RECOMP_TITLE");
	int v = SDL_GetVersion();
	ImGui::TextDisabled("%s  ·  Dear ImGui %s  ·  SDL %d.%d.%d", title && *title ? title : ui_tr("help.unknown_title"), IMGUI_VERSION, SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
	{	char line[192]; version_line(line, sizeof line); if (*line) ImGui::TextDisabled("%s", line); }   /* (U4) */
	ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
	ui::muted_wrapped(ui_tr("help.bar_hint"));
	{	static double copied_until;   /* "Copied" shows for 2 s next to the button (U5) */
		if (ImGui::Button(ui_tr("help.copy_sysinfo"))) {
			MetricsSnapshot m; metrics_snapshot(&m);
			char ver[192], sdl[32], cwd[1024];
			version_line(ver, sizeof ver);
			int sv = SDL_GetVersion(); snprintf(sdl, sizeof sdl, "%d.%d.%d", SDL_VERSIONNUM_MAJOR(sv), SDL_VERSIONNUM_MINOR(sv), SDL_VERSIONNUM_MICRO(sv));
			uint32_t w = 0, h = 0; window_pixel_size(&w, &h);
			SysInfo si; memset(&si, 0, sizeof si);
			si.versions = ver; si.platform = SDL_GetPlatform(); si.sdl_version = sdl;
			si.cpu = m.cpu_name; si.gpu = m.gpu_name;
			si.win_w = (int)w; si.win_h = (int)h;
			si.render_scale = settings_int("render_scale", 1);
			si.present_mode = window_present_mode();
			si.frame_rate = settings_int("frame_rate", 30);
			cwd[0] = 0; if (char *d = SDL_GetCurrentDirectory()) { snprintf(cwd, sizeof cwd, "%s", d); SDL_free(d); }
			si.log_dir = cwd;
			si.home = getenv("USERPROFILE"); if (!si.home || !*si.home) si.home = getenv("HOME");   /* cmd_report's ~ rule */
			char text[2048];
			sysinfo_text(text, sizeof text, &si);
			SDL_SetClipboardText(text);
			copied_until = ImGui::GetTime() + 2.0;
		}
		if (copied_until > ImGui::GetTime()) { ImGui::SameLine(); ImGui::TextDisabled("%s", ui_tr("help.copied")); }
		ImGui::SameLine();
		if (ImGui::Button(ui_tr("help.open_log"))) {   /* hub.cpp's open_folder pattern */
			char d[1024], url[1200];
			d[0] = 0; if (char *c = SDL_GetCurrentDirectory()) { snprintf(d, sizeof d, "%s", c); SDL_free(c); }
			for (char *p = d; *p; p++) if (*p == '\\') *p = '/';
#ifdef _WIN32
			snprintf(url, sizeof url, "file:///%s", d);
#else
			snprintf(url, sizeof url, "file://%s", d);
#endif
			SDL_OpenURL(url);
		}
	}
	ui::muted_wrapped(ui_tr("help.bug_hint"));
	if (ImGui::Button(ui_tr("help.reset_all"))) ImGui::OpenPopup(ui_tr("help.reset_all"));   /* (U6: confirmed below) */
	if ((g_modal_open = ImGui::BeginPopupModal(ui_tr("help.reset_all"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))) {
		ImGui::TextWrapped("%s", ui_tr("help.reset_all_help"));
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();   /* Cancel is the focused button first */
		if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) ImGui::CloseCurrentPopup();   /* B/Esc = Cancel (window.c leaves Esc to a modal) */
		if (ImGui::Button(ui_tr("common.cancel"))) ImGui::CloseCurrentPopup();
		ImGui::SameLine();
		if (ImGui::Button(ui_tr("help.reset_btn"))) {
			if (!settings_reset_all(settings_default_keep)) overlay_toast(ui_tr("help.not_saved"), 3.0);   /* the one keep list (settings.c): saves and the server know the player by these */
			g_prefs_epoch++; P.loaded = false; prefs_load();   /* the pages re-read the way the menu does on open */
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

/* ---- the full menu (build_ui): a panel over the paused, dimmed game -- the game's name and a Paused chip, the
 * sections down the side, the page, and Resume / Capture / Quit at the foot */
enum { TAB_DISPLAY, TAB_PERF, TAB_GAME, TAB_CONTROLS, TAB_HELP, TAB_N };
static const char *const g_tab_names[TAB_N] = {"Display", "Performance", "Game", "Controls", "Help"};   /* (U12b: English names -- the dump's page selector matches them (:1656); the UI shows the catalog) */
static const char *const g_tab_ids[TAB_N] = {"nav.display", "nav.performance", "nav.game", "nav.controls", "nav.help"};
static const char *const g_tab_blurbs[TAB_N] = {"Resolution, picture and image quality", "Frame rate and on-screen stats",
	"Frame rate, view and detail options, volume", "Keyboard, mouse, controller, gyro and camera", "Shortcuts and about"};
static const char *const g_tab_blurb_ids[TAB_N] = {"blurb.display", "blurb.performance", "blurb.game", "blurb.controls", "blurb.help"};
static int g_select_tab = -1;   /* the bar asked for this page (open_page): selected on the menu's next frame, then -1 */
static int g_section = TAB_DISPLAY;
static void open_page(int tab) { g_select_tab = tab; overlay_open(); }
static void build_ui(void) {
	ImGuiIO &io = ImGui::GetIO();
	const float em = ImGui::GetFontSize();
	if (g_select_tab >= 0) { g_section = g_select_tab; g_select_tab = -1; }
	ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, ui::u32(ui::bg, 0.62f));   /* the paused game, dimmed behind */
	/* (U10) a window at least 70 em wide -- the U2 scale makes that per-aspect, not per-resolution (16:9 is 85 em
	 * at every size, 960x544 is 64 em) -- gets the two-pane page: rows left, the hovered row's help right, in a
	 * wider menu that holds both; below it the rows keep their one-line help */
	const bool two_pane = io.DisplaySize.x >= 70 * em;
	ui::two_pane = two_pane;
	const float wcap = (two_pane ? 76 : 58) * em;
	const float W = io.DisplaySize.x - 2 * em < wcap ? io.DisplaySize.x - 2 * em : wcap;
	const float H = io.DisplaySize.y - 2 * em < 38 * em ? io.DisplaySize.y - 2 * em : 38 * em;
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::surface);
	ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleColor();
	/* header: the game, Paused, Resume */
	{	const char *name = std::getenv("RECOMP_GAME_NAME"); if (!name || !*name) name = std::getenv("RECOMP_TITLE"); if (!name || !*name) name = "Project Nativ";
		ui::heading(name, em * 1.35f);
		ImGui::SameLine(0, em * 0.7f); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + em * 0.18f); ui::chip(ui_tr("header.paused"), ui::accent);
		const char *resume = ui_tr("header.resume");
		float rw = ImGui::CalcTextSize(resume).x + ImGui::GetStyle().FramePadding.x * 2;
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - rw);
		if (ui::primary_button(resume)) overlay_close();
	}
	{	char line[192]; version_line(line, sizeof line);   /* (U4: right-aligned under the header row) */
		if (*line) { float lw = ImGui::CalcTextSize(line).x; ImGui::SetCursorPosX(ImMax(0.0f, ImGui::GetWindowContentRegionMax().x - lw)); ImGui::TextDisabled("%s", line); }
	}
	ImGui::Dummy(ImVec2(0, em * 0.2f));
	const float foot = ImGui::GetFrameHeightWithSpacing() + em * 0.9f;
	/* the sections: tabs across the top (UI-03, menu_shell.h); a controller's L1 / R1 (or Ctrl+Tab, Ctrl+Shift+Tab) step them
	 * and the new page's first row takes the focus; left / right on the tabs pick one; D-pad down from the tabs enters
	 * the rows, up from the first row comes back */
	static const int order[TAB_N] = {TAB_DISPLAY, TAB_GAME, TAB_CONTROLS, TAB_PERF, TAB_HELP};
	static bool focus_page;
	/* (U12c) Circle goes up one level -- a row: to its tab; the tabs: resumes the game, as Esc does (UI-03) -- unless it had
	 * something to cancel: an edited slider, a combo or the reset dialog in the frame before (ImGui's nav took this
	 * frame's cancel before we look) */
	static bool busy_before;
	ImGuiWindow *menu_win = ImGui::GetCurrentWindow();
	static int open_frames;
	if (ImGui::IsWindowAppearing()) open_frames = 0;
	if (!ImGui::IsAnyItemActive() && !g_modal_open) {   /* (U6) no page step under the reset dialog */
		const bool ctrl = io.KeyCtrl, shift = io.KeyShift; int step = 0;
		if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false))) step = -1;
		if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false))) step = 1;
		if (step) { int k = 0; while (k < TAB_N - 1 && order[k] != g_section) k++; g_section = order[(k + step + TAB_N) % TAB_N]; focus_page = true; }
	}
	ImGuiID tab_ids[MENU_TABS_MAX];
	{	const char *labels[TAB_N]; for (int i = 0; i < TAB_N; i++) labels[i] = ui_tr(g_tab_ids[i]);
		menu_tabs(&g_section, order, TAB_N, labels, tab_ids); }
	const ImGuiID tab_now = tab_ids[menu_tab_place(g_section, order, TAB_N)];
	if (open_frames < 2) { ImGui::SetFocusID(tab_now, menu_win); ImGui::SetNavCursorVisible(true); open_frames++; }   /* (the menu opens on the tab in use: ImGui puts the focus on a new window's first item after its first frame) */
	ImGui::Dummy(ImVec2(0, em * 0.15f));
	/* the page: under the tabs, the window's full width (NavFlattened: the D-pad crosses between the tabs and the rows) */
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::bg);
	ImGui::BeginChild("page", ImVec2(0, -foot), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
	ImGui::PopStyleColor();
	ImGuiWindow *page_win = ImGui::GetCurrentWindow();
	static bool tabs_had_focus;
	ui::tabs_down_enters(tabs_had_focus);   /* (UI-03: down from the tabs: the page's first control) */
	ui::heading(ui_tr(g_tab_ids[g_section]), em * 1.25f);
	ImGui::TextDisabled("%s", ui_tr(g_tab_blurb_ids[g_section]));
	if (focus_page) { ui::focus_next_item(); focus_page = false; }   /* (the page's first control -- not taken: UI-03) */
	ImGui::Dummy(ImVec2(0, em * 0.2f));
	static void (*const pages[TAB_N])(void) = {page_display, page_performance, page_game, page_controls, page_help};
	static int shown_section = -1;
	if (shown_section != g_section) { shown_section = g_section; ui::help_label = nullptr; }   /* (U10) a new page: the pane starts at its first row */
	/* (U12c, owner: "why so much space for the explanation?") the help pane takes 26 % of the page, 12-16 em
	 * (was a fixed 23 em) -- its texts are a sentence or two; the rows get the rest */
	const float help_w = ImClamp(ImGui::GetContentRegionAvail().x * 0.26f, em * 12, em * 16);
	if (two_pane) ImGui::BeginChild("rows-l", ImVec2(-(help_w + em * 0.8f), 0), ImGuiChildFlags_NavFlattened);
	pages[g_section]();
	if (two_pane) {
		ImGui::EndChild();
		ImGui::SameLine(0, em * 0.8f);
		ImGui::BeginChild("help-r", ImVec2(0, 0), ImGuiChildFlags_Borders);
		ui::help_pane();
		ImGui::EndChild();
	}
	ui::nav_keep_visible();   /* (UI-03) */
	ImGui::EndChild();
	if (!busy_before && !g_modal_open && g_rebind < 0 && g_pad_rebind < 0 && menu_back(page_win, menu_win, tab_now)) overlay_close();   /* (UI-03) */
	tabs_had_focus = menu_tab_focused(tab_ids, TAB_N);
	/* the foot */
	ImGui::Dummy(ImVec2(0, em * 0.15f));
	if (ImGui::Button(ui_tr("foot.capture"))) g_want_capture = true;
	if (SDL_HasGamepad()) {   /* (UI-01e) the buttons as printed on the pad in use: LB / RB and B on an Xbox pad or the Deck's A/B, Circle on a PlayStation pad */
		int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);
		const int type = ids && n > 0 ? (int)SDL_GetGamepadTypeForID(ids[0]) : -1;
		SDL_free(ids);
		const char *deck = std::getenv("SteamDeck");
		const PadNames *pn = pad_names_of(pad_family(type, deck && !strcmp(deck, "1")));
		const bool keys = GImGui->NavInputSource == ImGuiInputSource_Keyboard;
		char hint[200];
		if (ui::editing()) { if (keys) snprintf(hint, sizeof hint, "%s", ui_tr("foot.edit_key")); else snprintf(hint, sizeof hint, ui_tr("foot.edit_pad"), pn->select, pn->back); }   /* (UI-04) */
		else if (ui::value_focused() && ui::nav_by_pad()) { if (keys) snprintf(hint, sizeof hint, "%s", ui_tr("foot.change_key")); else snprintf(hint, sizeof hint, ui_tr("foot.change_pad"), pn->select, pn->l, pn->r, pn->back); }
		else snprintf(hint, sizeof hint, ui_tr("foot.hint_pad"), pn->l, pn->r, pn->back, pn->menu, pn->menu, pn->view);
		ImGui::SameLine(); ImGui::TextDisabled("%s", hint);
	} else {   /* (no pad: the keys' words) */
		const char *h = ui::editing() ? ui_tr("foot.edit_key") : ui::value_focused() && ui::nav_by_pad() ? ui_tr("foot.change_key") : ui_tr("foot.hint");
		ImGui::SameLine(); ImGui::TextDisabled("%s", h);
	}
	if (g_save_failed) {   /* (U8: until a later save succeeds) */
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, ui::error);
		ImGui::TextUnformatted(ui_tr("foot.save_failed"));
		ImGui::PopStyleColor();
	}
	const char *quit = quit_label();
	float qw = ImGui::CalcTextSize(quit).x + ImGui::GetStyle().FramePadding.x * 2;
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - qw);
	ImGui::PushStyleColor(ImGuiCol_Text, ui::error);
	if (ImGui::Button(quit)) g_want_quit = true;
	ImGui::PopStyleColor();
	busy_before = ImGui::IsAnyItemActive() || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) || g_modal_open || g_rebind >= 0 || g_pad_rebind >= 0;
	g_value_edit = ui::editing();
	ImGui::End();
}

/* ---- the menu bar and everything else the closed state shows (build_closed). All of it runs on the
 * window thread (window.c's present loop), like the full menu. Times are ImGui's own clock (GetTime:
 * io.DeltaTime summed), so the headless path's fixed 1/60 s steps work the same way. */
enum {
	HOT_PX = 4,          /* the top-edge zone: the pointer resting within this many points of the edge opens the bar */
	TAB_NEAR_PX = 48,    /* the pull tab shows while the pointer is within this of the top */
};
static const float HOT_DWELL_S = 0.18f, HIDE_S = 1.0f, SLIDE_S = 0.16f, PIN_S = 4.0f, TOAST_S = 4.5f, CURSOR_HIDE_S = 3.0f;
static float g_bar_vis;                 /* 0 hidden (parked above the window's top edge) .. 1 fully down */
static bool g_bar_want;                 /* where it's heading */
static double g_hot_since = -1;         /* when the pointer entered the top-edge zone (-1: it isn't there) */
static bool g_hot_block;                /* Escape hid the bar: the pointer must leave the zone before it can re-open it */
static double g_bar_active_t = -1e9;    /* the last moment the pointer was over the bar, a menu was open, or the tab pinned it */
static double g_bar_pin_until = -1e9;   /* the pull tab was tapped: the bar stays at least until then */
static float g_tab_a;                   /* the pull tab's alpha */
static double g_toast_until = -1e9; static bool g_toast_checked;   /* the first-launch menu hint's end (the pull tab shows with it) */
/* the toasts: badges at the top right, newest on top (overlay_toast_titled: posted by any thread, taken in by the next frame) */
struct Toast { char title[48]; char body[224]; double start, until; };
static Toast g_toasts[3]; static int g_ntoasts;
static Toast g_toast_q[8]; static int g_toast_qn; static std::mutex g_toast_mx;
static double g_idle_since; static ImVec2 g_last_mouse(-1, -1);
static bool g_dismiss_req;              /* overlay_bar_dismiss (Escape in window.c): close the menus, hide the bar */
static bool g_mouse_captured;           /* io.WantCaptureMouse as of this thread's last ImGui frame (overlay_mouse_captured) */
static const char *g_force_menu; static bool g_force_bar, g_force_toast;   /* RECOMP_OVERLAY_DUMP_UI (overlay_render_forced) */
static bool g_touch_cursor;             /* overlay_touch_cursor: the pointer is a touch mark this frame -- no arrow */
static bool g_skip_draw;                /* overlay_render_forced's warm-up frames: built (ImGui sizes its windows) but not drawn */
static float g_bar_h;                   /* the bar's height this frame, in points (0: none drawn) -- the HUD moves down by it */

void overlay_bar_dismiss(void) { if (!g_ctx) return; if (g_intro) intro_end(); else g_dismiss_req = true; }   /* (Escape: the touch card skipped, else the bar away) */
int overlay_mouse_captured(void) { return g_ctx && g_has_window && g_mouse_captured; }

static void menu_game(void) {
	if (ImGui::MenuItem(g_paused ? ui_tr("header.resume") : ui_tr("header.pause"))) overlay_set_paused(!g_paused);
	ImGui::TextUnformatted("%s", ui_tr("game.volume")); ImGui::SameLine(); volume_slider();
	if (ImGui::MenuItem(ui_tr("bar.capture"), "F12")) g_want_capture = true;
	if (ImGui::MenuItem(ui_tr("bar.full_menu"), "F10")) overlay_open();
	ImGui::Separator();
	if (ImGui::MenuItem(quit_label())) g_want_quit = true;
}
static void menu_view(void) {
	if (ImGui::BeginMenu(ui_tr("display.resolution"))) {
		int rs = gfx_gpu_render_scale_wanted();
		for (int n = 1; n <= gfx_gpu_render_scale_max(); n++) { char l[48]; scale_label(l, sizeof l, n); if (ImGui::MenuItem(l, nullptr, rs == n)) gfx_gpu_render_scale_request(n); }
		ImGui::Separator();
		int mw = scale_for_window();
		if (mw) { char l[48]; std::snprintf(l, sizeof l, ui_tr("bar.match_window"), mw); if (ImGui::MenuItem(l)) gfx_gpu_render_scale_request(mw); }
		if (ImGui::MenuItem(ui_tr("bar.cycle"), "F2")) gfx_gpu_render_scale_cycle();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu(ui_tr("display.fit"))) {
		int am = window_get_aspect();
		for (int i = 0; i < 3; i++) if (ImGui::MenuItem(ui_tr(g_aspect_names[i]), nullptr, am == i)) window_set_aspect(i);
		ImGui::EndMenu();
	}
	bool fs = window_get_fullscreen() != 0;
	if (ImGui::MenuItem(ui_tr("display.fullscreen"), nullptr, &fs)) window_set_fullscreen(fs ? 1 : 0);
	ImGui::Separator();
	prefs_load();
	if (game_option("fov")) { ImGui::TextDisabled("%s", ui_tr("game.fov")); ImGui::SetNextItemWidth(170 * g_ui_scale); if (ui::slider_float("##fov", &P.fov, 0.75f, 1.5f, "%.2fx")) set_fov(P.fov); }
	if (game_option("view_distance")) { ImGui::TextDisabled("%s", ui_tr("game.view")); ImGui::SetNextItemWidth(170 * g_ui_scale); if (ui::slider_float("##view", &P.view, 1.0f, 3.0f, "%.2fx")) set_view(P.view); }
	if (game_option("scenery_detail")) { ImGui::TextDisabled("%s", ui_tr("game.scen")); ImGui::SetNextItemWidth(170 * g_ui_scale); if (ui::slider_float("##scen", &P.scen, 1.0f, 8.0f, "%.1fx")) set_scen(P.scen); }
	if (game_option("character_detail")) { ImGui::TextDisabled("%s", ui_tr("game.chr")); ImGui::SetNextItemWidth(170 * g_ui_scale); if (ui::slider_float("##chr", &P.chr, 1.0f, 8.0f, "%.1fx")) set_chr(P.chr); }
	if (ImGui::BeginMenu(ui_tr("game.fps"))) {
		int l[3], n = fr_list(display_frame_rates_allowed(), l), cur = fr_effective(P.fr, display_frame_rates_allowed());
		for (int i = 0; i < n; i++) if (ImGui::MenuItem(l[i] >= 120 ? ui_tr("bar.fps120") : l[i] >= 60 ? ui_tr("bar.fps60") : "30", nullptr, cur == l[i])) set_frame_rate(l[i]);
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu(ui_tr("perf.osd"))) {
		const char *const names[3] = {ui_tr("common.off"), "FPS", ui_tr("perf.osd_full")};
		int lvl = metrics_osd_level();
		for (int i = 0; i < 3; i++) if (ImGui::MenuItem(names[i], nullptr, lvl == i)) set_osd(i);
		ImGui::Separator();
		if (ImGui::MenuItem(ui_tr("bar.cycle"), "F1")) metrics_osd_cycle();
		ImGui::EndMenu();
	}
}
static void menu_controls(void) {
	prefs_load();
	ImGui::TextDisabled("%s", ui_tr("bar.touch_speed"));
	ImGui::SetNextItemWidth(170 * g_ui_scale);
	if (ui::slider_float("##tcs", &P.tcs, 0.25f, 3.0f, "%.2fx")) set_tcs(P.tcs);
	ImGui::Separator();
	bool gy = P.gyro != 0;
	if (ImGui::MenuItem(ui_tr("bar.gyro_toggle"), nullptr, &gy)) set_gyro(gy);
	if (ImGui::BeginMenu(ui_tr("bar.gyro_pose"))) {
		if (ImGui::MenuItem(ui_tr("bar.upright"), nullptr, !P.gyro_flat)) set_gyro_pose(0);
		if (ImGui::MenuItem(ui_tr("bar.flat"), nullptr, P.gyro_flat != 0)) set_gyro_pose(1);
		ImGui::EndMenu();
	}
	ImGui::TextDisabled("%s", ui_tr("ctrl.gyro_sens"));
	ImGui::SetNextItemWidth(170 * g_ui_scale);
	if (ui::slider_float("##gs", &P.gyro_sens, 0.25f, 3.0f, "%.2fx")) set_gyro_sens(P.gyro_sens);
	if (ImGui::MenuItem(ui_tr("ctrl.recentre_btn"), "R3")) motion_recenter();
	ImGui::Separator();
	if (ImGui::MenuItem(ui_tr("bar.controls_ref"))) open_page(TAB_CONTROLS);   /* (the full menu: pauses) */
}
static void menu_help(void) {
	if (ImGui::MenuItem(ui_tr("bar.shortcuts"))) open_page(TAB_HELP);
	if (ImGui::MenuItem(ui_tr("bar.about"))) open_page(TAB_HELP);
}
/* the bar: a window the width of the display, exactly one menu-bar high, parked above the top edge by
 * (1 - vis) of its height -- the slide. Its menus are ImGui popups below it. */
static void build_bar(float w) {
	const float h = ImGui::GetFrameHeight();
	g_bar_h = h;
	const float e = g_bar_vis * g_bar_vis * (3.0f - 2.0f * g_bar_vis);   /* smoothstep: eases in and out */
	ImGui::SetNextWindowPos(ImVec2(0, -(1.0f - e) * (h + 1)));
	ImGui::SetNextWindowSize(ImVec2(w, h));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	const ImVec4 bg = ui::with_alpha(ui::surface, 0.96f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
	ImGui::PushStyleColor(ImGuiCol_MenuBarBg, bg);
	const ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
	const bool open = ImGui::Begin("##menubar", nullptr, fl);
	ImGui::PopStyleVar(4);   /* (the bar window's own: the menus it opens get the theme's padding, a little tighter) */
	if (open && ImGui::BeginMenuBar()) {
		const float em = ImGui::GetFontSize();
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.7f, em * 0.55f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em * 0.6f, em * 0.45f));
		/* (headless dump: a menu forced open -- OpenPopup here shares BeginMenu's ID scope, so the ids match) */
		if (g_force_menu) {   /* (U12b) the dump's selector key -> the translated menu label, in this frame's language */
			const char *k = g_force_menu; g_force_menu = nullptr;
			if (!strcmp(k, "Game")) ImGui::OpenPopup(ui_tr("nav.game"));
			else if (!strcmp(k, "View")) ImGui::OpenPopup(ui_tr("bar.view"));
			else if (!strcmp(k, "Controls")) ImGui::OpenPopup(ui_tr("nav.controls"));
			else if (!strcmp(k, "Help")) ImGui::OpenPopup(ui_tr("nav.help"));
			else ImGui::OpenPopup(k);
		}
		if (ImGui::BeginMenu(ui_tr("nav.game"))) { menu_game(); ImGui::EndMenu(); }
		if (ImGui::BeginMenu(ui_tr("bar.view"))) { menu_view(); ImGui::EndMenu(); }
		if (ImGui::BeginMenu(ui_tr("nav.controls"))) { menu_controls(); ImGui::EndMenu(); }
		if (ImGui::BeginMenu(ui_tr("nav.help"))) { menu_help(); ImGui::EndMenu(); }
		ImGui::PopStyleVar(2);
		ImGui::EndMenuBar();
	}
	ImGui::End();
	ImGui::PopStyleColor(2);
}
/* the pull tab: a small rounded tongue at the top centre with a chevron, in a window of its own size so
 * it's hoverable and tappable (touch has no top-edge hover); a tap brings the bar down and pins it a while */
static void build_tab(float w, double t) {
	const float tw = 72, th = 14;
	ImGui::SetNextWindowPos(ImVec2(w * 0.5f - tw * 0.5f, 0));
	ImGui::SetNextWindowSize(ImVec2(tw, th));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse;
	if (ImGui::Begin("##pulltab", nullptr, fl)) {
		if (ImGui::InvisibleButton("tab", ImVec2(tw, th))) { g_bar_want = true; g_bar_pin_until = t + PIN_S; g_bar_active_t = t; }
		const bool hov = ImGui::IsItemHovered();
		/* the window is the hit area; the picture goes on the foreground list, over anything else (the HUD
		 * panel sits at the top-left on the background list). NB ImGui 1.92.8+ takes AddRect(..., rounding,
		 * THICKNESS, flags): flags passed in the thickness slot draw a 192-px-thick outline (a star burst). */
		ImDrawList *dl = ImGui::GetForegroundDrawList();
		const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
		dl->AddRectFilled(p0, p1, IM_COL32(30, 30, 36, (int)(g_tab_a * (hov ? 240 : 190))), 6.0f, ImDrawFlags_RoundCornersBottom);
		dl->AddRect(p0, p1, IM_COL32(255, 255, 255, (int)(g_tab_a * 60)), 6.0f, 1.0f, ImDrawFlags_RoundCornersBottom);
		const ImU32 c = IM_COL32(230, 230, 240, (int)(g_tab_a * 235));
		const float cx = (p0.x + p1.x) * 0.5f, cy = p0.y + th * 0.3f;
		dl->AddLine(ImVec2(cx - 6, cy), ImVec2(cx, cy + 5), c, 2.0f);
		dl->AddLine(ImVec2(cx, cy + 5), ImVec2(cx + 6, cy), c, 2.0f);
	}
	ImGui::End();
	ImGui::PopStyleVar(3);
}
extern "C" void overlay_touch_cursor(int on) { g_touch_cursor = on != 0; }
extern "C" void overlay_toast_titled(const char *title, const char *body, double seconds) {
	std::lock_guard<std::mutex> l(g_toast_mx);
	if (g_toast_qn == 8) { std::memmove(g_toast_q, g_toast_q + 1, sizeof g_toast_q[0] * 7); g_toast_qn--; }   /* (a burst: the oldest unseen goes) */
	Toast &q = g_toast_q[g_toast_qn++];
	std::snprintf(q.title, sizeof q.title, "%s", title ? title : ""); std::snprintf(q.body, sizeof q.body, "%s", body ? body : "");
	q.start = 0; q.until = seconds;   /* (a duration until taken in) */
}
extern "C" void overlay_toast(const char *msg, double seconds) { overlay_toast_titled(nullptr, msg, seconds); }
/* the posted toasts into the stack: one already showing (same title and text) runs on instead of stacking twice;
 * three at most, the oldest dropped; each gone when its time is up */
static void toasts_take(double t) {
	{
		std::lock_guard<std::mutex> l(g_toast_mx);
		for (int i = 0; i < g_toast_qn; i++) {
			Toast q = g_toast_q[i]; double dur = q.until;
			int k = 0; while (k < g_ntoasts && (std::strcmp(g_toasts[k].title, q.title) || std::strcmp(g_toasts[k].body, q.body))) k++;
			if (k < g_ntoasts) { g_toasts[k].until = t + dur; continue; }
			if (g_ntoasts == 3) g_ntoasts--;
			std::memmove(g_toasts + 1, g_toasts, sizeof g_toasts[0] * g_ntoasts); g_ntoasts++;
			q.start = t; q.until = t + dur; g_toasts[0] = q;
		}
		g_toast_qn = 0;
	}
	int n = 0; for (int i = 0; i < g_ntoasts; i++) if (t < g_toasts[i].until) g_toasts[n++] = g_toasts[i];
	g_ntoasts = n;
}
/* A toast as a badge at the top right (below the menu bar when it's down): a dark card with an amber edge, the
 * title in amber, the text under it, wrapped to a third of the window; "[F10]" in the text draws as a key cap.
 * It slides in, and fades over its last 0.4 s. */
struct ToastTok { const char *s; int n; bool key, brk; float w; };
static int toast_tokens(const char *body, ImFont *font, float fs, float cap_pad, ToastTok *out, int max) {
	int n = 0;
	for (const char *p = body; *p && n < max; ) {
		if (*p == '\n') { out[n++] = {p, 0, false, true, 0}; p++; continue; }
		if (*p == ' ') { p++; continue; }
		const char *e = p; bool key = *p == '[' && std::strchr(p, ']');
		if (key) { e = std::strchr(p, ']') + 1; out[n] = {p + 1, (int)(e - p - 2), true, false, 0}; }
		else { while (*e && *e != ' ' && *e != '\n' && !(*e == '[' && e > p)) e++; out[n] = {p, (int)(e - p), false, false, 0}; }
		out[n].w = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, out[n].s, out[n].s + out[n].n).x + (key ? 2 * cap_pad : 0);
		n++; p = e;
	}
	return n;
}
static void build_toasts(float w, float hgt, double t) {
	(void)hgt;
	/* (GetFontSize carries the window's scale, ui_scale.h: a toast keeps the share of the window it has at 1280x720) */
	ImFont *font = ImGui::GetFont(); const float fs = ImGui::GetFontSize() * (w >= 1000 ? 2.0f : 1.0f);   /* (the bitmap font: integer multiples stay crisp) */
	const float pad = fs * 0.75f, edge = fs * 0.3f, cap_pad = fs * 0.3f, space = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, " ").x, lh = fs * 1.35f;
	const float maxw = fmaxf(w / 3.0f, fs * 18.0f);
	float y = fs * 0.75f + g_bar_h * g_bar_vis;
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	for (int i = 0; i < g_ntoasts; i++) {
		const Toast &q = g_toasts[i];
		const float in = g_has_window ? (float)fmin(1.0, (t - q.start) / 0.15) : 1.0f, a = (float)fmin(1.0, (q.until - t) / 0.4) * in;   /* (a headless dump: as it settles) */
		ToastTok tok[64]; const int nt = toast_tokens(q.body, font, fs, cap_pad, tok, 64);
		/* the lines: tokens laid left to right, a new line at a "\n" or where the next would pass maxw */
		float lx = 0, widest = q.title[0] ? font->CalcTextSizeA(fs, FLT_MAX, 0.0f, q.title).x : 0; int lines = nt ? 1 : 0;
		float tx[64], ty_line[64];
		for (int k = 0; k < nt; k++) {
			if (tok[k].brk) { lines++; lx = 0; tx[k] = 0; ty_line[k] = (float)(lines - 1); continue; }
			if (lx > 0 && lx + space + tok[k].w > maxw) { lines++; lx = 0; }
			if (lx > 0) lx += space;
			tx[k] = lx; ty_line[k] = (float)(lines - 1); lx += tok[k].w; if (lx > widest) widest = lx;
		}
		const float cw = widest + 2 * pad + edge, ch = (q.title[0] ? lh : 0) + lines * lh + 2 * pad - (lh - fs);
		const float x0 = w - fs * 0.75f - cw + (1 - in) * fs * 1.5f;
		const ImVec2 p0(x0, y), p1(x0 + cw, y + ch);
		dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 26, (int)(a * 235)), fs * 0.4f);
		dl->AddRectFilled(p0, ImVec2(p0.x + edge, p1.y), IM_COL32(255, 186, 72, (int)(a * 255)), fs * 0.4f, ImDrawFlags_RoundCornersLeft);
		dl->AddRect(p0, p1, IM_COL32(255, 255, 255, (int)(a * 40)), fs * 0.4f);
		float cy = y + pad, cx = x0 + edge + pad;
		if (q.title[0]) { dl->AddText(font, fs, ImVec2(cx, cy), IM_COL32(255, 196, 92, (int)(a * 255)), q.title); cy += lh; }
		for (int k = 0; k < nt; k++) {
			if (tok[k].brk) continue;
			const ImVec2 at(cx + tx[k], cy + ty_line[k] * lh);
			if (tok[k].key) {   /* a key cap */
				dl->AddRectFilled(ImVec2(at.x, at.y - fs * 0.12f), ImVec2(at.x + tok[k].w, at.y + fs * 1.12f), IM_COL32(58, 62, 74, (int)(a * 255)), fs * 0.2f);
				dl->AddRect(ImVec2(at.x, at.y - fs * 0.12f), ImVec2(at.x + tok[k].w, at.y + fs * 1.12f), IM_COL32(150, 156, 172, (int)(a * 255)), fs * 0.2f);
				dl->AddText(font, fs, ImVec2(at.x + cap_pad, at.y), IM_COL32(255, 255, 255, (int)(a * 255)), tok[k].s, tok[k].s + tok[k].n);
			} else dl->AddText(font, fs, at, IM_COL32(232, 234, 240, (int)(a * 255)), tok[k].s, tok[k].s + tok[k].n);
		}
		y = p1.y + fs * 0.5f;
	}
}
/* Game > Pause (not the menu): the picture stays, with this over it */
static void build_pause_banner(float w, float hgt) {
	ImFont *font = ImGui::GetFont(); const float fs = ImGui::GetFontSize() * 2.0f;
	const char *const l1 = ui_tr("bar.paused"), *const l2 = ui_tr("bar.paused_hint");
	const ImVec2 s1 = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, l1), s2 = font->CalcTextSizeA(fs * 0.5f, FLT_MAX, 0.0f, l2);
	const float pad = 12, bw = fmaxf(s1.x, s2.x) + 2 * pad, bh = s1.y + s2.y + 3 * pad;
	const ImVec2 p0(w * 0.5f - bw * 0.5f, hgt * 0.5f - bh * 0.5f), p1(p0.x + bw, p0.y + bh);
	ImDrawList *dl = ImGui::GetBackgroundDrawList();   /* (under the bar's menus, like the HUD) */
	dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 170), 8.0f);
	dl->AddText(font, fs, ImVec2(w * 0.5f - s1.x * 0.5f, p0.y + pad), IM_COL32(255, 255, 255, 255), l1);
	dl->AddText(font, fs * 0.5f, ImVec2(w * 0.5f - s2.x * 0.5f, p0.y + 2 * pad + s1.y), IM_COL32(205, 205, 215, 255), l2);
}
/* the HUD while the menu is closed: the touch markers (window.c's gamepad touch) and the performance overlay
 * (metrics.c's F1 lines) -- no window, no input, the foreground draw list only */
static const OverlayMark *g_marks; static int g_nmarks;
static float g_rear[4];   /* the rear panel's rows on the picture, in window points (x, y, w, h); w 0: none */
static char g_osd[7][40]; static int g_nosd;   /* the overlay's lines this frame (metrics_osd_lines) */
static float g_ft[240]; static int g_nft;      /* the full level's frame-time graph this frame (metrics_frame_times); 0: none */
static MetricsSnapshot g_snap; static bool g_full;   /* the full level's numbers this frame (metrics_snapshot) */
int overlay_draws_hud(void) { return g_ctx && g_has_window; }
/* ---- the full performance overlay (F1's level 2): the machine, the game's frames, the renderer and the picture, in
 * coloured groups and three columns. A number this machine or backend does not give is drawn as "-". Sized from the
 * window's height (a vector font: any size is sharp). */
static void perf_fmt(char *b, size_t n, float v, const char *fmt) { if (v < 0) std::snprintf(b, n, "-"); else std::snprintf(b, n, fmt, v); }
static void build_perf_full(ImDrawList *dl) {
	const MetricsSnapshot &m = g_snap;
	ImFont *font = ImGui::GetFont();
	const float H = ImGui::GetIO().DisplaySize.y, fs = H / 46.0f < 13.0f ? 13.0f : H / 46.0f > 30.0f ? 30.0f : H / 46.0f;
	const float pad = fs * 0.6f, lh = fs * 1.25f, W = fs * 21.0f;
	const float x0 = 8, y0 = 8 + g_bar_h * g_bar_vis;
	const ImU32 c_gpu = IM_COL32(140, 225, 120, 255), c_cpu = IM_COL32(110, 175, 255, 255), c_ram = IM_COL32(205, 145, 235, 255),
	            c_txt = IM_COL32(245, 245, 245, 255), c_mut = IM_COL32(170, 170, 180, 255), c_ren = IM_COL32(240, 200, 90, 255), c_dsp = IM_COL32(150, 195, 245, 255);
	const float cx1 = x0 + pad + fs * 9.0f, cx2 = x0 + pad + fs * 14.5f, cx3 = x0 + W - pad;   /* right edges of the three value columns */
	struct Row { const char *label; ImU32 col; char v[3][48]; };
	auto text_r = [&](float xr, float y, ImU32 col, const char *t) { const float w = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, t).x; dl->AddText(font, fs, ImVec2(xr - w, y), col, t); };
	auto gb = [](char *b, size_t n, float mb, const char *pre) { if (mb < 0) std::snprintf(b, n, "%s-", pre); else std::snprintf(b, n, "%s%.1f GB", pre, mb / 1024.0f); };
	const bool mach = m.machine != 0;

	Row top[6] = {};
	top[0].label = "GPU"; top[0].col = c_gpu;
	perf_fmt(top[0].v[0], 48, mach ? m.gpu_pct : -1, "%.0f%%"); perf_fmt(top[0].v[1], 48, mach ? m.gpu_temp_c : -1, "%.0f \xc2\xb0""C");
	if (mach && m.gpu_power_w >= 0 && m.gpu_clock_mhz >= 0) std::snprintf(top[0].v[2], 48, "%.0f W  %.0f MHz", m.gpu_power_w, m.gpu_clock_mhz); else perf_fmt(top[0].v[2], 48, mach ? m.gpu_power_w : -1, "%.0f W");
	top[1].label = "VRAM"; top[1].col = c_gpu; gb(top[1].v[0], 48, mach ? m.vram_used_mb : -1, ""); gb(top[1].v[1], 48, mach ? m.vram_total_mb : -1, "/ ");
	top[2].label = "CPU"; top[2].col = c_cpu; perf_fmt(top[2].v[0], 48, mach ? m.cpu_total_pct : -1, "%.0f%%"); std::snprintf(top[2].v[1], 48, "-");
	if (mach && m.cpu_game_pct >= 0) std::snprintf(top[2].v[2], 48, "game %.0f%%", m.cpu_game_pct); else std::snprintf(top[2].v[2], 48, "game -");
	top[3].label = "RAM"; top[3].col = c_ram; gb(top[3].v[0], 48, mach ? m.ram_used_mb : -1, ""); gb(top[3].v[1], 48, mach ? m.ram_total_mb : -1, "/ "); gb(top[3].v[2], 48, mach ? m.ram_game_mb : -1, "game ");
	top[4].label = "FPS"; top[4].col = c_txt; std::snprintf(top[4].v[0], 48, "%.0f", m.fps);
	if (m.low1_fps >= 0) std::snprintf(top[4].v[1], 48, "1%% low %.0f", m.low1_fps); else std::snprintf(top[4].v[1], 48, "1%% low -");
	if (m.low01_fps >= 0) std::snprintf(top[4].v[2], 48, "0.1%% low %.0f", m.low01_fps); else std::snprintf(top[4].v[2], 48, "0.1%% low -");
	top[5].label = "Frame"; top[5].col = c_txt; std::snprintf(top[5].v[0], 48, "%.1f ms", m.frame_ms_avg); std::snprintf(top[5].v[1], 48, "max %.1f ms", m.frame_ms_max);

	/* the renderer's and the picture's rows: a label and one value at the right edge */
	char rv[8][48]; const char *rl[8]; ImU32 rc[8]; int nr = 0;
	auto row2 = [&](const char *l, ImU32 c) -> char * { rl[nr] = l; rc[nr] = c; return rv[nr++]; };
	perf_fmt(row2(ui_tr("sys.render"), c_ren), 48, m.render_ms, "%.2f ms");
	std::snprintf(row2(ui_tr("sys.gpu_busy"), c_ren), 48, "-");   /* (SDL's GPU API has no timestamp queries) */
	if (m.builds >= 0) std::snprintf(row2(ui_tr("sys.builds"), c_ren), 48, "%.1f (%.2f ms)", m.builds, m.build_ms); else std::snprintf(row2(ui_tr("sys.builds"), c_ren), 48, "-");
	perf_fmt(row2(ui_tr("sys.waits"), c_ren), 48, m.waits_ms, "%.2f ms");
	perf_fmt(row2(ui_tr("sys.uploads"), c_ren), 48, m.tex_uploads, "%.1f per frame");
	perf_fmt(row2(ui_tr("sys.draws_waiting"), c_ren), 48, m.skipped, "%.1f per frame");
	std::snprintf(row2(ui_tr("sys.scenes"), c_ren), 48, m.fallback ? "%u / %u (%u software)" : "%u / %u", m.scenes, m.draws, m.fallback);
	std::snprintf(row2(ui_tr("sys.audio"), c_ren), 48, m.audio_underruns ? "%u ms, %u underruns" : "%u ms", m.audio_ms, m.audio_underruns);
	char dv[6][64]; const char *dl_[6]; int nd = 0;
	uint32_t ww = 0, wh = 0; window_pixel_size(&ww, &wh);
	const int rs = m.render_scale > 0 ? m.render_scale : 1;
	dl_[nd] = ui_tr("sys.window"); if (ww && wh) std::snprintf(dv[nd++], 64, "%ux%u", ww, wh); else std::snprintf(dv[nd++], 64, "-");
	dl_[nd] = ui_tr("sys.game_image"); std::snprintf(dv[nd++], 64, "%dx%d (%dx)", 960 * rs, 544 * rs, rs);
	dl_[nd] = ui_tr("sys.upscaling"); std::snprintf(dv[nd++], 64, "%s", gfx_gpu_fsr_active() ? "FSR" : "bilinear");
	dl_[nd] = ui_tr("sys.frame_limit"); std::snprintf(dv[nd++], 64, "%d FPS", display_frame_rate());
	dl_[nd] = ui_tr("sys.presentation"); std::snprintf(dv[nd++], 64, "%s", window_present_mode());
	dl_[nd] = ui_tr("sys.renderer"); std::snprintf(dv[nd++], 64, "%s", m.gpu_backend ? "GPU" : "software");

	const char *title = std::getenv("RECOMP_GAME_NAME"); if (!title || !*title) title = std::getenv("RECOMP_TITLE"); if (!title || !*title) title = "Project Nativ";
	const float gh = fs * 2.6f;
	const float h = pad * 2 + lh * (1 + 6) + pad + gh + pad + lh * nr + pad * 0.5f + lh * nd + pad * 0.5f + lh * 2;
	dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + W, y0 + h), IM_COL32(0, 0, 0, 175), fs * 0.4f);
	float y = y0 + pad; const float xl = x0 + pad;
	dl->AddText(font, fs, ImVec2(xl, y), c_mut, title); y += lh;
	for (const Row &r : top) {
		dl->AddText(font, fs, ImVec2(xl, y), r.col, r.label);
		if (r.v[0][0]) text_r(cx1, y, c_txt, r.v[0]);
		if (r.v[1][0]) text_r(cx2, y, c_txt, r.v[1]);
		if (r.v[2][0]) text_r(cx3, y, c_txt, r.v[2]);
		y += lh;
	}
	/* the graph: the newest frames, newest at the right; guides at 16.7 ms (60 fps) and 33.3 ms (30 fps); the scale is
	 * 50 ms, or the worst frame shown when that is longer */
	y += pad * 0.5f;
	const float gx = xl, gw = W - 2 * pad, gy = y;
	dl->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + gw, gy + gh), IM_COL32(40, 70, 130, 150), fs * 0.25f);
	if (m.nframes >= 2) {
		float top_ms = 50.0f; for (int i = 0; i < m.nframes; i++) if (m.frame_ms[i] > top_ms) top_ms = m.frame_ms[i];
		for (float guide : {1000.0f / 60.0f, 1000.0f / 30.0f}) { const float ly = gy + gh - gh * guide / top_ms; dl->AddLine(ImVec2(gx, ly), ImVec2(gx + gw, ly), IM_COL32(255, 255, 255, 55), 1.0f); }
		ImVec2 pts[METRICS_GRAPH];
		for (int i = 0; i < m.nframes; i++) pts[i] = ImVec2(gx + gw * i / (float)(m.nframes - 1), gy + gh - gh * m.frame_ms[i] / top_ms);
		dl->AddPolyline(pts, m.nframes, IM_COL32(225, 235, 255, 230), ImDrawFlags_None, fs * 0.08f > 1.0f ? fs * 0.08f : 1.0f);
	}
	y = gy + gh + pad;
	for (int i = 0; i < nr; i++) { dl->AddText(font, fs, ImVec2(xl, y), rc[i], rl[i]); text_r(cx3, y, c_txt, rv[i]); y += lh; }
	y += pad * 0.5f;
	for (int i = 0; i < nd; i++) { dl->AddText(font, fs, ImVec2(xl, y), c_dsp, dl_[i]); text_r(cx3, y, c_txt, dv[i]); y += lh; }
	y += pad * 0.5f;
	dl->AddText(font, fs, ImVec2(xl, y), c_mut, m.gpu_name[0] ? m.gpu_name : "GPU: -"); y += lh;
	dl->AddText(font, fs, ImVec2(xl, y), c_mut, m.cpu_name[0] ? m.cpu_name : "CPU: -");
}
/* (UI-01n) the on-screen buttons (touch_controls.h, the layout window.c presses by): translucent, a held one lit; the
 * face buttons as the Vita's shapes, the D-pad's arms lit by direction, a stick's knob under its finger */
extern "C" int window_osb_state(float *size, unsigned *held, unsigned *bits, float knob[4]);
static bool g_force_osb;   /* RECOMP_OVERLAY_DUMP_UI=osb: shown, nothing held */
static void build_osb(ImDrawList *dl) {
	float size = 1.0f, knob[4] = {0, 0, 0, 0}; unsigned held = 0, bits = 0;
	bool shown = window_osb_state(&size, &held, &bits, knob) != 0;
	if (g_force_osb) { shown = true; held = bits = 0; size = (float)settings_float("osb_size", 1.0); }
	if (!shown || g_intro) return;
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	OsbLayout L; osb_layout(&L, ds.x, ds.y, size);
	const float u = L.unit, lw = u * 0.045f > 1.5f ? u * 0.045f : 1.5f;
	const ImU32 fill = IM_COL32(16, 18, 24, 96), lit = IM_COL32(255, 255, 255, 120), edge = IM_COL32(255, 255, 255, 150), ink = IM_COL32(255, 255, 255, 215);
	ImFont *font = ImGui::GetFont(); const float fs = u * 0.38f;
	auto label = [&](const OsbCtrl &c) {
		const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, c.label);
		dl->AddText(font, fs, ImVec2(c.x + (c.w - ts.x) * 0.5f, c.y + (c.h - ts.y) * 0.5f), ink, c.label);
	};
	for (int i = 0; i < OSB_N; i++) {
		const OsbCtrl &c = L.c[i]; const bool on = (held >> i) & 1u;
		const ImVec2 mid(c.x + c.w * 0.5f, c.y + c.h * 0.5f);
		if (i == OSB_DPAD) {   /* a plus of four arms, each lit by its direction */
			const float a = u * 0.5f;
			const struct { float x0, y0, x1, y1; unsigned bit; } arm[4] = {
				{mid.x - a, c.y, mid.x + a, mid.y - a, OSB_BIT_UP}, {mid.x - a, mid.y + a, mid.x + a, c.y + c.h, OSB_BIT_DOWN},
				{c.x, mid.y - a, mid.x - a, mid.y + a, OSB_BIT_LEFT}, {mid.x + a, mid.y - a, c.x + c.w, mid.y + a, OSB_BIT_RIGHT}};
			dl->AddRectFilled(ImVec2(mid.x - a, mid.y - a), ImVec2(mid.x + a, mid.y + a), fill);
			for (const auto &r : arm) {
				dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), (bits & r.bit) ? lit : fill, u * 0.12f);
				dl->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), edge, u * 0.12f, 0, lw);
				const ImVec2 cm((r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f); const float s = u * 0.16f;   /* the arrow */
				if (r.bit == OSB_BIT_UP) dl->AddTriangleFilled(ImVec2(cm.x, cm.y - s), ImVec2(cm.x - s, cm.y + s * 0.6f), ImVec2(cm.x + s, cm.y + s * 0.6f), ink);
				if (r.bit == OSB_BIT_DOWN) dl->AddTriangleFilled(ImVec2(cm.x, cm.y + s), ImVec2(cm.x + s, cm.y - s * 0.6f), ImVec2(cm.x - s, cm.y - s * 0.6f), ink);
				if (r.bit == OSB_BIT_LEFT) dl->AddTriangleFilled(ImVec2(cm.x - s, cm.y), ImVec2(cm.x + s * 0.6f, cm.y + s), ImVec2(cm.x + s * 0.6f, cm.y - s), ink);
				if (r.bit == OSB_BIT_RIGHT) dl->AddTriangleFilled(ImVec2(cm.x + s, cm.y), ImVec2(cm.x - s * 0.6f, cm.y - s), ImVec2(cm.x - s * 0.6f, cm.y + s), ink);
			}
		} else if (i == OSB_LSTICK || i == OSB_RSTICK) {   /* the ring, and the knob where the finger is (clamped to the ring) */
			const float r = c.w * 0.5f; const int k = i == OSB_RSTICK;
			dl->AddCircleFilled(mid, r, fill, 48); dl->AddCircle(mid, r, edge, 48, lw);
			ImVec2 kp = mid;
			if (on) { float dx = knob[2 * k] - mid.x, dy = knob[2 * k + 1] - mid.y; const float d = std::sqrt(dx * dx + dy * dy), lim = r - u * 0.55f; if (d > lim && d > 0) { dx *= lim / d; dy *= lim / d; } kp = ImVec2(mid.x + dx, mid.y + dy); }
			dl->AddCircleFilled(kp, u * 0.55f, on ? lit : IM_COL32(255, 255, 255, 60), 32); dl->AddCircle(kp, u * 0.55f, edge, 32, lw);
		} else if (c.round) {   /* a face button: a circle with its shape */
			const float r = c.w * 0.5f, s = u * 0.2f;
			dl->AddCircleFilled(mid, r, on ? lit : fill, 32); dl->AddCircle(mid, r, edge, 32, lw);
			if (i == OSB_TRIANGLE) dl->AddTriangle(ImVec2(mid.x, mid.y - s * 1.1f), ImVec2(mid.x - s * 1.1f, mid.y + s * 0.8f), ImVec2(mid.x + s * 1.1f, mid.y + s * 0.8f), ink, lw * 1.4f);
			if (i == OSB_CIRCLE) dl->AddCircle(mid, s, ink, 24, lw * 1.4f);
			if (i == OSB_CROSS) { dl->AddLine(ImVec2(mid.x - s, mid.y - s), ImVec2(mid.x + s, mid.y + s), ink, lw * 1.4f); dl->AddLine(ImVec2(mid.x + s, mid.y - s), ImVec2(mid.x - s, mid.y + s), ink, lw * 1.4f); }
			if (i == OSB_SQUARE) dl->AddRect(ImVec2(mid.x - s * 0.9f, mid.y - s * 0.9f), ImVec2(mid.x + s * 0.9f, mid.y + s * 0.9f), ink, 0.0f, 0, lw * 1.4f);
		} else {   /* L, R, Select, Start: a rounded box with its name */
			dl->AddRectFilled(ImVec2(c.x, c.y), ImVec2(c.x + c.w, c.y + c.h), on ? lit : fill, c.h * 0.45f);
			dl->AddRect(ImVec2(c.x, c.y), ImVec2(c.x + c.w, c.y + c.h), edge, c.h * 0.45f, 0, lw);
			label(c);
		}
	}
}
/* (UI-02a) the player's controller: its SDL type (-1: none), for the names in a hint */
static int player_pad_type(void) {
	int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);
	const int type = ids && n > 0 ? (int)SDL_GetGamepadTypeForID(ids[0]) : -1;
	SDL_free(ids);
	return type;
}
static bool steam_deck(void) { const char *d = std::getenv("SteamDeck"); return d && !strcmp(d, "1"); }
/* a touch screen is there (SDL's direct touch devices; RECOMP_OVERLAY_DUMP_TOUCH=1: as if, for a dump) */
static bool touch_screen_present(void) {
	if (std::getenv("RECOMP_OVERLAY_DUMP_TOUCH") && atoi(std::getenv("RECOMP_OVERLAY_DUMP_TOUCH"))) return true;
	int n = 0; bool any = false; SDL_TouchID *t = SDL_GetTouchDevices(&n);
	for (int i = 0; t && i < n; i++) any |= SDL_GetTouchDeviceType(t[i]) == SDL_TOUCH_DEVICE_DIRECT;
	SDL_free(t);
	return any;
}
extern "C" int window_osb_mode(void);   /* (UI-02e: the hint names the off switch while they can show) */
/* (UI-02a, HINT-01) the game's first hint: every way to open the menu, this player's first (menu_hint.h) */
static void menu_hint_body(char *body, size_t n) {
	const bool dump = !g_has_window;   /* (a dump: as a desktop with a keyboard and a mouse) */
	const char *dp = std::getenv("RECOMP_OVERLAY_DUMP_NOPAD");   /* (a dump as on a PC without a controller) */
	const bool pad = SDL_HasGamepad() && !(dp && *dp == '1'), touch = touch_screen_present();
	menu_hint_text(body, n, dump || SDL_HasKeyboard(), dump || SDL_HasMouse(), pad ? player_pad_type() : -1, steam_deck(), touch, touch && !pad && window_osb_mode() != OSB_OFF);
}
static void menu_hint_toast(void) {
	char body[224]; menu_hint_body(body, sizeof body);
	overlay_toast_titled("Menu", body, TOAST_S);
}
/* (HINT-01) a controller connected while the game runs: a short note with its own buttons for the menu (window.c's
 * SDL_EVENT_GAMEPAD_ADDED; the pads already there at the start are announced in the first seconds too -- not those) */
static std::atomic<int> g_pad_hint_req;
static double g_pad_hint_from = -1;   /* (the first windowed frame's time + 3 s) */
extern "C" void overlay_pad_added(void) { g_pad_hint_req = 1; }
static void pad_hint_maybe(double t) {
	if (g_pad_hint_from < 0) g_pad_hint_from = t + 3.0;
	if (!g_pad_hint_req.exchange(0) || t < g_pad_hint_from || g_intro) return;
	char line[96]; menu_hint_pad_line(line, sizeof line, player_pad_type(), steam_deck());
	char body[128]; snprintf(body, sizeof body, "%s opens the menu", line);
	overlay_toast_titled("Controller connected", body, 4.0);
}
/* (UI-02d) while the controller drives the touch screen (the shoulder pair that isn't L and R held: window.c's touch
 * cursor), a badge at the top centre says which panel it is -- the Vita's front touch screen or its rear touch pad --
 * and how to touch; it goes when the pair is let go. RECOMP_OVERLAY_DUMP_UI=touch:front|touch:rear shows it in a dump. */
extern "C" int window_touch_emulation(void);
static int g_force_touch_badge;
static void build_touch_badge(ImDrawList *dl) {
	const int m = g_force_touch_badge ? g_force_touch_badge : g_has_window ? window_touch_emulation() : 0;
	if (m != 1 && m != 2) return;
	const ImVec2 ds = ImGui::GetIO().DisplaySize;
	ImFont *font = ImGui::GetFont(); const float fs = ImGui::GetFontSize() * (ds.x >= 1000 ? 2.0f : 1.0f);
	const PadNames *pn = pad_names_of(pad_family(SDL_HasGamepad() ? player_pad_type() : -1, steam_deck()));
	const char *title = ui_tr(m == 2 ? "touch.badge_rear" : "touch.badge_front");
	char hint[96]; snprintf(hint, sizeof hint, ui_tr("touch.badge_hint"), pn->select);
	const float pad = fs * 0.55f, gap = fs * 0.2f;
	const ImVec2 t1 = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, title), t2 = font->CalcTextSizeA(fs * 0.75f, FLT_MAX, 0.0f, hint);
	const float bw = (t1.x > t2.x ? t1.x : t2.x) + 2 * pad, bh = t1.y + gap + t2.y + 2 * pad;
	const float x = (ds.x - bw) * 0.5f, y = 8 + g_bar_h * g_bar_vis;
	const ImU32 col = m == 2 ? IM_COL32(255, 170, 60, 255) : IM_COL32(80, 200, 255, 255);   /* (the markers' colours: rear orange, front blue) */
	dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + bh), IM_COL32(16, 18, 24, 200), fs * 0.5f);
	dl->AddRect(ImVec2(x, y), ImVec2(x + bw, y + bh), col, fs * 0.5f, 0, fs * 0.08f > 1.5f ? fs * 0.08f : 1.5f);
	dl->AddText(font, fs, ImVec2(x + (bw - t1.x) * 0.5f, y + pad), col, title);
	dl->AddText(font, fs * 0.75f, ImVec2(x + (bw - t2.x) * 0.5f, y + pad + t1.y + gap), IM_COL32(235, 235, 240, 230), hint);
}
static void build_marks(void) {
	ImDrawList *dl = g_intro ? ImGui::GetForegroundDrawList() : ImGui::GetBackgroundDrawList();   /* over the game's frame (LOAD), under the bar and its menus; over the touch card */
	if (g_full) build_perf_full(dl);
	else if (g_nosd) {   /* the overlay: a translucent dark panel at the top-left corner with the lines, as the bitmap version drew it (metrics.c); below the bar while that's down */
		ImFont *font = ImGui::GetFont(); float fs = ImGui::GetFontSize() * 2.0f;   /* the embedded bitmap font at an integer multiple: crisp */
		const float x = 8, y = 8 + g_bar_h * g_bar_vis, pad = 6, lh = fs + 2; float wmax = 0;
		for (int i = 0; i < g_nosd; i++) { ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, g_osd[i]); if (sz.x > wmax) wmax = sz.x; }
		/* the full level adds the frame-time graph under the lines: the last ~1.5 s of game frames, one point per
		 * frame, newest at the right; dim guides at 16.7 ms (60 fps) and 33.3 ms (30 fps); the scale is 50 ms, or
		 * the worst frame shown when that is longer, so a stutter stays inside the panel */
		const float gh = g_nft >= 2 ? fs * 3.0f : 0.0f, gw = wmax;
		dl->AddRectFilled(ImVec2(x, y), ImVec2(x + wmax + 2 * pad, y + g_nosd * lh + 2 * pad + (gh > 0 ? gh + pad : 0)), IM_COL32(0, 0, 0, 170));
		for (int i = 0; i < g_nosd; i++) dl->AddText(font, fs, ImVec2(x + pad, y + pad + i * lh), IM_COL32(255, 255, 255, 255), g_osd[i]);
		if (gh > 0) {
			const float gx = x + pad, gy = y + pad + g_nosd * lh + pad;
			float top = 50.0f; for (int i = 0; i < g_nft; i++) if (g_ft[i] > top) top = g_ft[i];
			for (float guide : {1000.0f / 60.0f, 1000.0f / 30.0f}) {
				const float ly = gy + gh - gh * guide / top;
				dl->AddLine(ImVec2(gx, ly), ImVec2(gx + gw, ly), IM_COL32(255, 255, 255, 60), 1.0f);
			}
			ImVec2 pts[240];
			for (int i = 0; i < g_nft; i++) pts[i] = ImVec2(gx + gw * i / (float)(g_nft - 1), gy + gh - gh * g_ft[i] / top);
			dl->AddPolyline(pts, g_nft, IM_COL32(120, 230, 120, 255), ImDrawFlags_None, 1.5f);
		}
	}
	build_osb(dl);   /* (under the touch markers) */
	build_touch_badge(dl);
	if (g_rear[2] > 0) dl->AddRect(ImVec2(g_rear[0], g_rear[1]), ImVec2(g_rear[0] + g_rear[2], g_rear[1] + g_rear[3]), IM_COL32(255, 170, 60, 140), 0.0f, 2.0f);   /* (rounding 0, thickness 2: ImGui 1.92.8+ argument order -- the old (0, 0, 2.0f) put 2 in `flags`, an ImGui user-error assert) */
	for (int i = 0; i < g_nmarks; i++) {
		const OverlayMark &m = g_marks[i];
		ImU32 col = m.rear ? IM_COL32(255, 170, 60, 230) : IM_COL32(80, 200, 255, 230);   /* rear orange, front blue */
		float r = m.cursor ? 14.f : 10.f;
		if (m.pressed) dl->AddCircleFilled(ImVec2(m.x, m.y), r, (col & 0x00ffffff) | IM_COL32(0, 0, 0, 120));
		dl->AddCircle(ImVec2(m.x, m.y), r, col, 0, 2.5f);
		if (m.cursor) { dl->AddLine(ImVec2(m.x - r - 4, m.y), ImVec2(m.x + r + 4, m.y), col, 1.5f); dl->AddLine(ImVec2(m.x, m.y - r - 4), ImVec2(m.x, m.y + r + 4), col, 1.5f); }
	}
}
/* overlay_modal_open: one of our modal dialogs is up (the menu's "Reset all settings…") -- window.c leaves Esc to
 * it so the dialog cancels instead of the whole menu closing (U6) */
int overlay_modal_open(void) {
	return g_modal_open || g_value_edit;
}
/* overlay_dialog_active: the save-data dialog is waiting on the player (the dialog itself is drawn
 * and answered by savedlg_ui.cpp) */
void overlay_input_lost(void) {
	if (!g_ctx) return;
	ImGuiContext *prev = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(g_ctx);
	savedlg_ui_input_lost();
	ImGui::SetCurrentContext(prev);
}
int overlay_dialog_active(void) {
	if (!g_ctx || !g_has_window) return 0;
	SaveDlgView v; const int k = savedlg_view(&v); savedlg_release();
	if (k == SAVEDLG_LIST || k == SAVEDLG_YESNO) return 1;
	char t[8], x[8]; uint32_t m; int s; return imedlg_view(t, sizeof t, x, sizeof x, &m, &s);   /* (HLE-LIB-05: the IME card) */
}
/* everything the closed state shows, and the bar's state machine (see the file's header comment) */
static void build_closed(void) {
	ImGuiIO &io = ImGui::GetIO();
	const float w = io.DisplaySize.x, hgt = io.DisplaySize.y, dt = io.DeltaTime;
	const double t = ImGui::GetTime();
	g_bar_h = 0;
	const bool popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
	if (g_dismiss_req) {   /* Escape: the menus close, the bar leaves, and the pointer sitting in the top zone doesn't bring it straight back */
		g_dismiss_req = false;
		if (popup_open) ImGui::ClosePopupsExceptModals();
		g_bar_want = false; g_bar_pin_until = -1e9; g_bar_active_t = -1e9; g_hot_since = -1; g_hot_block = true;
	}
	if (g_has_window) {
		pad_hint_maybe(t);   /* (HINT-01) */
		if (!g_toast_checked) {   /* the hint, once per game, on this overlay's first windowed frame */
			g_toast_checked = true;
			if (!settings_has("touch_intro_shown")) {   /* the player's first game: the touch card first (it says where the menu is too) */
				g_intro = true; g_paused = true; apply_pause();   /* (HINT-01: the start hint still comes once, at the next start -- it was marked shown here, and never came) */
			} else if (!settings_has("menu_hint_shown")) { g_toast_until = t + TOAST_S; menu_hint_toast(); settings_set_int("menu_hint_shown", 1); save_settings(); }
		}
		const ImVec2 m = io.MousePos;
		const bool inside = ImGui::IsMousePosValid(&m) && m.x >= 0 && m.x < w && m.y >= 0 && m.y < hgt;   /* (focused but outside the window, e.g. on the title bar: the backend reports a position there too) */
		const float bar_h = ImGui::GetFrameHeight();
		const bool in_hot = inside && m.y <= HOT_PX;
		if (!in_hot) { g_hot_since = -1; g_hot_block = false; }
		else if (g_hot_since < 0) g_hot_since = t;
		if (in_hot && !g_hot_block && t - g_hot_since >= HOT_DWELL_S) { g_bar_want = true; g_bar_active_t = t; }
		const bool over_bar = inside && g_bar_vis > 0 && m.y <= bar_h * g_bar_vis + 2;
		if (over_bar || popup_open || io.WantCaptureMouse || t < g_bar_pin_until) g_bar_active_t = t;
		if (g_bar_want && t - g_bar_active_t > HIDE_S) g_bar_want = false;
		/* the pull tab: with the pointer near the top while the bar is away, and with the hint */
		const bool near_top = inside && m.y < TAB_NEAR_PX;
		const float tab_target = (near_top || t < g_toast_until) && g_bar_vis <= 0.01f ? 1.0f : 0.0f;
		g_tab_a += (tab_target - g_tab_a) * (dt > 0.1f ? 1.0f : dt * 8.0f);
		if ((tab_target == 0.0f && g_tab_a < 0.02f) || g_bar_vis > 0.01f) g_tab_a = 0;   /* (gone at once as the bar arrives: the two never overlap) */
		/* the OS cursor: hidden after a few seconds still over the picture, back the moment it moves */
		const bool moved = m.x != g_last_mouse.x || m.y != g_last_mouse.y; g_last_mouse = m;
		if (moved || ImGui::IsAnyMouseDown()) g_idle_since = t;
		if (g_bar_vis <= 0 && !popup_open && !io.WantCaptureMouse && !g_paused && t - g_idle_since > CURSOR_HIDE_S) ImGui::SetMouseCursor(ImGuiMouseCursor_None);
		if (g_touch_cursor && g_bar_vis <= 0 && !popup_open && !io.WantCaptureMouse) ImGui::SetMouseCursor(ImGuiMouseCursor_None);   /* (the touch mark stands in for it) */
	} else {   /* headless (the dump): no pointer; RECOMP_OVERLAY_DUMP_UI says what to show */
		if (g_force_bar) { g_bar_vis = 1; g_bar_want = true; }
		if (g_force_toast) { g_toast_until = t + TOAST_S; g_tab_a = 1; g_force_toast = false; menu_hint_toast(); overlay_toast_titled("Balance", "[A] [D] tilt left / right", TOAST_S); }
	}
	{	/* the slide, at a fixed rate either way */
		const float target = g_bar_want ? 1.0f : 0.0f, step = dt > 0.1f ? 1.0f : dt / SLIDE_S;
		g_bar_vis += target > g_bar_vis ? fminf(step, target - g_bar_vis) : -fminf(step, g_bar_vis - target);
	}
	if (g_has_window) {   /* the shader prep (gfx_gpu.c): the card when it asks (after the touch card), the corner line while skipped, a toast when done */
		int done, total; double eta; const int ps = gfx_gpu_shader_prep(&done, &total, &eta);
		if (g_layout_ask && !g_layout_card && !g_intro && !g_open) { g_layout_card = true; g_open = true; apply_pause(); }   /* (the game waits for it: before the prep card) */
		else if (ps == 1 && !g_intro && !g_open && !g_layout_card) { g_prep_card = true; g_open = true; apply_pause(); }
		else if (ps == 2 && total > 0) build_prep_corner(w, hgt, done, total);
		else if (ps == 3) overlay_toast_titled("Shaders ready", "Built for this computer: no more pauses to build them.", 4.0);
	}
	if (g_bar_vis > 0) build_bar(w);
	if (g_tab_a > 0) build_tab(w, t);
	if (g_intro) build_intro();   /* (the touch card in place of the pause banner; the markers drawn over it) */
	build_marks();
	if (g_has_window) savedlg_ui_build(w, hgt);
	if (g_has_window) imedlg_ui_build(w, hgt);   /* (HLE-LIB-05) */
	if (g_paused && !g_intro) build_pause_banner(w, hgt);
	toasts_take(t);
	if (g_ntoasts) build_toasts(w, hgt, t);
}

/* shared by overlay_frame, overlay_marks and overlay_render_forced: builds one ImGui frame and renders
 * it into `target` with LOAD (never CLEAR -- the game's own frame, or the dump path's copy of it, is
 * already there and must show through everywhere the overlay doesn't draw). `have_window` picks
 * whether the SDL3 platform backend supplies DisplaySize/DeltaTime/mouse (interactive) or this
 * function sets them itself from the caller's w/h/dt_s (headless: see this file's own header
 * comment on the render paths). A frame that drew nothing (the bar away, no HUD) costs no render pass. */
static void render_frame(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, uint32_t w, uint32_t h, double dt_s, bool have_window, void (*build)(void) = build_ui) {
	if (!g_ctx || !w || !h) return;
	ImGui::SetCurrentContext(g_ctx);
	ImGuiIO &io = ImGui::GetIO();
	/* keyboard and gamepad navigation only while the full menu is up: the bar is pointer-only, and while
	 * it's closed a game's keys and pad are the game's (window.c forwards only mouse events then) */
	io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
	if (g_open) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
	ImGui_ImplSDLGPU3_NewFrame();
	if (have_window) {
		ImGui_ImplSDL3_NewFrame();
		savedlg_ui_gamepad_frame(SDL_HasGamepad());   /* (U12c) after the backend cleared the flag: a pad keeps it */
	} else {
		io.DisplaySize = ImVec2((float)w, (float)h);
		io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
		io.DeltaTime = dt_s > 0 ? (float)dt_s : 1.0f / 60.0f;
	}
	{	/* the scale for this window (ui_scale.h): from the base style each time it changes, before NewFrame lays out
		 * anything; sizes in the code are in em (GetFontSize), so they follow the font */
		const float dpi = have_window && g_window ? SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(g_window)) : 1.0f;
		const float k = overlay_ui_scale(io.DisplaySize.y, dpi);
		if (std::fabs(k - g_ui_scale) > 0.005f) {
			ImGuiStyle &st = ImGui::GetStyle();
			st = g_base_style; st.ScaleAllSizes(k); st.FontSizeBase = g_base_style.FontSizeBase * k;   /* (not FontScaleDpi: PushFont(nullptr, em * x) passes sizes already in em, which FontScaleDpi would scale a second time) */
			g_ui_scale = k;
		}
	}
	ImGui::NewFrame();
	g_mouse_captured = io.WantCaptureMouse;   /* (computed by NewFrame from where the pointer was: the bar, a menu, the tab) */
	build();
	ImGui::Render();
	ImDrawData *dd = ImGui::GetDrawData();
	if (g_skip_draw || dd->DisplaySize.x <= 0 || dd->DisplaySize.y <= 0 || dd->TotalVtxCount == 0) return;
	/* RECOMP_OVERLAY_DEBUG=1: what ImGui handed the backend this frame -- per draw command, the vertices it
	 * indexes: their position box, how many are not finite, the first one's colour (a verification aid
	 * for the headless dump, where the picture can't be watched being drawn) */
	static int dbg = -1; if (dbg < 0) dbg = std::getenv("RECOMP_OVERLAY_DEBUG") != nullptr;
	if (dbg) {
		std::fprintf(stderr, "    overlay frame: %d lists, %d vtx, %d idx\n", dd->CmdListsCount, dd->TotalVtxCount, dd->TotalIdxCount);
		for (int l = 0; l < dd->CmdListsCount; l++) {
			const ImDrawList *dl = dd->CmdLists[l];
			for (int c = 0; c < dl->CmdBuffer.Size; c++) {
				const ImDrawCmd &pc = dl->CmdBuffer[c];
				if (!pc.ElemCount) continue;
				float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX; int bad = 0;
				for (unsigned i = 0; i < pc.ElemCount; i++) {
					const ImDrawVert &v = dl->VtxBuffer[dl->IdxBuffer[pc.IdxOffset + i] + pc.VtxOffset];
					if (!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y) || !std::isfinite(v.uv.x) || !std::isfinite(v.uv.y)) { bad++; continue; }
					x0 = fminf(x0, v.pos.x); y0 = fminf(y0, v.pos.y); x1 = fmaxf(x1, v.pos.x); y1 = fmaxf(y1, v.pos.y);
				}
				const ImDrawVert &v0 = dl->VtxBuffer[dl->IdxBuffer[pc.IdxOffset] + pc.VtxOffset];
				std::fprintf(stderr, "      list %d '%s' cmd %d: %u idx (vtx off %u, idx off %u) clip (%.0f,%.0f)-(%.0f,%.0f) pos box (%.1f,%.1f)-(%.1f,%.1f) nonfinite %d first col %08x uv (%.4f,%.4f) texdata %p (status %d, %dx%d)\n",
					l, dl->_OwnerName ? dl->_OwnerName : "?", c, pc.ElemCount, pc.VtxOffset, pc.IdxOffset, pc.ClipRect.x, pc.ClipRect.y, pc.ClipRect.z, pc.ClipRect.w, x0, y0, x1, y1, bad, v0.col, v0.uv.x, v0.uv.y,
					(void *)pc.TexRef._TexData, pc.TexRef._TexData ? (int)pc.TexRef._TexData->Status : -1, pc.TexRef._TexData ? pc.TexRef._TexData->Width : 0, pc.TexRef._TexData ? pc.TexRef._TexData->Height : 0);
			}
		}
	}
	/* mandatory before the render pass below: uploads the vertex/index buffers. It also acquires and
	 * submits a command buffer of its own whenever ImGui has a texture to create or update (the font
	 * atlas: the first frame, and any frame a new glyph is rasterised), with SDL's plain
	 * SDL_SubmitGPUCommandBuffer -- the backend is third_party code built without gpulock.h's renames
	 * -- so the whole call runs under the host's submit lock, as every submit must (gpulock.h) */
	gpu_lock(); ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd); gpu_unlock();
	SDL_GPUColorTargetInfo ti = {};
	ti.texture = target; ti.load_op = SDL_GPU_LOADOP_LOAD; ti.store_op = SDL_GPU_STOREOP_STORE; ti.cycle = false;
	SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ti, 1, nullptr);
	ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd, rp);
	SDL_EndGPURenderPass(rp);
}
/* ---- the touch card (this file's header): the three ways to touch, the device last used highlighted */
enum { DEV_KBM, DEV_PAD, DEV_TOUCH };
static int g_intro_dev = -1; static double g_intro_done = -1;   /* the device highlighted (-1: not yet chosen); when the touch was made */
static double g_intro_since = -1;   /* (INTRO-01) when the card first showed: it ends by itself after INTRO_SKIP_S */
static double g_intro_at = -1;   /* headless (RECOMP_OVERLAY_DUMP_UI=intro@<seconds>): the moment of the loops drawn */
extern "C" int window_touch_contacts(int port, int *keys, int *xs, int *ys, int max);
/* the card's input icons: Kenney's Input Prompts (CC0), third_party/kenney-prompts/make.py's ICONS in this order, white
 * masks the card tints. Each pad family has seven from P_PAD: the left and right bumpers held and up, the south button
 * pressed and up, the left stick */
enum { P_OPTION, P_OPTION_UP, P_ALT, P_ALT_UP, P_MOUSE, P_MOUSE_LEFT, P_MOUSE_RIGHT, P_FINGER, P_PAD, P_N = P_PAD + 3 * 7 };
enum { PAD_L, PAD_L_UP, PAD_R, PAD_R_UP, PAD_A, PAD_A_UP, PAD_STICK };
enum { FAM_PS, FAM_XBOX, FAM_SWITCH };
static const int P_COLS = 8;
static ImFontAtlasRectId g_prompts[3] = {ImFontAtlasRectId_Invalid, ImFontAtlasRectId_Invalid, ImFontAtlasRectId_Invalid};   /* 128, 64 and 32 pixels a cell */
static void prompts_load(void) {   /* once, in a frame: the masks into ImGui's own font texture, which every path already draws from */
	static bool tried; if (tried) return; tried = true;
	int w, h, n; unsigned char *px = stbi_load_from_memory(prompts_png, (int)sizeof prompts_png, &w, &h, &n, 1);
	if (!px) return;
	ImFontAtlas *atlas = ImGui::GetIO().Fonts;
	for (int l = 0; l < 3; l++) {
		if (l) {   /* each level half the one before, a pixel the mean of four (in place: a write never passes a read) */
			for (int y = 0; y < h / 2; y++) for (int x = 0; x < w / 2; x++) { const unsigned char *s = px + 2 * y * w + 2 * x; px[y * (w / 2) + x] = (unsigned char)((s[0] + s[1] + s[w] + s[w + 1] + 2) / 4); }
			w /= 2; h /= 2;
		}
		ImFontAtlasRect r; g_prompts[l] = atlas->AddCustomRect(w, h, &r);   /* (queues its upload: the pixels are written before the frame renders) */
		if (g_prompts[l] == ImFontAtlasRectId_Invalid) break;
		ImTextureData *tex = atlas->TexData;   /* (after the add: it may have grown the texture) */
		ImFontAtlasTextureBlockConvert(px, ImTextureFormat_Alpha8, w, (unsigned char *)tex->GetPixelsAt(r.x, r.y), tex->Format, tex->GetPitch(), w, h);
	}
	stbi_image_free(px);
}
static void prompt(ImDrawList *dl, int k, ImVec2 c, float size, ImU32 col) {   /* icon k, size across, centred on c */
	const float fb = ImGui::GetIO().DisplayFramebufferScale.y, px = size * fb;
	int l = px > 70 ? 0 : px > 35 ? 1 : 2;   /* the smallest level about as big as the size drawn: drawn down, sharp, never up */
	while (l > 0 && g_prompts[l] == ImFontAtlasRectId_Invalid) l--;
	ImFontAtlas *atlas = ImGui::GetIO().Fonts; ImFontAtlasRect r;
	if (g_prompts[l] == ImFontAtlasRectId_Invalid || !atlas->GetCustomRect(g_prompts[l], &r)) return;
	const int rows = (P_N + P_COLS - 1) / P_COLS;
	const ImVec2 d((r.uv1.x - r.uv0.x) / P_COLS, (r.uv1.y - r.uv0.y) / rows), uv(r.uv0.x + k % P_COLS * d.x, r.uv0.y + k / P_COLS * d.y);
	const ImVec2 p(roundf((c.x - size * 0.5f) * fb) / fb, roundf((c.y - size * 0.5f) * fb) / fb);   /* (on whole pixels: sharp) */
	dl->AddImage(atlas->TexRef, p, ImVec2(p.x + size, p.y + size), uv, ImVec2(uv.x + d.x, uv.y + d.y), col);
}
static void pad_names(const char **l, const char **r, const char **a, bool *touchpad, int *fam) {   /* the first pad's own button names and icons */
	*l = "L1"; *r = "R1"; *a = "Cross"; *touchpad = true; *fam = FAM_PS;   /* (the Vita's own names, and a DualShock 4's or DualSense's: unless the pad says otherwise) */
	int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);
	if (ids && n > 0) {
		const SDL_GamepadType t = SDL_GetGamepadTypeForID(ids[0]);
		if (t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE) { *l = "LB"; *r = "RB"; *a = "A"; *touchpad = false; *fam = FAM_XBOX; }
		else if (t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) { *l = "L"; *r = "R"; *a = "B"; *touchpad = false; *fam = FAM_SWITCH; }
		else if (t == SDL_GAMEPAD_TYPE_PS3) *touchpad = false;
	}
	SDL_free(ids);
}
static float ease(float x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return x * x * (3 - 2 * x); }
/* one device's demonstration at p (its top-left), w x h: its buttons doing the touch, beside a small Vita screen whose
 * marker is the one play draws (build_marks: front blue, rear orange). Played on a loop for the highlighted device,
 * the others at rest with their buttons down; the keyboard's and the pad's loops alternate the front and the rear */
static void intro_demo(ImDrawList *dl, int dev, bool on, int fam, ImVec2 p, float w, float h, double t) {
	const float em = ImGui::GetFontSize(), H = h * 0.74f, sw = h * 1.12f, sh = sw * 544 / 960;
	const ImVec2 s0(p.x + w - sw, p.y + (h - sh) * 0.5f - em * 0.35f), s1(s0.x + sw, s0.y + sh);   /* the screen; its caption below */
	const ImU32 idle = on ? ui::u32(ui::text, 0.92f) : ui::u32(ui::muted, 0.7f), lit = on ? ui::u32(ui::accent) : ui::u32(ui::text, 0.6f);
	const float cy = (s0.y + s1.y) * 0.5f, step = H * 0.8f;   /* the icons' centre line, the screen's; their spacing (cells have margins) */
	const double period = dev == DEV_PAD ? 3.6 : dev == DEV_KBM ? 3.0 : 2.0;
	const float u = on ? (float)fmod(t, period) : dev == DEV_PAD ? 2.1f : dev == DEV_KBM ? 1.3f : 0.7f;   /* (at rest: the moment the touch lands) */
	const bool rear = on && dev != DEV_TOUCH && ((long)(t / period) & 1);
	dl->AddRectFilled(s0, s1, ui::u32(ui::bg, 0.85f), em * 0.25f);
	dl->AddRect(s0, s1, on ? ui::u32(ui::text, 0.35f) : ui::u32(ui::muted, 0.3f), em * 0.25f, 0, 1.0f);
	if (dev != DEV_TOUCH) {
		const char *cap = rear ? "rear" : "front"; const float fs = em * 0.78f;
		const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, cap);
		dl->AddText(ImGui::GetFont(), fs, ImVec2((s0.x + s1.x - ts.x) * 0.5f, s1.y + em * 0.15f), ui::u32(ui::muted, on ? 1.0f : 0.7f), cap);
	}
	ImVec2 m(s0.x + sw * 0.62f, s0.y + sh * 0.45f); bool shown = false, pressed = false, cursor = false; float since = -1;   /* the marker; seconds since it pressed */
	const float ex = s0.x - H * 0.3f;   /* where the icons end */
	if (dev == DEV_KBM) {
#ifdef __APPLE__
		const int key = P_OPTION;
#else
		const int key = P_ALT;
#endif
		const bool held = u >= 0.35f && u < 2.5f, click = held && u >= 1.2f && u < 1.55f;
		const ImVec2 kc(ex - step - H * 0.5f, cy + (held ? H * 0.03f : 0)), mc(ex - H * 0.5f, cy);
		prompt(dl, held ? key : key + 1, kc, H, held ? lit : idle);
		prompt(dl, P_MOUSE, mc, H, idle);
		if (click) prompt(dl, rear ? P_MOUSE_RIGHT : P_MOUSE_LEFT, mc, H, lit);
		if (held) { shown = true; const float k = ease((u - 0.35f) / 0.75f); m = ImVec2(s0.x + sw * (0.3f + 0.32f * k), s0.y + sh * (0.62f - 0.17f * k)); }
		pressed = click; since = u >= 1.2f && held ? u - 1.2f : -1;
	} else if (dev == DEV_PAD) {
		const int base = P_PAD + fam * 7;
		const bool held = u >= 0.3f && u < 3.0f, press = held && u >= 2.0f && u < 2.35f;
		const float mv = u >= 0.7f && u < 1.7f ? sinf((u - 0.7f) * 3.14159f) : 0;   /* the stick pushed, up and to the right, and let go */
		const ImVec2 bc(ex - 2 * step - H * 0.5f, cy + (held ? H * 0.03f : 0)), sc(ex - step - H * 0.5f + mv * H * 0.07f, cy - mv * H * 0.04f), ac(ex - H * 0.5f, cy);
		prompt(dl, base + (rear ? (held ? PAD_R : PAD_R_UP) : (held ? PAD_L : PAD_L_UP)), bc, H, held ? lit : idle);
		prompt(dl, base + PAD_STICK, sc, H, idle);
		prompt(dl, base + (press ? PAD_A : PAD_A_UP), ac, press ? H * 0.92f : H, press ? lit : idle);
		if (held) { shown = cursor = true; const float k = ease((u - 0.7f) / 1.0f); m = ImVec2(s0.x + sw * (0.5f + 0.14f * k), s0.y + sh * (0.5f - 0.08f * k)); }
		pressed = press; since = held && u >= 2.0f ? u - 2.0f : -1;
	} else {   /* a finger over the screen, coming down onto it */
		const float down = u < 0.6f ? ease(u / 0.6f) : u < 0.95f ? 1 : 1 - ease((u - 0.95f) / 0.4f);
		pressed = u >= 0.6f && u < 0.95f; shown = pressed; since = u >= 0.6f ? u - 0.6f : -1;
		const float fh = H * 1.45f * (1.12f - 0.12f * down);   /* (the finger's icon is small in its cell; it shrinks as it comes down) */
		prompt(dl, P_FINGER, ImVec2(m.x + fh * 0.02f, m.y + fh * 0.07f + (1 - down) * H * 0.18f), fh, idle);   /* (the fingertip sits at 0.48, 0.43 of the cell) */
	}
	const ImU32 mc = rear ? IM_COL32(255, 170, 60, 230) : IM_COL32(80, 200, 255, 230);
	const float r = sh * (cursor ? 0.15f : 0.11f);
	if (on && since >= 0 && since < 0.6f) dl->AddCircle(m, r * (1 + since * 3.0f), (mc & 0x00ffffff) | ((ImU32)(200 * (1 - since / 0.6f)) << 24), 0, 1.5f);   /* the touch's ripple */
	if (shown) {
		if (pressed) dl->AddCircleFilled(m, r, (mc & 0x00ffffff) | IM_COL32(0, 0, 0, 120));
		dl->AddCircle(m, r, mc, 0, 1.6f);
		if (cursor) { dl->AddLine(ImVec2(m.x - r * 1.5f, m.y), ImVec2(m.x + r * 1.5f, m.y), mc, 1.0f); dl->AddLine(ImVec2(m.x, m.y - r * 1.5f), ImVec2(m.x, m.y + r * 1.5f), mc, 1.0f); }
	}
}
static void build_intro(void) {
	const double t = g_intro_at >= 0 ? g_intro_at : ImGui::GetTime();
	prompts_load();
	if (g_has_window && (g_intro_dev < 0 || g_intro_done < 0)) g_intro_dev = window_kbm_active() ? DEV_KBM : DEV_PAD;   /* (the device last used, as play sees it; headless, the one asked for) */
	if (g_intro_done < 0 && g_has_window) {   /* a real touch on either panel, through the in-game path: done (its input says which device) */
		int keys[8], xs[8], ys[8];
		for (int port = 0; port < 2 && g_intro_done < 0; port++) {
			const int n = window_touch_contacts(port, keys, xs, ys, 8);
			if (n > 0) { const int k = keys[0]; g_intro_dev = k == 1 ? DEV_KBM : k >= 20 ? DEV_TOUCH : (k >= 4 && k <= 6) ? (window_kbm_active() ? DEV_KBM : DEV_PAD) : DEV_PAD; g_intro_done = t; }
		}
	} else if (g_has_window && g_intro_done >= 0 && t - g_intro_done > 1.2) intro_end();   /* (a moment to see it worked, then the game) */
	if (g_has_window && g_intro_at < 0) {   /* (INTRO-01) untouched: it ends by itself */
		if (g_intro_since < 0) g_intro_since = t;
		if (g_intro_done < 0 && intro_timed_out(g_intro_since, t)) { intro_end(); return; }
	}
	const double left = g_intro_since >= 0 ? INTRO_SKIP_S - (t - g_intro_since) : INTRO_SKIP_S;
	const char *pl, *pr, *pa; bool tp; int fam; pad_names(&pl, &pr, &pa, &tp, &fam);
#ifdef __APPLE__
	const char *alt = "Option";
#else
	const char *alt = "Alt";
#endif
	char kbm[200], padb[240];
	snprintf(kbm, sizeof kbm, "Hold %s and click to touch the screen. Right-click instead to touch the rear pad.", alt);
	snprintf(padb, sizeof padb, "Hold %s and steer the cursor with a stick, then press %s to touch. %s does the same on the rear pad.%s", pl, pa, pr, tp ? " The touchpad works too." : "");
	const struct { const char *name, *how; } devs[3] = {{"With keyboard and mouse", kbm}, {"With a controller", padb}, {"On a touch screen", "Touch the picture, as you would the Vita's own screen."}};   /* (INTRO-01: "With ..." -- three ways to touch, not a choice of what to play with) */
	const ImGuiViewport *vp = ImGui::GetMainViewport(); const float em = ImGui::GetFontSize();
	ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), ui::u32(ui::bg, 0.55f));   /* (the paused game, dimmed behind it) */
	const float cw = fminf(vp->Size.x - 2 * em, 38 * em);
	ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(cw, 0), ImVec2(cw, vp->Size.y - 2 * em));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.4f, em * 1.2f));
	ImGui::Begin("##touchcard", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);   /* (a click anywhere, the card too, is a touch) */
	ui::heading(g_intro_done < 0 ? "How to touch the screen" : "That's it", em * 1.35f);
	ui::muted_wrapped("Vita games are played with the touch screen as well as buttons. Here's how to touch it:");
	ImGui::Dummy(ImVec2(0, em * 0.2f));
	const float dh = em * 3.8f, dw = dh * 3.3f;   /* each row's demonstration strip (intro_demo), at its right */
	for (int d = 0; d < 3; d++) {
		const bool on = d == g_intro_dev;
		ImGui::PushStyleColor(ImGuiCol_ChildBg, on ? ui::with_alpha(ui::accent, 0.13f) : ui::card);
		ImGui::PushStyleColor(ImGuiCol_Border, on ? ui::with_alpha(ui::accent, 0.55f) : ui::border);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, em * 0.4f);
		char id[16]; snprintf(id, sizeof id, "##dev%d", d);
		ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
		const ImVec2 at = ImGui::GetCursorScreenPos(); const float avail = ImGui::GetContentRegionAvail().x;
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail - dw - em);
		ImGui::BeginGroup();
		ImGui::PushStyleColor(ImGuiCol_Text, on ? ui::accent : ui::text); ImGui::TextUnformatted(devs[d].name); ImGui::PopStyleColor();
		ImGui::PushStyleColor(ImGuiCol_Text, on ? ui::text : ui::muted); ImGui::TextWrapped("%s", devs[d].how); ImGui::PopStyleColor();
		ImGui::EndGroup(); ImGui::PopTextWrapPos();
		const float th = ImGui::GetItemRectSize().y;
		if (th < dh) ImGui::Dummy(ImVec2(0, fmaxf(0, dh - th - ImGui::GetStyle().ItemSpacing.y)));
		intro_demo(ImGui::GetWindowDrawList(), d, on, fam, ImVec2(at.x + avail - dw, at.y + (fmaxf(th, dh) - dh) * 0.5f), dw, dh, t);
		ImGui::EndChild();
		ImGui::PopStyleVar(); ImGui::PopStyleColor(2);
	}
	ImGui::Dummy(ImVec2(0, em * 0.2f));
	if (g_intro_done < 0) {
		char go[96]; snprintf(go, sizeof go, "Try a touch now, or press any key or button to start playing (by itself in %d s).", (int)ceil(left > 0 ? left : 0));
		ImGui::PushStyleColor(ImGuiCol_Text, ui::accent); ImGui::TextWrapped("%s", go); ImGui::PopStyleColor();
	} else ImGui::TextUnformatted("Back to the game.");
	ui::muted_wrapped("You don't choose anything here: every way works whenever you like.");
	{	/* (HINT-01) every way to open the menu, as the start hint says it -- the ways this player has now first */
		char body[224]; menu_hint_body(body, sizeof body);
		ImGui::Dummy(ImVec2(0, em * 0.15f));
		ImGui::TextUnformatted("The menu, with every control:");
		for (char *l = body; l && *l;) {
			char *nl = strchr(l, '\n'); if (nl) *nl = 0;
			std::string plain; for (const char *c = l; *c; c++) if (*c != '[' && *c != ']') plain += *c;   /* (no key caps here) */
			ImGui::Bullet(); ui::muted_wrapped(plain.c_str());
			l = nl ? nl + 1 : nullptr;
		}
	}
	ImGui::End();
	ImGui::PopStyleVar();
}
/* ---- the shader prep card and its corner line (this file's header) */
static void fmt_count(char *out, size_t n, int v) {   /* 5837 -> "5,837" */
	char d[16]; snprintf(d, sizeof d, "%d", v); const int len = (int)strlen(d); size_t o = 0;
	for (int i = 0; i < len && o + 1 < n; i++) { if (i && (len - i) % 3 == 0 && o + 1 < n) out[o++] = ','; out[o++] = d[i]; }
	out[o] = 0;
}
static void build_prep_corner(float w, float hgt, int done, int total) {
	char line[64]; snprintf(line, sizeof line, "Building shaders %d%%", total ? (int)(100.0 * done / total) : 0);
	ImFont *font = ImGui::GetFont(); const float fs = ImGui::GetFontSize(), pad = fs * 0.5f;
	const ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, line);
	const ImVec2 p0(w - sz.x - 3 * pad, hgt - sz.y - 3 * pad), p1(w - pad, hgt - pad);
	ImDrawList *dl = ImGui::GetBackgroundDrawList();   /* (under the bar and its menus, like the HUD) */
	dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 150), fs * 0.3f);
	dl->AddText(font, fs, ImVec2(p0.x + pad, p0.y + pad * 0.6f), IM_COL32(220, 220, 228, 255), line);
	const float bx0 = p0.x + pad, bx1 = p1.x - pad, by = p1.y - pad * 0.7f;
	dl->AddLine(ImVec2(bx0, by), ImVec2(bx1, by), IM_COL32(255, 255, 255, 50), 2.0f);
	if (total) dl->AddLine(ImVec2(bx0, by), ImVec2(bx0 + (bx1 - bx0) * done / total, by), ui::u32(ui::accent, 1.0f), 2.0f);
}
static void build_prep(void) {
	int done, total; double eta; int ps = 1;
	if (g_prep_demo[0] >= 0) { done = g_prep_demo[0]; total = g_prep_demo[1]; eta = g_prep_demo[2]; }
	else ps = gfx_gpu_shader_prep(&done, &total, &eta);
	if (ps != 1) {   /* the builds are done (or the card was never meant): back to the game */
		g_prep_card = false; g_open = false; g_paused = false; apply_pause();
		if (ps == 3) overlay_toast_titled("Shaders ready", "Built for this computer: no more pauses to build them.", 4.0);
		return;
	}
	const ImGuiViewport *vp = ImGui::GetMainViewport(); const float em = ImGui::GetFontSize();
	ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), ui::u32(ui::bg, 0.78f));   /* (the paused game, dimmed behind it) */
	const float cw = fminf(vp->Size.x - 2 * em, 30 * em);
	ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(cw, 0), ImVec2(cw, vp->Size.y - 2 * em));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.4f, em * 1.2f));
	ImGui::Begin("##prepcard", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
	ui::heading("Preparing shaders", em * 1.35f);
	const char *game = std::getenv("RECOMP_GAME_NAME"); char body[256];
	snprintf(body, sizeof body, "Building %s's shaders for this computer, so it plays without pauses. This happens once: later starts are quick.", game && *game ? game : "the game");
	ImGui::TextWrapped("%s", body);
	ImGui::Dummy(ImVec2(0, em * 0.3f));
	char a[16], b[16], over[48]; fmt_count(a, sizeof a, done); fmt_count(b, sizeof b, total); snprintf(over, sizeof over, "%s of %s", a, b);
	ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ui::accent);
	ImGui::ProgressBar(total ? (float)done / (float)total : 0.0f, ImVec2(-1, em * 1.4f), over);
	ImGui::PopStyleColor();
	char left[64];
	if (eta < 0) snprintf(left, sizeof left, "Working out the time left");
	else if (eta < 60) snprintf(left, sizeof left, "Less than a minute left");
	else snprintf(left, sizeof left, "About %d minute%s left", (int)ceil(eta / 60), eta > 60 ? "s" : "");
	ui::muted_wrapped(left);
	ImGui::Dummy(ImVec2(0, em * 0.4f));
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	if (ImGui::Button("Skip")) overlay_close();   /* (closing is Skip: overlay_close) */
	ImGui::Dummy(ImVec2(0, em * 0.2f));
	ui::muted_wrapped("Skipping starts the game now. You may see short pauses until the shaders are built: they keep building while you play.");
	ImGui::End();
	ImGui::PopStyleVar();
}
/* ---- the layout card (overlay_layout_ask): Killzone 1.12 decides at boot whether it plays as a PS TV (every touch action on
 * buttons and sticks, a controller only) or a handheld (touch, with keyboard, mouse or a controller's touch cursor), and
 * keeps it. Touch is the default; the PS TV layout is the player's choice, offered before the title */
static void layout_choose(int v) {
	if (g_layout_remember) { settings_set_int("controller_layout", v); save_settings(); }
	g_layout_card = false; g_layout_ask = 0; g_layout_answer = v;
	g_open = false; g_paused = false; apply_pause();
}
static void build_layout(void) {
	const ImGuiViewport *vp = ImGui::GetMainViewport(); const float em = ImGui::GetFontSize();
	ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), ui::u32(ui::bg, 0.85f));
	const float cw = fminf(vp->Size.x - 2 * em, 34 * em);
	ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(cw, 0), ImVec2(cw, vp->Size.y - 2 * em));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.4f, em * 1.2f));
	ImGui::Begin("##layoutcard", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
	ui::heading("Controller layout", em * 1.35f);
	const char *game = std::getenv("RECOMP_GAME_NAME"); char body[320];
	snprintf(body, sizeof body, "A controller is connected. %s can also play with its own PS TV layout, which puts every touch action on buttons and sticks. It keeps the layout it starts with until you quit.", game && *game ? game : "This game");
	ImGui::TextWrapped("%s", body);
	ImGui::Dummy(ImVec2(0, em * 0.4f));
	const float bw = (ImGui::GetContentRegionAvail().x - em * 0.6f) * 0.5f;
	int chosen = 0;
	ImGui::BeginGroup();
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	if (ui::primary_button("Standard##lc", ImVec2(bw, em * 2.4f))) chosen = 1;
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw); ui::muted_wrapped("The game as on a handheld. Keyboard, mouse and controller (its touch cursor) all work."); ImGui::PopTextWrapPos();
	ImGui::EndGroup();
	ImGui::SameLine(0, em * 0.6f);
	ImGui::BeginGroup();
	if (ImGui::Button("PS TV##lc", ImVec2(bw, em * 2.4f))) chosen = 2;
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw); ui::muted_wrapped("Controller only: every action on buttons and sticks. Keyboard and mouse don't play the game in it."); ImGui::PopTextWrapPos();
	ImGui::EndGroup();
	ImGui::Dummy(ImVec2(0, em * 0.4f));
	ImGui::Checkbox("Remember my choice##lc", &g_layout_remember);
	ui::muted_wrapped("Change it later in the menu: Game > Controller layout.");
	ImGui::End();
	ImGui::PopStyleVar();
	if (chosen) layout_choose(chosen);
}
extern "C" int overlay_layout_ask(void) {   /* (the game's thread) */
	if (!g_ctx || !g_has_window) return 1;
	g_layout_answer = 0; g_layout_ask = 1;
	int a; while (!(a = g_layout_answer.load())) { struct timespec ts = {0, 10000000}; nanosleep(&ts, nullptr); }
	return a;
}
/* ---- the quit card (this file's header) */
static void build_quit(void) {
	const ImGuiViewport *vp = ImGui::GetMainViewport(); const float em = ImGui::GetFontSize();
	ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), ui::u32(ui::bg, 0.55f));
	const char *game = std::getenv("RECOMP_GAME_NAME"); char title[160];
	snprintf(title, sizeof title, "Quit %s?", game && *game ? game : "the game");
	ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(fminf(vp->Size.x - 2 * em, 24 * em), 0), ImVec2(fminf(vp->Size.x - 2 * em, 24 * em), vp->Size.y));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.4f, em * 1.2f));
	ImGui::Begin("##quitcard", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
	ui::heading(title, em * 1.35f);
	ui::muted_wrapped("Progress since your last save will be lost.");
	ImGui::Dummy(ImVec2(0, em * 0.3f));
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	if (ui::primary_button("Keep playing")) overlay_close();
	ImGui::SameLine();
	ImGui::PushStyleColor(ImGuiCol_Text, ui::error);
	if (ImGui::Button(quit_label())) g_want_quit = true;
	ImGui::PopStyleColor();
	ImGui::End();
	ImGui::PopStyleVar();
}
void overlay_frame(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, uint32_t w, uint32_t h, double dt_s) {
	if (!overlay_is_open()) return;
	render_frame(cmd, target, w, h, dt_s, g_has_window, g_quit_card ? build_quit : g_layout_card ? build_layout : g_prep_card ? build_prep : build_ui);
}
void overlay_marks(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, uint32_t w, uint32_t h, double dt_s, const OverlayMark *marks, int n, const float rear_rect[4]) {
	if (overlay_is_open()) return;   /* the menu has its own fps line (page_performance) */
	g_nosd = metrics_osd_lines(g_osd, 7);
	g_nft = g_nosd > 1 ? metrics_frame_times(g_ft, 240) : 0;   /* more than one line: the full level */
	g_full = metrics_osd_level() == 2; if (g_full) metrics_snapshot(&g_snap);
	/* headless (the dump's HUD check): only with something to show. Windowed: every present -- the bar's
	 * hover and timers, the cursor and the capture flag live in this frame (a frame that draws nothing is
	 * a few microseconds of CPU and no render pass: render_frame) */
	if (!g_has_window && !n && !g_nosd && !(rear_rect && rear_rect[2] > 0)) return;
	g_marks = marks; g_nmarks = n;
	for (int i = 0; i < 4; i++) g_rear[i] = rear_rect ? rear_rect[i] : 0;
	render_frame(cmd, target, w, h, dt_s, g_has_window, build_closed);
}
/* RECOMP_OVERLAY_DUMP (window.c): deliberately bypasses overlay_is_open() -- this must never be the
 * thing that makes the rest of the host think the menu is open (display.c's pause gate, window.c's
 * input routing and audio mute all key off that flag, not off anything here). `ui` (overlay.h): NULL
 * or "menu" the full menu; "bar" the menu bar down; "Game"/"View"/"Controls"/"Help" the bar with that
 * menu open; "toast" the first-launch hint and the pull tab. */
/* one headless frame of `build`, on a command buffer of its own when drawn (the SDLGPU3 backend keeps one
 * vertex/index buffer set and one texture-upload path per frame: one drawn frame per command buffer, as
 * the interactive present loop does, keeps a later frame's uploads out of an earlier frame's draws) */
static void forced_frame(SDL_GPUDevice *device, SDL_GPUTexture *target, uint32_t w, uint32_t h, bool draw, void (*build)(void)) {
	g_skip_draw = !draw;
	SDL_GPUCommandBuffer *cmd = draw ? SDL_AcquireGPUCommandBuffer(device) : nullptr;
	if (draw && !cmd) { g_skip_draw = false; return; }
	render_frame(cmd, target, w, h, 0, false, build);
	if (cmd) SDL_SubmitGPUCommandBuffer(cmd);
	g_skip_draw = false;
}
void overlay_render_forced(SDL_GPUDevice *device, SDL_GPUTexture *target, uint32_t w, uint32_t h, const char *ui) {
	if (!g_ctx || !device) return;
	g_nmarks = 0; g_rear[2] = 0; g_nosd = 0; g_nft = 0; g_full = false;
	if (!ui || !*ui || !strncmp(ui, "menu", 4)) {
		/* "menu:<tab>" opens the full menu on that page (the tab bar applies a selection on its next layout) */
		if (ui && ui[4] == ':') for (int i = 0; i < TAB_N; i++) if (!strcmp(ui + 5, g_tab_names[i])) g_select_tab = i;
		/* three frames, the last one drawn: a window ImGui has never seen before (every window here, this
		 * context's first and only frame otherwise) renders nothing on its own first Begin/End -- auto-fit
		 * sizing and the font atlas's GPU texture both need one full frame to settle before anything is
		 * visible -- and a tab selection takes one more. The interactive path (overlay_frame) never needs
		 * this: it runs every loop iteration while the menu is open, so the same one-frame blank
		 * self-corrects at ~60 Hz, imperceptibly, on its own. */
		for (int i = 0; i < 3; i++) forced_frame(device, target, w, h, i == 2, build_ui);
		return;
	}
	if (!strcmp(ui, "ime")) {   /* (HLE-LIB-05) the IME card with an example dialog */
		imedlg_set_ui_ready(1); imedlg_open("Enter your name", "Player", 16);
		for (int i = 0; i < 3; i++) forced_frame(device, target, w, h, i == 2, [] { ImGuiIO &io = ImGui::GetIO(); imedlg_ui_build(io.DisplaySize.x, io.DisplaySize.y); });
		imedlg_close(); imedlg_set_ui_ready(g_has_window); return;
	}
	if (!strcmp(ui, "quit")) { for (int i = 0; i < 3; i++) forced_frame(device, target, w, h, i == 2, build_quit); return; }   /* (the quit card) */
	if (!strcmp(ui, "layout")) { for (int i = 0; i < 3; i++) forced_frame(device, target, w, h, i == 2, build_layout); return; }   /* (the layout card: RECOMP_GAME_NAME names the game) */
	if (!strcmp(ui, "prep")) { g_prep_demo[0] = 1240; g_prep_demo[1] = 5837; g_prep_demo[2] = 372; for (int i = 0; i < 3; i++) forced_frame(device, target, w, h, i == 2, build_prep); g_prep_demo[0] = -1; return; }   /* (the shader prep card, example numbers) */
	if (!strncmp(ui, "intro", 5)) {   /* "intro", "intro:pad", "intro:touch": the touch card with that device highlighted; "@<seconds>" after it, that moment of its loop */
		const char *at = strchr(ui, '@');
		g_intro_dev = !strncmp(ui, "intro:pad", 9) ? DEV_PAD : !strncmp(ui, "intro:touch", 11) ? DEV_TOUCH : DEV_KBM; g_intro_done = -1; g_intro_at = at ? atof(at + 1) : 0;
		for (int i = 0; i < 3; i++) forced_frame(device, target, w, h, i == 2, build_intro);
		g_intro_dev = -1; g_intro_at = -1;
		return;
	}
	if (!strncmp(ui, "touch:", 6)) { g_force_touch_badge = !strcmp(ui + 6, "rear") ? 2 : 1; for (int i = 0; i < 3; i++) forced_frame(device, target, w, h, i == 2, build_closed); g_force_touch_badge = 0; return; }   /* (UI-02d) the touch badge */
	if (!strcmp(ui, "osb")) { g_force_osb = true; for (int i = 0; i < 3; i++) forced_frame(device, target, w, h, i == 2, build_closed); g_force_osb = false; return; }   /* (UI-01n) the on-screen buttons */
	if (!strcmp(ui, "toast")) g_force_toast = true;
	else { g_force_bar = true; if (strcmp(ui, "bar")) {
		const char *colon = strrchr(ui, ':');   /* (U12b fix) "bar:Game" (ui_shots' page names) or bare "Game" (window.c's list): the menu key is the part after the last ':' */
		g_force_menu = colon ? colon + 1 : ui;
	} }
	g_nosd = metrics_osd_lines(g_osd, 7);   /* the HUD too (RECOMP_OSD), in the same frame as the bar so it layers as it does live: under it */
	g_nft = g_nosd > 1 ? metrics_frame_times(g_ft, 240) : 0;
	g_full = metrics_osd_level() == 2; if (g_full) metrics_snapshot(&g_snap);
	/* three frames: the bar's first, its menu popup's auto-fit on the second, and only the settled third
	 * drawn (the texture is LOADed into, so drawing all three would stack the bar's translucency) */
	for (int i = 0; i < 3; i++) forced_frame(device, target, w, h, i == 2, build_closed);
	g_nosd = 0; g_nft = 0; g_full = false;
	/* then put the closed state back the way a run finds it, inside one more (undrawn) frame so an open
	 * menu popup is closed through ImGui itself, not left in its popup stack */
	g_force_bar = false; g_force_menu = nullptr; g_bar_vis = 0; g_bar_want = false; g_toast_until = -1e9; g_tab_a = 0; g_dismiss_req = true;
	forced_frame(device, target, w, h, false, build_closed);
}
