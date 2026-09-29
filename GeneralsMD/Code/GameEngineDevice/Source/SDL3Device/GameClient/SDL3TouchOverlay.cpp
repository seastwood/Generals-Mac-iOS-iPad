/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
** SDL3TouchOverlay.cpp
**
** GeneralsX @feature seastwood 29/09/2026 On-screen overlay controls for touch devices (iOS).
**
**   Keyboard button   tap: show / hide the on-screen keyboard
**                     long-press, then drag: move it (the position is saved)
**   Hotkey toolbar    a tab fixed in the top-right corner, in a game only; tap it to open or
**                     close the toolbar (it stays as left, and is remembered). Buttons send the
**                     game's own hotkey commands (no key bindings involved). Top row: All, Same,
**                     Stop, Scatter, Home, Alert, Shift, Menu, Opts (settings page). Round buttons
**                     down the right side: Ctrl, then groups 1-5 (tap: select, Ctrl + tap or
**                     long-press: assign the current selection). Ctrl / Shift: tap = held for the
**                     next tap on the game, long-press = locked until tapped again.
**   Settings page     button size, opacity, keyboard button on/off, double-tap right-click,
**                     edge scrolling, tap feedback rings
**   Tap feedback      a short ring where each click lands (white: left, orange: right)
**
** Positions are normalized to the screen; drawing uses the display's pixel coordinates.
*/

#include "SDL3Device/GameClient/SDL3TouchOverlay.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE

#include "Common/AsciiString.h"
#include "Common/MessageStream.h"
#include "Common/UnicodeString.h"
#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/GameFont.h"
#include "GameClient/GlobalLanguage.h"
#include "GameClient/Keyboard.h"
#include "GameLogic/GameLogic.h"
#include "SDL3Device/GameClient/SDL3Keyboard.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

extern SDL_Window *TheSDL3Window;   // created in SDL3Main.cpp

