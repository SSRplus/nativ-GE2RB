/*
 * The in-game overlay's scale (overlay.cpp's render_frame, every path: the menu, the bar, the cards, toasts, the HUD
 * and the headless dump). The overlay was laid out at 15 px for a 960x544 game; that size holds up to 720 lines.
 * Above, everything grows with the window's height (720 lines = 1, 1080 = 1.5, 2160 = 3), so the menu keeps its
 * share of the window. The display's content scale (Windows' 125/150/200 %) lifts the floor to 544 lines x that
 * scale, so a small window on a high-DPI display still gets text the size the system asks for. At most 4.
 * Plain C, header-only: runtime/tests/overlay_scale_test.c checks it.
 */
#ifndef RECOMP_UI_SCALE_H
#define RECOMP_UI_SCALE_H

static inline float overlay_ui_scale(float h, float dpi) {
	if (!(dpi > 0)) dpi = 1;
	float lines = h > 544.0f * dpi ? h : 544.0f * dpi;
	float k = lines / 720.0f;
#ifdef RECOMP_IOS
	{ float m = dpi * 1.1f; if (k < m) k = m; }   /* a phone: touch targets and text the size a thumb and an arm's length want (15 px at 3x is under 2 mm) */
#endif
	return k < 1 ? 1 : k > 4 ? 4 : k;
}
#endif
