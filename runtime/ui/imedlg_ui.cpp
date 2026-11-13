/* imedlg_ui.cpp -- HLE-LIB-05: the IME dialog's card, over the running game (the game polls the dialog; nothing pauses).
 * The keyboard types into the text field (Enter = OK, Escape = Cancel). The controller drives an on-screen key grid:
 * D-pad / left stick move, Cross types the key, Square deletes, Triangle is a space, L1 shifts, Start = OK, Circle =
 * Cancel. The mouse clicks keys and the field. window.c hands this the keys, text and pad while overlay_dialog_active;
 * the pad arrives as ImGui keys through savedlg_ui's feed (U12c). A key already down when the card appears answers
 * nothing (the press that opened it). The font is Latin-1: the grid holds only characters it can draw. */
#include "imedlg_ui.h"
#include "imgui.h"
#include "imgui_internal.h"   /* (ClearActiveID: a grid key edits the text while the field is being typed into) */
#include "imedlg.h"
#include <cmath>
#include <cstring>
#include <cstdio>

static int g_seq = -1, g_r, g_c; static bool g_armed, g_shift, g_focus;
static char g_buf[IMEDLG_TEXT_MAX]; static uint32_t g_max;
static const char *const ROWS[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-"};
static const char *const SPECIAL[4] = {"Shift", "Space", "Del", "OK"};
enum { NROWS = 5 };   /* the four letter rows and the special row */
static int row_len(int r) { return r < 4 ? 10 : 4; }

static bool add_char(char ch) {
	if (g_max && imedlg_utf8_len(g_buf) >= g_max) return false;
	const size_t n = strlen(g_buf); if (n + 2 >= sizeof g_buf) return false;
	g_buf[n] = ch; g_buf[n + 1] = 0; return true;
}
static void del_char(void) {
	size_t n = strlen(g_buf); if (!n) return;
	do n--; while (n && ((unsigned char)g_buf[n] & 0xC0) == 0x80);
	g_buf[n] = 0;
}
static int filter(ImGuiInputTextCallbackData *d) {   /* the length limit, in characters */
	if (d->EventFlag == ImGuiInputTextFlags_CallbackCharFilter && g_max && imedlg_utf8_len(g_buf) >= g_max) return 1;
	return 0;
}
static bool pressed(ImGuiKey k) { return ImGui::IsKeyPressed(k, true); }
static bool any_down(void) {
	static const ImGuiKey ks[] = {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Escape, ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadStart};
	for (ImGuiKey k : ks) if (ImGui::IsKeyDown(k)) return true;
	return false;
}

void imedlg_ui_build(float w, float hgt) {
	char title[512]; uint32_t mx; int seq;
	static char shown[IMEDLG_TEXT_MAX];
	if (!imedlg_view(title, sizeof title, shown, sizeof shown, &mx, &seq)) { g_seq = -1; return; }
	if (seq != g_seq) { g_seq = seq; snprintf(g_buf, sizeof g_buf, "%s", shown); g_max = mx; g_r = 1; g_c = 0; g_armed = false; g_shift = false; g_focus = true; }
	if (!g_armed && !any_down()) g_armed = true;
	int answer = -1;   /* 1 OK, 0 cancel */
	const float s = w >= 1400 ? 2.0f : 1.0f, em = ImGui::GetFontSize();
	ImGui::SetNextWindowPos(ImVec2(w * 0.5f, hgt * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(fminf(w - 32, 560 * s), 0), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.96f);
	ImGui::Begin("##imedlg", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
	ImGui::TextUnformatted(title[0] ? title : "Enter text");
	ImGui::Spacing();
	/* the pad: grid moves and keys (handled before the field, so its text is current when the field draws) */
	bool pad_edit = false;
	if (g_armed) {
		const int dr = (pressed(ImGuiKey_GamepadDpadDown) || pressed(ImGuiKey_GamepadLStickDown)) - (pressed(ImGuiKey_GamepadDpadUp) || pressed(ImGuiKey_GamepadLStickUp));
		const int dc = (pressed(ImGuiKey_GamepadDpadRight) || pressed(ImGuiKey_GamepadLStickRight)) - (pressed(ImGuiKey_GamepadDpadLeft) || pressed(ImGuiKey_GamepadLStickLeft));
		if (dr) { g_r = (g_r + dr + NROWS) % NROWS; if (g_c >= row_len(g_r)) g_c = row_len(g_r) - 1; }
		if (dc) g_c = (g_c + dc + row_len(g_r)) % row_len(g_r);
		if (pressed(ImGuiKey_GamepadL1)) g_shift = !g_shift;
		if (pressed(ImGuiKey_GamepadFaceLeft)) { del_char(); pad_edit = true; }
		if (pressed(ImGuiKey_GamepadFaceUp)) { add_char(' '); pad_edit = true; }
		if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) answer = 1;
		if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) answer = 0;
		if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) {
			if (g_r < 4) { char ch = ROWS[g_r][g_c]; if (g_shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32); add_char(ch); pad_edit = true; }
			else if (g_c == 0) g_shift = !g_shift;
			else if (g_c == 1) { add_char(' '); pad_edit = true; }
			else if (g_c == 2) { del_char(); pad_edit = true; }
			else answer = 1;
		}
	}
	if (pad_edit && ImGui::GetActiveID() != 0) ImGui::ClearActiveID();   /* (the field re-reads the text when not being edited) */
	/* the field: typing from the keyboard */
	ImGui::SetNextItemWidth(-1);
#ifdef RECOMP_IOS
	/* iOS: the key grid below is the keyboard (touch); the system keyboard would cover half the screen and this card with it, so the
	 * field only shows the text and never asks for it */
	g_focus = false;
	ImGui::InputText("##imetext", g_buf, sizeof g_buf, ImGuiInputTextFlags_ReadOnly);
#else
	if (g_focus || (!ImGui::IsAnyItemActive() && ImGui::GetIO().InputQueueCharacters.Size > 0)) { ImGui::SetKeyboardFocusHere(); g_focus = false; }
	if (ImGui::InputText("##imetext", g_buf, sizeof g_buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter, filter) && g_armed) answer = 1;
#endif
	if (g_armed && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) answer = 0;
	if (g_max) ImGui::TextDisabled("%u / %u", (unsigned)imedlg_utf8_len(g_buf), (unsigned)g_max);
	ImGui::Spacing();
	/* the grid (the pad's cursor drawn as a frame; clicks type too) */
	const float kw = (ImGui::GetContentRegionAvail().x - 9 * ImGui::GetStyle().ItemSpacing.x) / 10;
	const float sw = (ImGui::GetContentRegionAvail().x - 3 * ImGui::GetStyle().ItemSpacing.x) / 4;   /* (the special row: four keys) */
	for (int r = 0; r < NROWS; r++) {
		for (int c = 0; c < row_len(r); c++) {
			if (c) ImGui::SameLine();
			char lab[16];
			if (r < 4) { char ch = ROWS[r][c]; if (g_shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32); snprintf(lab, sizeof lab, "%c##k%d%d", ch, r, c); }
			else snprintf(lab, sizeof lab, "%s##s%d", c == 0 && g_shift ? "SHIFT" : SPECIAL[c], c);
			const float bw = r < 4 ? kw : sw;
			const bool cur = r == g_r && c == g_c;
			if (cur) { ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)); ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f); }
			if (ImGui::Button(lab, ImVec2(bw, em * 1.6f)) && g_armed) {
				g_r = r; g_c = c;
				if (r < 4) { char ch = ROWS[r][c]; if (g_shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32); add_char(ch); }
				else if (c == 0) g_shift = !g_shift; else if (c == 1) add_char(' '); else if (c == 2) del_char(); else answer = 1;
				g_focus = true;
			}
			if (cur) { ImGui::PopStyleVar(); ImGui::PopStyleColor(); }
		}
	}
	ImGui::Spacing();
	ImGui::PushTextWrapPos(0); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
#ifndef RECOMP_IOS
	ImGui::TextWrapped("Keyboard: type, Enter OK, Esc cancel. Pad: Cross types the key, Square deletes, Triangle space, L1 shift, Start OK, Circle cancel.");
#endif
	ImGui::PopStyleColor(); ImGui::PopTextWrapPos();
	if (ImGui::Button("Cancel") && g_armed) answer = 0;
	ImGui::SameLine();
	if (ImGui::Button("OK") && g_armed) answer = 1;
	ImGui::End();
	(void)s;
	if (answer >= 0) { imedlg_answer(answer, g_buf); g_seq = -1; }
}