namespace {

// ---------------------------------------------------------------------------
// Settings (Documents/touch-overlay.ini)
// ---------------------------------------------------------------------------
struct OverlaySettings {
	float keyboardX = 0.95f;          // keyboard button center, normalized
	float keyboardY = 0.30f;
	bool keyboardVisible = true;
	float scale = 1.0f;               // size of the overlay controls
	float opacity = 1.0f;             // multiplies every overlay alpha
	bool doubleTapRightClick = false; // off by default: the first tap is already a left click
	bool edgePan = true;
	bool tapFeedback = true;
	bool toolbarOpen = false;         // stays open until closed, remembered across launches
};

OverlaySettings s_settings;
bool s_settingsLoaded = false;

const float SCALE_MIN = 0.6f, SCALE_MAX = 1.6f, SCALE_STEP = 0.1f;
const float OPACITY_MIN = 0.3f, OPACITY_MAX = 1.0f, OPACITY_STEP = 0.1f;

bool documentsPath(const char *fileName, char *path, size_t size)
{
	const char *home = getenv("HOME");
	if (home == nullptr) {
		return false;
	}
	snprintf(path, size, "%s/Documents/%s", home, fileName);
	return true;
}

void clampSettings()
{
	s_settings.keyboardX = SDL_clamp(s_settings.keyboardX, 0.0f, 1.0f);
	s_settings.keyboardY = SDL_clamp(s_settings.keyboardY, 0.0f, 1.0f);
	s_settings.scale = SDL_clamp(s_settings.scale, SCALE_MIN, SCALE_MAX);
	s_settings.opacity = SDL_clamp(s_settings.opacity, OPACITY_MIN, OPACITY_MAX);
}

void loadSettings()
{
	s_settingsLoaded = true;
	char path[1024];
	if (!documentsPath("touch-overlay.ini", path, sizeof(path))) {
		return;
	}

	FILE *file = fopen(path, "r");
	if (file == nullptr) {
		// Earlier builds saved only the keyboard button position.
		char legacyPath[1024];
		if (documentsPath("touch-keyboard-button.txt", legacyPath, sizeof(legacyPath))) {
			FILE *legacy = fopen(legacyPath, "r");
			if (legacy != nullptr) {
				float x = 0.0f, y = 0.0f;
				if (fscanf(legacy, "%f %f", &x, &y) == 2) {
					s_settings.keyboardX = x;
					s_settings.keyboardY = y;
				}
				fclose(legacy);
			}
		}
		clampSettings();
		return;
	}

	char line[256];
	while (fgets(line, sizeof(line), file) != nullptr) {
		char key[64];
		float value = 0.0f;
		if (sscanf(line, " %63[^= ] = %f", key, &value) != 2) {
			continue;
		}
		const std::string name(key);
		if (name == "keyboard_x") s_settings.keyboardX = value;
		else if (name == "keyboard_y") s_settings.keyboardY = value;
		else if (name == "keyboard_visible") s_settings.keyboardVisible = value != 0.0f;
		else if (name == "scale") s_settings.scale = value;
		else if (name == "opacity") s_settings.opacity = value;
		else if (name == "double_tap_right_click") s_settings.doubleTapRightClick = value != 0.0f;
		else if (name == "edge_pan") s_settings.edgePan = value != 0.0f;
		else if (name == "tap_feedback") s_settings.tapFeedback = value != 0.0f;
		else if (name == "toolbar_open") s_settings.toolbarOpen = value != 0.0f;
	}
	fclose(file);
	clampSettings();
}

void saveSettings()
{
	char path[1024];
	if (!documentsPath("touch-overlay.ini", path, sizeof(path))) {
		return;
	}
	FILE *file = fopen(path, "w");
	if (file == nullptr) {
		fprintf(stderr, "WARNING: could not save touch overlay settings to %s\n", path);
		return;
	}
	fprintf(file, "keyboard_x=%f\n", s_settings.keyboardX);
	fprintf(file, "keyboard_y=%f\n", s_settings.keyboardY);
	fprintf(file, "keyboard_visible=%d\n", s_settings.keyboardVisible ? 1 : 0);
	fprintf(file, "scale=%f\n", s_settings.scale);
	fprintf(file, "opacity=%f\n", s_settings.opacity);
	fprintf(file, "double_tap_right_click=%d\n", s_settings.doubleTapRightClick ? 1 : 0);
	fprintf(file, "edge_pan=%d\n", s_settings.edgePan ? 1 : 0);
	fprintf(file, "tap_feedback=%d\n", s_settings.tapFeedback ? 1 : 0);
	fprintf(file, "toolbar_open=%d\n", s_settings.toolbarOpen ? 1 : 0);
	fclose(file);
}

void ensureSettings()
{
	if (!s_settingsLoaded) {
		loadSettings();
	}
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------
struct Rect {
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;   // display pixels
	bool contains(float px, float py, float margin = 0.0f) const
	{
		return px >= x - margin && px <= x + w + margin && py >= y - margin && py <= y + h + margin;
	}
};

bool screenSize(float &width, float &height)
{
	if (TheDisplay == nullptr || TheDisplay->getWidth() == 0 || TheDisplay->getHeight() == 0) {
		return false;
	}
	width = (float)TheDisplay->getWidth();
	height = (float)TheDisplay->getHeight();
	return true;
}

// Screen area clear of the rounded corners, camera housing and home indicator, as display-pixel
// insets. SDL reports the UIKit safe area in window points.
struct Insets {
	float left = 0.0f, right = 0.0f, top = 0.0f, bottom = 0.0f;
};

Insets safeAreaInsets(float screenW, float screenH)
{
	Insets insets;
	SDL_Rect safe;
	int windowW = 0, windowH = 0;
	if (TheSDL3Window == nullptr || !SDL_GetWindowSafeArea(TheSDL3Window, &safe) ||
	    !SDL_GetWindowSize(TheSDL3Window, &windowW, &windowH) || windowW <= 0 || windowH <= 0) {
		return insets;
	}
	const float scaleX = screenW / (float)windowW;
	const float scaleY = screenH / (float)windowH;
	insets.left = SDL_max(0.0f, (float)safe.x * scaleX);
	insets.top = SDL_max(0.0f, (float)safe.y * scaleY);
	insets.right = SDL_max(0.0f, (float)(windowW - safe.x - safe.w) * scaleX);
	insets.bottom = SDL_max(0.0f, (float)(windowH - safe.y - safe.h) * scaleY);
	return insets;
}

Color overlayColor(UnsignedByte r, UnsignedByte g, UnsignedByte b, float alpha)
{
	const float a = SDL_clamp(alpha * s_settings.opacity, 0.0f, 255.0f);
	return GameMakeColor(r, g, b, (UnsignedByte)a);
}

// ---------------------------------------------------------------------------
// Text labels (one cached DisplayString per label text)
// ---------------------------------------------------------------------------
std::map<std::string, DisplayString *> s_labels;
GameFont *s_labelFont = nullptr;
Int s_labelFontSize = 0;

bool setLabelFontSize(Int pointSize)
{
	if (TheFontLibrary == nullptr) {
		return false;
	}
	pointSize = SDL_max(pointSize, 6);
	if (pointSize == s_labelFontSize && s_labelFont != nullptr) {
		return true;
	}
	const AsciiString fontName = TheGlobalLanguageData != nullptr ?
		TheGlobalLanguageData->m_defaultWindowFont.name : AsciiString("Arial");
	GameFont *font = TheFontLibrary->getFont(fontName, pointSize, TRUE);
	if (font == nullptr) {
		return false;
	}
	s_labelFont = font;
	s_labelFontSize = pointSize;
	for (std::map<std::string, DisplayString *>::iterator it = s_labels.begin(); it != s_labels.end(); ++it) {
		it->second->setFont(s_labelFont);
	}
	return true;
}

DisplayString *labelString(const std::string &text)
{
	std::map<std::string, DisplayString *>::iterator it = s_labels.find(text);
	if (it != s_labels.end()) {
		return it->second;
	}
	if (TheDisplayStringManager == nullptr || s_labelFont == nullptr) {
		return nullptr;
	}
	DisplayString *string = TheDisplayStringManager->newDisplayString();
	if (string == nullptr) {
		return nullptr;
	}
	string->setFont(s_labelFont);
	UnicodeString unicodeText;
	unicodeText.translate(AsciiString(text.c_str()));
	string->setText(unicodeText);
	s_labels[text] = string;
	return string;
}

void labelSize(const std::string &text, Int &width, Int &height)
{
	width = 0;
	height = 0;
	DisplayString *string = labelString(text);
	if (string != nullptr) {
		string->getSize(&width, &height);
	}
}

void drawLabelCentered(const std::string &text, const Rect &rect, Color color)
{
	DisplayString *string = labelString(text);
	if (string == nullptr) {
		return;
	}
	Int width = 0, height = 0;
	string->getSize(&width, &height);
	const Int x = (Int)(rect.x + (rect.w - (float)width) * 0.5f);
	const Int y = (Int)(rect.y + (rect.h - (float)height) * 0.5f);
	string->draw(x, y, color, overlayColor(0, 0, 0, 200));
}

// ---------------------------------------------------------------------------
// Ctrl / Shift modifiers, injected as key events so the game sees a held key
// ---------------------------------------------------------------------------
enum ModifierState { MODIFIER_OFF, MODIFIER_ONE_SHOT, MODIFIER_LOCKED };

ModifierState s_ctrl = MODIFIER_OFF;
ModifierState s_shift = MODIFIER_OFF;
Uint64 s_frame = 0;
Uint64 s_releaseOneShotAtFrame = 0;   // 0: nothing pending

void sendModifierKey(SDL_Scancode scancode, bool down)
{
	SDL3Keyboard *keyboard = dynamic_cast<SDL3Keyboard *>(TheKeyboard);
	if (keyboard == nullptr) {
		return;
	}
	SDL_Event event;
	SDL_zero(event);
	event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
	event.key.scancode = scancode;
	event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
	event.key.down = down;
	keyboard->addSDLEvent(&event);
}

void setModifier(ModifierState &state, ModifierState newState, SDL_Scancode scancode)
{
	const bool wasDown = state != MODIFIER_OFF;
	const bool isDown = newState != MODIFIER_OFF;
	state = newState;
	if (wasDown != isDown) {
		sendModifierKey(scancode, isDown);
	}
}

void releaseOneShotModifiers()
{
	if (s_ctrl == MODIFIER_ONE_SHOT) {
		setModifier(s_ctrl, MODIFIER_OFF, SDL_SCANCODE_LCTRL);
	}
	if (s_shift == MODIFIER_ONE_SHOT) {
		setModifier(s_shift, MODIFIER_OFF, SDL_SCANCODE_LSHIFT);
	}
}

void releaseAllModifiers()
{
	setModifier(s_ctrl, MODIFIER_OFF, SDL_SCANCODE_LCTRL);
	setModifier(s_shift, MODIFIER_OFF, SDL_SCANCODE_LSHIFT);
	s_releaseOneShotAtFrame = 0;
}

// ---------------------------------------------------------------------------
// Hotkey toolbar
// ---------------------------------------------------------------------------
enum ToolbarAction {
	ACTION_META,            // value: GameMessage::Type
	ACTION_GROUP,           // value: group number
	ACTION_CTRL,
	ACTION_SHIFT,
	ACTION_OPEN_SETTINGS,
	ACTION_CLOSE_SETTINGS,
	ACTION_SIZE_DOWN,
	ACTION_SIZE_UP,
	ACTION_FADE_DOWN,
	ACTION_FADE_UP,
	ACTION_TOGGLE_KEYBOARD_BUTTON,
	ACTION_TOGGLE_DOUBLE_TAP,
	ACTION_TOGGLE_EDGE_PAN,
	ACTION_TOGGLE_TAP_FEEDBACK
};

struct ToolbarButton {
	const char *label;
	ToolbarAction action;
	Int value;
};

// Top row, to the left of the Hotkeys tab.
const ToolbarButton MAIN_BUTTONS[] = {
	{ "All",     ACTION_META,  GameMessage::MSG_META_SELECT_ALL },
	{ "Same",    ACTION_META,  GameMessage::MSG_META_SELECT_MATCHING_UNITS },
	{ "Stop",    ACTION_META,  GameMessage::MSG_META_STOP },
	{ "Scatter", ACTION_META,  GameMessage::MSG_META_SCATTER },
	{ "Home",    ACTION_META,  GameMessage::MSG_META_VIEW_COMMAND_CENTER },
	{ "Alert",   ACTION_META,  GameMessage::MSG_META_VIEW_LAST_RADAR_EVENT },
	{ "Shift",   ACTION_SHIFT, 0 },
	{ "Menu",    ACTION_META,  GameMessage::MSG_META_OPTIONS },
	{ "Opts",    ACTION_OPEN_SETTINGS, 0 },
};

const ToolbarButton SETTINGS_BUTTONS[] = {
	{ "Size -",   ACTION_SIZE_DOWN, 0 },
	{ "Size +",   ACTION_SIZE_UP, 0 },
	{ "Fade -",   ACTION_FADE_DOWN, 0 },
	{ "Fade +",   ACTION_FADE_UP, 0 },
	{ "Keys",     ACTION_TOGGLE_KEYBOARD_BUTTON, 0 },
	{ "2-Tap",    ACTION_TOGGLE_DOUBLE_TAP, 0 },
	{ "Edge",     ACTION_TOGGLE_EDGE_PAN, 0 },
	{ "Rings",    ACTION_TOGGLE_TAP_FEEDBACK, 0 },
	{ "Back",     ACTION_CLOSE_SETTINGS, 0 },
};

// Round buttons in a column down the right side, below the tab: Ctrl on top, then the groups.
const ToolbarButton COLUMN_BUTTONS[] = {
	{ "Ctrl", ACTION_CTRL,  0 },
	{ "1",    ACTION_GROUP, 1 },
	{ "2",    ACTION_GROUP, 2 },
	{ "3",    ACTION_GROUP, 3 },
	{ "4",    ACTION_GROUP, 4 },
	{ "5",    ACTION_GROUP, 5 },
};

const Int MAIN_BUTTON_COUNT = (Int)(sizeof(MAIN_BUTTONS) / sizeof(MAIN_BUTTONS[0]));
const Int SETTINGS_BUTTON_COUNT = (Int)(sizeof(SETTINGS_BUTTONS) / sizeof(SETTINGS_BUTTONS[0]));
const Int COLUMN_BUTTON_COUNT = (Int)(sizeof(COLUMN_BUTTONS) / sizeof(COLUMN_BUTTONS[0]));

// Button indices: row buttons use their index on the current page, column buttons are offset by
// COLUMN_BASE, and the tab has its own id.
const Int COLUMN_BASE = 100;
const Int TAB_INDEX = -2;

const Uint64 TOOLBAR_LONG_PRESS_MS = 500;

bool s_settingsPage = false;

const ToolbarButton *currentButtons(Int &count)
{
	if (s_settingsPage) {
		count = SETTINGS_BUTTON_COUNT;
		return SETTINGS_BUTTONS;
	}
	count = MAIN_BUTTON_COUNT;
	return MAIN_BUTTONS;
}

const ToolbarButton *buttonForIndex(Int index)
{
	if (index >= COLUMN_BASE) {
		const Int column = index - COLUMN_BASE;
		return column < COLUMN_BUTTON_COUNT ? &COLUMN_BUTTONS[column] : nullptr;
	}
	Int count = 0;
	const ToolbarButton *buttons = currentButtons(count);
	return index >= 0 && index < count ? &buttons[index] : nullptr;
}

// Press feedback: a tap is often shorter than a frame, so a button that was just activated keeps
// a bright highlight for a moment. Long-press actions (group assigned, key locked) flash green.
const Uint64 FLASH_MS = 250;
struct ButtonFlash {
	Int index = -1;
	bool settingsPage = false;  // page a row index belongs to
	bool longPress = false;
	Uint64 until = 0;
};
ButtonFlash s_flash;

void flashButton(Int index, bool longPress)
{
	s_flash.index = index;
	s_flash.settingsPage = s_settingsPage;
	s_flash.longPress = longPress;
	s_flash.until = SDL_GetTicks() + FLASH_MS;
}

bool isFlashing(Int index, bool &longPress)
{
	if (s_flash.index != index || SDL_GetTicks() >= s_flash.until) {
		return false;
	}
	if (index >= 0 && index < COLUMN_BASE && s_flash.settingsPage != s_settingsPage) {
		return false;
	}
	longPress = s_flash.longPress;
	return true;
}

struct ToolbarLayout {
	bool valid = false;
	Rect tab;
	std::vector<Rect> buttons;   // row buttons of the current page, empty while collapsed
	std::vector<Rect> column;    // round buttons, empty while collapsed
};
ToolbarLayout s_layout;

std::string onOff(const char *name, bool on)
{
	return std::string(name) + (on ? ": On" : ": Off");
}

std::string buttonLabel(const ToolbarButton &button)
{
	switch (button.action) {
	case ACTION_TOGGLE_KEYBOARD_BUTTON: return onOff(button.label, s_settings.keyboardVisible);
	case ACTION_TOGGLE_DOUBLE_TAP: return onOff(button.label, s_settings.doubleTapRightClick);
	case ACTION_TOGGLE_EDGE_PAN: return onOff(button.label, s_settings.edgePan);
	case ACTION_TOGGLE_TAP_FEEDBACK: return onOff(button.label, s_settings.tapFeedback);
	default: return std::string(button.label);
	}
}

bool toolbarAvailable()
{
	// In a game only: the shell menus have no use for unit hotkeys.
	return TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame();
}

void activateToolbarButton(const ToolbarButton &button, bool longPress)
{
	switch (button.action) {
	case ACTION_META:
		if (TheMessageStream != nullptr) {
			TheMessageStream->appendMessage((GameMessage::Type)button.value);
		}
		break;
	case ACTION_GROUP:
		if (TheMessageStream != nullptr) {
			// Ctrl + number assigns the group, as on a keyboard; so does a long-press. The toolbar
			// sends the command directly, so it has to honor the sticky Ctrl itself.
			const bool assign = longPress || s_ctrl != MODIFIER_OFF;
			const Int base = assign ? GameMessage::MSG_META_CREATE_TEAM0 : GameMessage::MSG_META_SELECT_TEAM0;
			TheMessageStream->appendMessage((GameMessage::Type)(base + button.value));
			if (!longPress && s_ctrl == MODIFIER_ONE_SHOT) {
				setModifier(s_ctrl, MODIFIER_OFF, SDL_SCANCODE_LCTRL);
			}
		}
		break;
	case ACTION_CTRL:
	case ACTION_SHIFT:
		{
			ModifierState &state = button.action == ACTION_CTRL ? s_ctrl : s_shift;
			const SDL_Scancode scancode = button.action == ACTION_CTRL ? SDL_SCANCODE_LCTRL : SDL_SCANCODE_LSHIFT;
			ModifierState next = MODIFIER_OFF;
			if (longPress) {
				next = MODIFIER_LOCKED;
			} else if (state == MODIFIER_OFF) {
				next = MODIFIER_ONE_SHOT;
			}
			setModifier(state, next, scancode);
		}
		break;
	case ACTION_OPEN_SETTINGS:
		s_settingsPage = true;
		break;
	case ACTION_CLOSE_SETTINGS:
		s_settingsPage = false;
		break;
	case ACTION_SIZE_DOWN:
		s_settings.scale = SDL_max(SCALE_MIN, s_settings.scale - SCALE_STEP);
		saveSettings();
		break;
	case ACTION_SIZE_UP:
		s_settings.scale = SDL_min(SCALE_MAX, s_settings.scale + SCALE_STEP);
		saveSettings();
		break;
	case ACTION_FADE_DOWN:
		s_settings.opacity = SDL_max(OPACITY_MIN, s_settings.opacity - OPACITY_STEP);
		saveSettings();
		break;
	case ACTION_FADE_UP:
		s_settings.opacity = SDL_min(OPACITY_MAX, s_settings.opacity + OPACITY_STEP);
		saveSettings();
		break;
	case ACTION_TOGGLE_KEYBOARD_BUTTON:
		s_settings.keyboardVisible = !s_settings.keyboardVisible;
		saveSettings();
		break;
	case ACTION_TOGGLE_DOUBLE_TAP:
		s_settings.doubleTapRightClick = !s_settings.doubleTapRightClick;
		saveSettings();
		break;
	case ACTION_TOGGLE_EDGE_PAN:
		s_settings.edgePan = !s_settings.edgePan;
		saveSettings();
		break;
	case ACTION_TOGGLE_TAP_FEEDBACK:
		s_settings.tapFeedback = !s_settings.tapFeedback;
		saveSettings();
		break;
	}
}

// Lay the toolbar out for the current page. Labels are measured with the real font; when the row
// does not fit the screen width the font and buttons shrink until it does.
void layoutToolbar(float screenW, float screenH)
{
	s_layout.valid = false;
	s_layout.buttons.clear();
	s_layout.column.clear();

	const float rowH = screenH * 0.07f * s_settings.scale;
	// Stay clear of the rounded screen corners: the safe area covers the camera housing on
	// phones, and the extra margin keeps the corner tab off the curve on every device.
	const Insets insets = safeAreaInsets(screenW, screenH);
	const float margin = screenH * 0.02f;
	const float gap = SDL_max(4.0f, rowH * 0.12f);
	Int pointSize = (Int)(rowH * 0.30f);

	Int count = 0;
	const ToolbarButton *buttons = currentButtons(count);
	std::vector<float> widths((size_t)count, 0.0f);
	float shrink = 1.0f;
	float total = 0.0f;
	for (Int attempt = 0; attempt < 8; ++attempt) {
		if (!setLabelFontSize((Int)((float)pointSize * shrink))) {
			return;
		}
		total = 0.0f;
		for (Int i = 0; i < count; ++i) {
			Int textW = 0, textH = 0;
			labelSize(buttonLabel(buttons[i]), textW, textH);
			// Pills: room for the rounded ends on both sides of the label.
			widths[(size_t)i] = SDL_max(rowH * shrink * 1.4f, (float)textW + rowH * shrink * 0.9f);
			total += widths[(size_t)i];
		}
		total += gap * (float)(count - 1);
		if (total <= screenW - insets.left - insets.right - 2.0f * margin - rowH * 2.6f) {
			break;
		}
		shrink *= 0.88f;
	}

	const float buttonH = rowH * shrink;

	// The tab never moves: top-right corner, same size open or closed.
	Int hotkeysW = 0, hotkeysH = 0, hideW = 0, hideH = 0;
	labelSize("Hotkeys", hotkeysW, hotkeysH);
	labelSize("Hide", hideW, hideH);
	const float tabTextW = (float)SDL_max(hotkeysW, hideW);
	s_layout.tab.w = SDL_max(rowH * 1.8f, tabTextW + rowH * 0.9f);
	s_layout.tab.h = rowH;
	s_layout.tab.x = screenW - insets.right - s_layout.tab.w - margin;
	s_layout.tab.y = insets.top + margin;

	if (s_settings.toolbarOpen) {
		// The button row sits to the left of the tab, right-aligned against it.
		float x = s_layout.tab.x - gap - total;
		for (Int i = 0; i < count; ++i) {
			Rect rect;
			rect.x = x;
			rect.y = s_layout.tab.y + (rowH - buttonH) * 0.5f;
			rect.w = widths[(size_t)i];
			rect.h = buttonH;
			s_layout.buttons.push_back(rect);
			x += widths[(size_t)i] + gap;
		}

		// The round buttons run down the right side under the tab, right edges aligned.
		const float diameter = screenH * 0.085f * s_settings.scale;
		const float columnGap = diameter * 0.2f;
		float y = s_layout.tab.y + s_layout.tab.h + columnGap * 1.5f;
		for (Int i = 0; i < COLUMN_BUTTON_COUNT; ++i) {
			Rect rect;
			rect.w = diameter;
			rect.h = diameter;
			rect.x = s_layout.tab.x + s_layout.tab.w - diameter;
			rect.y = y;
			s_layout.column.push_back(rect);
			y += diameter + columnGap;
		}
	}
	s_layout.valid = true;
}

// ---------------------------------------------------------------------------
// Rounded shapes. Filled with horizontal spans (batched by the caller), outlined with short line
// segments, which is enough for clean circles and pills at phone and tablet resolutions.
// ---------------------------------------------------------------------------
void fillRoundedRect(const Rect &rect, float radius, Color color)
{
	radius = SDL_min(radius, SDL_min(rect.w, rect.h) * 0.5f);
	const Int STEP = 2;
	for (Int dy = 0; dy < (Int)rect.h; dy += STEP) {
		const float rowCenter = (float)dy + (float)STEP * 0.5f;
		float inset = 0.0f;
		float fromEdge = -1.0f;
		if (rowCenter < radius) {
			fromEdge = radius - rowCenter;
		} else if (rowCenter > rect.h - radius) {
			fromEdge = rowCenter - (rect.h - radius);
		}
		if (fromEdge >= 0.0f) {
			inset = radius - SDL_sqrtf(SDL_max(0.0f, radius * radius - fromEdge * fromEdge));
		}
		const Int height = SDL_min(STEP, (Int)rect.h - dy);
		TheDisplay->drawFillRect((Int)(rect.x + inset), (Int)rect.y + dy, (Int)(rect.w - 2.0f * inset), height, color);
	}
}

void strokeRoundedRect(const Rect &rect, float radius, Color color, float lineWidth)
{
	radius = SDL_min(radius, SDL_min(rect.w, rect.h) * 0.5f);
	const Int ARC_SEGMENTS = 10;
	// Corner centers, clockwise from top-left, with the angle each arc starts at.
	const float cx[4] = { rect.x + radius, rect.x + rect.w - radius, rect.x + rect.w - radius, rect.x + radius };
	const float cy[4] = { rect.y + radius, rect.y + radius, rect.y + rect.h - radius, rect.y + rect.h - radius };
	const float start[4] = { SDL_PI_F, SDL_PI_F * 1.5f, 0.0f, SDL_PI_F * 0.5f };
	float prevX = 0.0f, prevY = 0.0f;
	bool havePrev = false;
	float firstX = 0.0f, firstY = 0.0f;
	for (Int corner = 0; corner < 4; ++corner) {
		for (Int s = 0; s <= ARC_SEGMENTS; ++s) {
			const float angle = start[corner] + (float)s / (float)ARC_SEGMENTS * SDL_PI_F * 0.5f;
			const float px = cx[corner] + SDL_cosf(angle) * radius;
			const float py = cy[corner] + SDL_sinf(angle) * radius;
			if (havePrev) {
				TheDisplay->drawLine((Int)prevX, (Int)prevY, (Int)px, (Int)py, lineWidth, color);
			} else {
				firstX = px;
				firstY = py;
			}
			prevX = px;
			prevY = py;
			havePrev = true;
		}
	}
	TheDisplay->drawLine((Int)prevX, (Int)prevY, (Int)firstX, (Int)firstY, lineWidth, color);
}

// ---------------------------------------------------------------------------
// Keyboard button
// ---------------------------------------------------------------------------
bool s_keyboardOpen = false;
float s_keyboardFieldTop = -1.0f;

const float KEYBOARD_BUTTON_SIZE_RATIO = 0.085f;   // side as a fraction of the screen height
const Uint64 KEYBOARD_BUTTON_DRAG_MS = 400;

bool keyboardButtonHalfExtents(float &halfW, float &halfH)
{
	float screenW = 0.0f, screenH = 0.0f;
	if (!screenSize(screenW, screenH)) {
		return false;
	}
	const float side = screenH * KEYBOARD_BUTTON_SIZE_RATIO * s_settings.scale;
	halfW = side * 0.5f / screenW;
	halfH = side * 0.5f / screenH;
	return true;
}

void clampKeyboardButton()
{
	float halfW = 0.0f, halfH = 0.0f;
	if (!keyboardButtonHalfExtents(halfW, halfH)) {
		return;
	}
	float screenW = 0.0f, screenH = 0.0f;
	screenSize(screenW, screenH);
	const Insets insets = safeAreaInsets(screenW, screenH);
	const float minX = halfW + insets.left / screenW, maxX = 1.0f - halfW - insets.right / screenW;
	const float minY = halfH + insets.top / screenH, maxY = 1.0f - halfH - insets.bottom / screenH;
	s_settings.keyboardX = SDL_clamp(s_settings.keyboardX, minX, SDL_max(minX, maxX));
	s_settings.keyboardY = SDL_clamp(s_settings.keyboardY, minY, SDL_max(minY, maxY));
}

// Where the button is actually shown. The saved position is untouched and applies again once
// whatever moved the button is gone.
// - While the keyboard is open it must not sit underneath it, or it could not be tapped to close
//   the keyboard again. The keyboard's height is not known here, so keep the button in the top
//   part of the screen; when an entry field is being typed into, SDL slides the view so the field
//   stays above the keyboard, and just above the field is visible too.
// - While the toolbar is open it must not cover the round buttons on the right: it moves left of
//   the column.
void keyboardButtonCenter(float &x, float &y)
{
	x = s_settings.keyboardX;
	y = s_settings.keyboardY;
	float halfW = 0.0f, halfH = 0.0f;
	if (!keyboardButtonHalfExtents(halfW, halfH)) {
		return;
	}
	if (s_keyboardOpen) {
		float limit = 0.30f;
		if (s_keyboardFieldTop >= 0.0f) {
			limit = SDL_max(s_keyboardFieldTop - halfH - 0.01f, halfH);
		}
		y = SDL_min(y, limit);
	}
	float screenW = 0.0f, screenH = 0.0f;
	if (s_layout.valid && !s_layout.column.empty() && screenSize(screenW, screenH)) {
		const float columnLeft = s_layout.column.front().x / screenW;
		const float columnTop = s_layout.column.front().y / screenH;
		const float columnBottom = (s_layout.column.back().y + s_layout.column.back().h) / screenH;
		if (x + halfW > columnLeft && y + halfH > columnTop && y - halfH < columnBottom) {
			x = columnLeft - halfW - 0.01f;
		}
	}
}

bool keyboardButtonHit(float x, float y)
{
	float halfW = 0.0f, halfH = 0.0f;
	if (!s_settings.keyboardVisible || !keyboardButtonHalfExtents(halfW, halfH)) {
		return false;
	}
	// A little extra margin: the button is small and fingers are not.
	halfW *= 1.2f;
	halfH *= 1.2f;
	float centerX = 0.0f, centerY = 0.0f;
	keyboardButtonCenter(centerX, centerY);
	return SDL_fabsf(x - centerX) <= halfW && SDL_fabsf(y - centerY) <= halfH;
}

// ---------------------------------------------------------------------------
// Press tracking (one overlay press at a time)
// ---------------------------------------------------------------------------
enum PressTarget { PRESS_NONE, PRESS_KEYBOARD_BUTTON, PRESS_TOOLBAR_TAB, PRESS_TOOLBAR_BUTTON };

struct PressState {
	PressTarget target = PRESS_NONE;
	Int index = -1;                // toolbar button index (row, or COLUMN_BASE + column)
	SDL_TouchID touch = 0;
	SDL_FingerID finger = 0;
	Uint64 downTicks = 0;
	float downX = 0.0f, downY = 0.0f;   // normalized
	float grabX = 0.0f, grabY = 0.0f;   // keyboard button drag offset
	bool moved = false;                 // slid away: neither a tap nor a long-press
	bool longPressFired = false;
	bool dragging = false;              // keyboard button follows the finger
};

PressState s_press;
const float PRESS_SLOP = 0.02f;   // normalized movement that cancels a tap

// ---------------------------------------------------------------------------
// Tap feedback rings
// ---------------------------------------------------------------------------
struct Ring {
	float x = 0.0f, y = 0.0f;   // normalized
	Uint64 start = 0;
	bool right = false;
	bool active = false;
};

const Int RING_COUNT = 8;
const Uint64 RING_MS = 350;
Ring s_rings[RING_COUNT];
Int s_nextRing = 0;

void drawRings(float screenW, float screenH)
{
	const Uint64 now = SDL_GetTicks();
	const float lineWidth = SDL_max(2.0f, screenH / 400.0f);
	for (Int i = 0; i < RING_COUNT; ++i) {
		Ring &ring = s_rings[i];
		if (!ring.active) {
			continue;
		}
		const Uint64 age = now - ring.start;
		if (age >= RING_MS) {
			ring.active = false;
			continue;
		}
		const float t = (float)age / (float)RING_MS;
		const float radius = screenH * (0.015f + 0.035f * t) * s_settings.scale;
		const Color color = ring.right ? overlayColor(255, 150, 40, 220.0f * (1.0f - t))
		                               : overlayColor(255, 255, 255, 200.0f * (1.0f - t));
		const float cx = ring.x * screenW;
		const float cy = ring.y * screenH;
		const Int SEGMENTS = 20;
		for (Int s = 0; s < SEGMENTS; ++s) {
			const float a0 = (float)s / (float)SEGMENTS * 2.0f * SDL_PI_F;
			const float a1 = (float)(s + 1) / (float)SEGMENTS * 2.0f * SDL_PI_F;
			TheDisplay->drawLine((Int)(cx + SDL_cosf(a0) * radius), (Int)(cy + SDL_sinf(a0) * radius),
			                     (Int)(cx + SDL_cosf(a1) * radius), (Int)(cy + SDL_sinf(a1) * radius),
			                     lineWidth, color);
		}
	}
}

// Button look, in the style of iOS game controller overlays: dark translucent material with a thin
// light rim and white label. Pressed and just-activated buttons are drawn opaque and bright,
// whatever the opacity setting, so a tap is always easy to see. Colors are the iOS system colors.
struct ButtonStyle {
	Color back;
	Color text;
	Color border;
	bool strong;   // pressed / flashing / active: thicker rim
};

ButtonStyle buttonStyle(Int index, const ToolbarButton *button)
{
	ButtonStyle style;
	style.back = overlayColor(28, 28, 30, 150);
	style.text = overlayColor(255, 255, 255, 240);
	style.border = overlayColor(255, 255, 255, 110);
	style.strong = false;

	const bool pressed = (index == TAB_INDEX) ? (s_press.target == PRESS_TOOLBAR_TAB && !s_press.moved)
	                                          : (s_press.target == PRESS_TOOLBAR_BUTTON && s_press.index == index && !s_press.moved);
	bool flashLong = false;
	const bool flashing = isFlashing(index, flashLong);

	if (button != nullptr && (button->action == ACTION_CTRL || button->action == ACTION_SHIFT)) {
		const ModifierState state = button->action == ACTION_CTRL ? s_ctrl : s_shift;
		if (state == MODIFIER_ONE_SHOT) {
			style.back = GameMakeColor(0, 122, 255, 235);   // iOS blue: held for the next tap
			style.border = GameMakeColor(255, 255, 255, 230);
			style.strong = true;
		} else if (state == MODIFIER_LOCKED) {
			style.back = GameMakeColor(255, 149, 0, 240);   // iOS orange: locked
			style.border = GameMakeColor(255, 255, 255, 230);
			style.strong = true;
		}
	}
	if (index == TAB_INDEX && s_settings.toolbarOpen) {
		style.back = overlayColor(0, 122, 255, 170);
	}
	if (pressed) {
		style.back = GameMakeColor(255, 255, 255, 240);
		style.text = GameMakeColor(0, 0, 0, 255);
		style.border = GameMakeColor(255, 255, 255, 255);
		style.strong = true;
	}
	if (flashing) {
		style.back = flashLong ? GameMakeColor(52, 199, 89, 245) : GameMakeColor(255, 204, 0, 245);
		style.text = GameMakeColor(0, 0, 0, 255);
		style.border = GameMakeColor(255, 255, 255, 255);
		style.strong = true;
	}
	return style;
}

struct PendingLabel {
	std::string text;
	Rect rect;
	Color color;
};

void drawShape(const Rect &rect, float radius, const ButtonStyle &style, float thinLine, float thickLine)
{
	fillRoundedRect(rect, radius, style.back);
	strokeRoundedRect(rect, radius, style.border, style.strong ? thickLine : thinLine);
}

void drawToolbar(float screenW, float screenH, std::vector<PendingLabel> &labels)
{
	layoutToolbar(screenW, screenH);
	if (!s_layout.valid) {
		return;
	}
	const float thinLine = SDL_max(1.5f, screenH / 600.0f);
	const float thickLine = SDL_max(3.0f, screenH / 250.0f);

	Int count = 0;
	const ToolbarButton *buttons = currentButtons(count);
	for (size_t i = 0; i < s_layout.buttons.size() && (Int)i < count; ++i) {
		const ButtonStyle style = buttonStyle((Int)i, &buttons[i]);
		const Rect &rect = s_layout.buttons[i];
		drawShape(rect, rect.h * 0.5f, style, thinLine, thickLine);
		labels.push_back({ buttonLabel(buttons[i]), rect, style.text });
	}
	for (size_t i = 0; i < s_layout.column.size() && (Int)i < COLUMN_BUTTON_COUNT; ++i) {
		const Int index = COLUMN_BASE + (Int)i;
		const ButtonStyle style = buttonStyle(index, &COLUMN_BUTTONS[i]);
		const Rect &rect = s_layout.column[i];
		drawShape(rect, rect.w * 0.5f, style, thinLine, thickLine);
		labels.push_back({ buttonLabel(COLUMN_BUTTONS[i]), rect, style.text });
	}

	const ButtonStyle tabStyle = buttonStyle(TAB_INDEX, nullptr);
	drawShape(s_layout.tab, s_layout.tab.h * 0.5f, tabStyle, thinLine, thickLine);
	labels.push_back({ s_settings.toolbarOpen ? "Hide" : "Hotkeys", s_layout.tab, tabStyle.text });
}

void drawKeyboardButton(float screenW, float screenH)
{
	if (!s_settings.keyboardVisible) {
		return;
	}
	clampKeyboardButton();

	const float side = screenH * KEYBOARD_BUTTON_SIZE_RATIO * s_settings.scale;
	float centerX = 0.0f, centerY = 0.0f;
	keyboardButtonCenter(centerX, centerY);
	Rect rect;
	rect.w = side;
	rect.h = side;
	rect.x = centerX * screenW - side * 0.5f;
	rect.y = centerY * screenH - side * 0.5f;

	const bool pressing = s_press.target == PRESS_KEYBOARD_BUTTON;
	const bool dragging = pressing && s_press.dragging;
	ButtonStyle style;
	style.back = s_keyboardOpen ? overlayColor(0, 122, 255, 190) : overlayColor(28, 28, 30, 150);
	style.border = overlayColor(255, 255, 255, 110);
	style.strong = false;
	Color key = overlayColor(255, 255, 255, 170);
	if (pressing) {
		style.back = GameMakeColor(255, 255, 255, 240);
		style.border = GameMakeColor(255, 255, 255, 255);
		style.strong = true;
		key = GameMakeColor(0, 0, 0, 220);
	}
	if (dragging) {
		style.back = GameMakeColor(255, 204, 0, 240);
	}
	drawShape(rect, side * 0.25f, style, SDL_max(1.5f, screenH / 600.0f), SDL_max(3.0f, screenH / 250.0f));

	// Keyboard glyph: two rows of four keys and a space bar.
	const Int left = (Int)rect.x;
	const Int top = (Int)rect.y;
	const Int iside = (Int)side;
	const Int pad = iside / 5;
	const Int innerW = iside - 2 * pad;
	const Int gap = SDL_max(2, iside / 24);
	const Int keyW = (innerW - 3 * gap) / 4;
	const Int keyH = (iside - 2 * pad - 2 * gap) / 3;
	for (Int row = 0; row < 2; ++row) {
		for (Int col = 0; col < 4; ++col) {
			TheDisplay->drawFillRect(left + pad + col * (keyW + gap), top + pad + row * (keyH + gap), keyW, keyH, key);
		}
	}
	TheDisplay->drawFillRect(left + pad + keyW + gap, top + pad + 2 * (keyH + gap), 2 * keyW + gap, keyH, key);
}

void resetPress(const char *reason)
{
	if (s_press.target == PRESS_KEYBOARD_BUTTON && s_press.dragging) {
		saveSettings();
	}
	if (reason != nullptr) {
		fprintf(stderr, "INFO: touch overlay: %s, press released\n", reason);
	}
	s_press = PressState();
}

} // anonymous namespace

namespace TouchOverlay {

bool handleFingerEvent(const SDL_Event &event, bool gestureIdle, bool &toggleKeyboard)
{
	toggleKeyboard = false;
	ensureSettings();

	float screenW = 0.0f, screenH = 0.0f;
	if (!screenSize(screenW, screenH)) {
		return false;
	}
	const float x = event.tfinger.x;
	const float y = event.tfinger.y;
	const float px = x * screenW;
	const float py = y * screenH;

	switch (event.type) {
	case SDL_EVENT_FINGER_DOWN:
		{
			if (s_press.target != PRESS_NONE || !gestureIdle) {
				return false;
			}
			PressState press;
			press.touch = event.tfinger.touchID;
			press.finger = event.tfinger.fingerID;
			press.downTicks = SDL_GetTicks();
			press.downX = x;
			press.downY = y;

			if (toolbarAvailable() && s_layout.valid) {
				const float margin = screenH * 0.006f;
				for (size_t i = 0; i < s_layout.buttons.size(); ++i) {
					if (s_layout.buttons[i].contains(px, py, margin)) {
						press.target = PRESS_TOOLBAR_BUTTON;
						press.index = (Int)i;
						break;
					}
				}
				for (size_t i = 0; press.target == PRESS_NONE && i < s_layout.column.size(); ++i) {
					// Circles: hit-test the round shape, a little larger than drawn.
					const Rect &rect = s_layout.column[i];
					const float dx = px - (rect.x + rect.w * 0.5f);
					const float dy = py - (rect.y + rect.h * 0.5f);
					const float radius = rect.w * 0.5f + margin;
					if (dx * dx + dy * dy <= radius * radius) {
						press.target = PRESS_TOOLBAR_BUTTON;
						press.index = COLUMN_BASE + (Int)i;
					}
				}
				if (press.target == PRESS_NONE && s_layout.tab.contains(px, py, screenH * 0.015f)) {
					press.target = PRESS_TOOLBAR_TAB;
				}
			}
			if (press.target == PRESS_NONE && keyboardButtonHit(x, y)) {
				float centerX = 0.0f, centerY = 0.0f;
				keyboardButtonCenter(centerX, centerY);
				press.target = PRESS_KEYBOARD_BUTTON;
				press.grabX = x - centerX;
				press.grabY = y - centerY;
			}
			if (press.target == PRESS_NONE) {
				return false;
			}
			s_press = press;
			return true;
		}

	case SDL_EVENT_FINGER_MOTION:
		if (s_press.target == PRESS_NONE || event.tfinger.fingerID != s_press.finger) {
			return false;
		}
		if (s_press.target == PRESS_KEYBOARD_BUTTON && s_press.dragging) {
			s_settings.keyboardX = x - s_press.grabX;
			s_settings.keyboardY = y - s_press.grabY;
			clampKeyboardButton();
		} else if (SDL_fabsf(x - s_press.downX) + SDL_fabsf(y - s_press.downY) > PRESS_SLOP) {
			s_press.moved = true;
		}
		return true;

	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		{
			if (s_press.target == PRESS_NONE || event.tfinger.fingerID != s_press.finger) {
				return false;
			}
			const bool tapped = event.type == SDL_EVENT_FINGER_UP && !s_press.moved;
			switch (s_press.target) {
			case PRESS_KEYBOARD_BUTTON:
				if (!s_press.dragging && tapped) {
					toggleKeyboard = true;
				}
				break;
			case PRESS_TOOLBAR_TAB:
				if (tapped) {
					s_settings.toolbarOpen = !s_settings.toolbarOpen;
					s_settingsPage = false;
					flashButton(TAB_INDEX, false);
					saveSettings();
				}
				break;
			case PRESS_TOOLBAR_BUTTON:
				if (tapped && !s_press.longPressFired) {
					const ToolbarButton *button = buttonForIndex(s_press.index);
					if (button != nullptr) {
						// Ctrl + group assigns the group: flash it like a long-press assignment.
						const bool assigns = button->action == ACTION_GROUP && s_ctrl != MODIFIER_OFF;
						flashButton(s_press.index, assigns);
						activateToolbarButton(*button, false);
					}
				}
				break;
			default:
				break;
			}
			resetPress(nullptr);
			return true;
		}

	default:
		return false;
	}
}

void update(void)
{
	ensureSettings();
	++s_frame;
	const Uint64 now = SDL_GetTicks();

	// A press whose finger SDL no longer tracks lost its lift event; release it, or it would keep
	// claiming events of a later touch that happens to get the same id.
	if (s_press.target != PRESS_NONE && !fingerIsDown(s_press.touch, s_press.finger)) {
		resetPress("finger no longer on screen");
	}

	// Long-presses: a stationary finger emits no events, so they are polled here.
	if (s_press.target == PRESS_KEYBOARD_BUTTON && !s_press.moved && !s_press.dragging &&
	    now - s_press.downTicks >= KEYBOARD_BUTTON_DRAG_MS) {
		s_press.dragging = true;
	}
	if (s_press.target == PRESS_TOOLBAR_BUTTON && !s_press.moved && !s_press.longPressFired &&
	    now - s_press.downTicks >= TOOLBAR_LONG_PRESS_MS) {
		const ToolbarButton *button = buttonForIndex(s_press.index);
		if (button != nullptr &&
		    (button->action == ACTION_GROUP || button->action == ACTION_CTRL || button->action == ACTION_SHIFT)) {
			flashButton(s_press.index, true);
			activateToolbarButton(*button, true);
			s_press.longPressFired = true;
		}
	}

	// One-shot modifiers are released a couple of frames after the gesture that used them, so the
	// click is turned into a game message while the key still reads as held.
	if (s_releaseOneShotAtFrame != 0 && s_frame >= s_releaseOneShotAtFrame) {
		s_releaseOneShotAtFrame = 0;
		releaseOneShotModifiers();
	}

	if (!toolbarAvailable()) {
		if (s_ctrl != MODIFIER_OFF || s_shift != MODIFIER_OFF) {
			releaseAllModifiers();
		}
		s_layout.valid = false;
	}
}

void draw(void)
{
	ensureSettings();
	float screenW = 0.0f, screenH = 0.0f;
	if (!screenSize(screenW, screenH)) {
		return;
	}

	// Shapes are many small rectangles and lines: batch them into one flush, then draw the labels
	// on top (text is rendered separately, so it must come after the batch is flushed).
	const bool ownBatch = !TheDisplay->isBatching();
	if (ownBatch) {
		TheDisplay->beginBatch();
	}
	drawRings(screenW, screenH);
	std::vector<PendingLabel> labels;
	if (toolbarAvailable()) {
		drawToolbar(screenW, screenH, labels);
	} else {
		s_layout.valid = false;
	}
	drawKeyboardButton(screenW, screenH);
	if (ownBatch) {
		TheDisplay->endBatch();
	} else {
		TheDisplay->flush();
	}

	for (size_t i = 0; i < labels.size(); ++i) {
		drawLabelCentered(labels[i].text, labels[i].rect, labels[i].color);
	}
}

void setKeyboardState(bool open, float fieldTop)
{
	s_keyboardOpen = open;
	s_keyboardFieldTop = fieldTop;
}

void onGameGestureEnded(void)
{
	if (s_ctrl == MODIFIER_ONE_SHOT || s_shift == MODIFIER_ONE_SHOT) {
		s_releaseOneShotAtFrame = s_frame + 2;
	}
}

void addTapFeedback(float x, float y, bool rightClick)
{
	ensureSettings();
	if (!s_settings.tapFeedback) {
		return;
	}
	Ring &ring = s_rings[s_nextRing];
	s_nextRing = (s_nextRing + 1) % RING_COUNT;
	ring.x = x;
	ring.y = y;
	ring.start = SDL_GetTicks();
	ring.right = rightClick;
	ring.active = true;
}

bool fingerIsDown(SDL_TouchID touchID, SDL_FingerID fingerID)
{
	int count = 0;
	SDL_Finger **fingers = SDL_GetTouchFingers(touchID, &count);
	if (fingers == nullptr) {
		return false;
	}
	bool found = false;
	for (int i = 0; i < count; ++i) {
		if (fingers[i] != nullptr && fingers[i]->id == fingerID) {
			found = true;
			break;
		}
	}
	SDL_free(fingers);
	return found;
}

bool doubleTapRightClickEnabled(void)
{
	ensureSettings();
	return s_settings.doubleTapRightClick;
}

bool edgePanEnabled(void)
{
	ensureSettings();
	return s_settings.edgePan;
}

} // namespace TouchOverlay

#endif // TARGET_OS_IPHONE
