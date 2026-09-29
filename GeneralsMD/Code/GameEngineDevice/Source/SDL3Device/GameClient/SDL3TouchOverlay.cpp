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
**                     close the toolbar (it stays as left, and is remembered). Buttons send the game's own hotkey commands (no key bindings involved):
**                     All, Same, Stop, Scatter, Home, Alert, groups 1-5 (tap: select, long-press:
**                     assign the current selection), Ctrl / Shift (tap: held for the next tap on
**                     the game, long-press: locked until tapped again), Menu, Opts (settings page)
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

void fillRect(const Rect &rect, Color color)
{
	TheDisplay->drawFillRect((Int)rect.x, (Int)rect.y, (Int)rect.w, (Int)rect.h, color);
}

void outlineRect(const Rect &rect, Color color, float lineWidth)
{
	TheDisplay->drawOpenRect((Int)rect.x, (Int)rect.y, (Int)rect.w, (Int)rect.h, lineWidth, color);
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

const ToolbarButton MAIN_BUTTONS[] = {
	{ "All",     ACTION_META,  GameMessage::MSG_META_SELECT_ALL },
	{ "Same",    ACTION_META,  GameMessage::MSG_META_SELECT_MATCHING_UNITS },
	{ "Stop",    ACTION_META,  GameMessage::MSG_META_STOP },
	{ "Scatter", ACTION_META,  GameMessage::MSG_META_SCATTER },
	{ "Home",    ACTION_META,  GameMessage::MSG_META_VIEW_COMMAND_CENTER },
	{ "Alert",   ACTION_META,  GameMessage::MSG_META_VIEW_LAST_RADAR_EVENT },
	{ "1",       ACTION_GROUP, 1 },
	{ "2",       ACTION_GROUP, 2 },
	{ "3",       ACTION_GROUP, 3 },
	{ "4",       ACTION_GROUP, 4 },
	{ "5",       ACTION_GROUP, 5 },
	{ "Ctrl",    ACTION_CTRL,  0 },
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

const Int MAIN_BUTTON_COUNT = (Int)(sizeof(MAIN_BUTTONS) / sizeof(MAIN_BUTTONS[0]));
const Int SETTINGS_BUTTON_COUNT = (Int)(sizeof(SETTINGS_BUTTONS) / sizeof(SETTINGS_BUTTONS[0]));

const Uint64 TOOLBAR_LONG_PRESS_MS = 500;

bool s_settingsPage = false;

// Press feedback: a tap is often shorter than a frame, so a button that was just activated keeps
// a bright highlight for a moment. Long-press actions (group assigned, key locked) flash green.
const Uint64 FLASH_MS = 250;
struct ButtonFlash {
	Int index = -1;             // toolbar button index, or FLASH_TAB
	bool settingsPage = false;  // page the index belongs to
	bool longPress = false;
	Uint64 until = 0;
};
const Int FLASH_TAB = -2;
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
	if (index != FLASH_TAB && s_flash.settingsPage != s_settingsPage) {
		return false;
	}
	longPress = s_flash.longPress;
	return true;
}

struct ToolbarLayout {
	bool valid = false;
	Rect tab;
	std::vector<Rect> buttons;   // buttons of the current page, empty while collapsed
};
ToolbarLayout s_layout;

const ToolbarButton *currentButtons(Int &count)
{
	if (s_settingsPage) {
		count = SETTINGS_BUTTON_COUNT;
		return SETTINGS_BUTTONS;
	}
	count = MAIN_BUTTON_COUNT;
	return MAIN_BUTTONS;
}

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

	const float rowH = screenH * 0.07f * s_settings.scale;
	// Stay clear of the rounded screen corners: the safe area covers the camera housing on
	// phones, and the extra margin keeps the corner tab off the curve on every device.
	const Insets insets = safeAreaInsets(screenW, screenH);
	const float margin = screenH * 0.02f;
	const float gap = SDL_max(4.0f, rowH * 0.08f);
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
			const float minW = rowH * shrink * (buttons[i].action == ACTION_GROUP ? 0.8f : 1.1f);
			widths[(size_t)i] = SDL_max(minW, (float)textW + rowH * shrink * 0.4f);
			total += widths[(size_t)i];
		}
		total += gap * (float)(count - 1);
		if (total <= screenW - insets.left - insets.right - 2.0f * margin - rowH * 2.4f) {
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
	s_layout.tab.w = SDL_max(rowH * 1.6f, tabTextW + rowH * 0.6f);
	s_layout.tab.h = rowH;
	s_layout.tab.x = screenW - insets.right - s_layout.tab.w - margin;
	s_layout.tab.y = insets.top + margin;

	// The button row sits to the left of the tab, right-aligned against it.
	if (s_settings.toolbarOpen) {
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
	}
	s_layout.valid = true;
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

// Where the button is actually shown. While the keyboard is open it must not sit underneath it, or
// it could not be tapped to close the keyboard again. The keyboard's height is not known here, so
// keep the button in the top part of the screen; when an entry field is being typed into, SDL
// slides the view so the field stays above the keyboard, and just above the field is visible too.
// The saved position is untouched and applies again once the keyboard closes.
void keyboardButtonCenter(float &x, float &y)
{
	x = s_settings.keyboardX;
	y = s_settings.keyboardY;
	float halfW = 0.0f, halfH = 0.0f;
	if (!s_keyboardOpen || !keyboardButtonHalfExtents(halfW, halfH)) {
		return;
	}
	float limit = 0.30f;
	if (s_keyboardFieldTop >= 0.0f) {
		limit = SDL_max(s_keyboardFieldTop - halfH - 0.01f, halfH);
	}
	y = SDL_min(y, limit);
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
	Int index = -1;                // toolbar button index
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

// Colors for a toolbar button. Pressed and just-activated buttons are drawn opaque and bright,
// whatever the opacity setting, so a tap is always easy to see.
void toolbarButtonColors(bool pressed, bool flashing, bool flashLong, Color &back, Color &text, Color &border)
{
	text = overlayColor(255, 255, 255, 235);
	border = overlayColor(255, 255, 255, 120);
	back = overlayColor(20, 20, 20, 120);
	if (pressed) {
		back = GameMakeColor(255, 255, 255, 235);
		text = GameMakeColor(0, 0, 0, 255);
		border = GameMakeColor(255, 255, 255, 255);
	}
	if (flashing) {
		back = flashLong ? GameMakeColor(60, 200, 80, 240) : GameMakeColor(255, 200, 40, 240);
		text = GameMakeColor(0, 0, 0, 255);
		border = GameMakeColor(255, 255, 255, 255);
	}
}

void drawToolbar(float screenW, float screenH)
{
	layoutToolbar(screenW, screenH);
	if (!s_layout.valid) {
		return;
	}
	const float lineWidth = SDL_max(1.0f, screenH / 600.0f);
	const float thickLine = SDL_max(3.0f, screenH / 250.0f);

	Int count = 0;
	const ToolbarButton *buttons = currentButtons(count);
	for (size_t i = 0; i < s_layout.buttons.size() && (Int)i < count; ++i) {
		const ToolbarButton &button = buttons[i];
		const bool pressed = s_press.target == PRESS_TOOLBAR_BUTTON && s_press.index == (Int)i && !s_press.moved;
		bool flashLong = false;
		const bool flashing = isFlashing((Int)i, flashLong);
		Color back, text, border;
		toolbarButtonColors(pressed, flashing, flashLong, back, text, border);
		if (!pressed && !flashing && (button.action == ACTION_CTRL || button.action == ACTION_SHIFT)) {
			const ModifierState state = button.action == ACTION_CTRL ? s_ctrl : s_shift;
			if (state == MODIFIER_ONE_SHOT) back = GameMakeColor(40, 110, 220, 230);
			else if (state == MODIFIER_LOCKED) back = GameMakeColor(230, 130, 20, 235);
		}
		fillRect(s_layout.buttons[i], back);
		outlineRect(s_layout.buttons[i], border, (pressed || flashing) ? thickLine : lineWidth);
		drawLabelCentered(buttonLabel(button), s_layout.buttons[i], text);
	}

	const bool tabPressed = s_press.target == PRESS_TOOLBAR_TAB && !s_press.moved;
	bool tabFlashLong = false;
	const bool tabFlashing = isFlashing(FLASH_TAB, tabFlashLong);
	Color back, text, border;
	toolbarButtonColors(tabPressed, tabFlashing, tabFlashLong, back, text, border);
	if (!tabPressed && !tabFlashing && s_settings.toolbarOpen) {
		back = overlayColor(40, 90, 160, 170);   // open: tinted, like the keyboard button
	}
	fillRect(s_layout.tab, back);
	outlineRect(s_layout.tab, border, (tabPressed || tabFlashing) ? thickLine : lineWidth);
	drawLabelCentered(s_settings.toolbarOpen ? "Hide" : "Hotkeys", s_layout.tab, text);
}

void drawKeyboardButton(float screenW, float screenH)
{
	if (!s_settings.keyboardVisible) {
		return;
	}
	clampKeyboardButton();

	const Int side = (Int)(screenH * KEYBOARD_BUTTON_SIZE_RATIO * s_settings.scale);
	float centerX = 0.0f, centerY = 0.0f;
	keyboardButtonCenter(centerX, centerY);
	const Int left = (Int)(centerX * screenW) - side / 2;
	const Int top = (Int)(centerY * screenH) - side / 2;

	const bool pressing = s_press.target == PRESS_KEYBOARD_BUTTON;
	const bool dragging = pressing && s_press.dragging;
	const float backAlpha = dragging ? 190.0f : (pressing ? 160.0f : 100.0f);
	const Color back = s_keyboardOpen ? overlayColor(40, 90, 160, backAlpha) : overlayColor(20, 20, 20, backAlpha);
	const Color border = overlayColor(255, 255, 255, dragging ? 230.0f : 150.0f);
	const Color key = overlayColor(255, 255, 255, dragging ? 220.0f : 150.0f);

	TheDisplay->drawFillRect(left, top, side, side, back);
	const Real borderWidth = (Real)SDL_max(2, side / 40);
	TheDisplay->drawOpenRect(left, top, side, side, borderWidth, border);

	// Keyboard glyph: two rows of four keys and a space bar.
	const Int pad = side / 5;
	const Int innerW = side - 2 * pad;
	const Int gap = SDL_max(2, side / 24);
	const Int keyW = (innerW - 3 * gap) / 4;
	const Int keyH = (side - 2 * pad - 2 * gap) / 3;
	for (Int row = 0; row < 2; ++row) {
		for (Int col = 0; col < 4; ++col) {
			TheDisplay->drawFillRect(left + pad + col * (keyW + gap), top + pad + row * (keyH + gap),
			                         keyW, keyH, key);
		}
	}
	TheDisplay->drawFillRect(left + pad + keyW + gap, top + pad + 2 * (keyH + gap), 2 * keyW + gap, keyH, key);
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
				if (s_press.dragging) {
					saveSettings();
				} else if (tapped) {
					toggleKeyboard = true;
				}
				break;
			case PRESS_TOOLBAR_TAB:
				if (tapped) {
					s_settings.toolbarOpen = !s_settings.toolbarOpen;
					s_settingsPage = false;
					flashButton(FLASH_TAB, false);
					saveSettings();
				}
				break;
			case PRESS_TOOLBAR_BUTTON:
				if (tapped && !s_press.longPressFired) {
					Int count = 0;
					const ToolbarButton *buttons = currentButtons(count);
					if (s_press.index >= 0 && s_press.index < count) {
						// Ctrl + group assigns the group: flash it like a long-press assignment.
						const bool assigns = buttons[s_press.index].action == ACTION_GROUP && s_ctrl != MODIFIER_OFF;
						flashButton(s_press.index, assigns);
						activateToolbarButton(buttons[s_press.index], false);
					}
				}
				break;
			default:
				break;
			}
			s_press = PressState();
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

	// Long-presses: a stationary finger emits no events, so they are polled here.
	if (s_press.target == PRESS_KEYBOARD_BUTTON && !s_press.moved && !s_press.dragging &&
	    now - s_press.downTicks >= KEYBOARD_BUTTON_DRAG_MS) {
		s_press.dragging = true;
	}
	if (s_press.target == PRESS_TOOLBAR_BUTTON && !s_press.moved && !s_press.longPressFired &&
	    now - s_press.downTicks >= TOOLBAR_LONG_PRESS_MS) {
		Int count = 0;
		const ToolbarButton *buttons = currentButtons(count);
		if (s_press.index >= 0 && s_press.index < count) {
			const ToolbarAction action = buttons[s_press.index].action;
			if (action == ACTION_GROUP || action == ACTION_CTRL || action == ACTION_SHIFT) {
				flashButton(s_press.index, true);
				activateToolbarButton(buttons[s_press.index], true);
				s_press.longPressFired = true;
			}
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
		return;
	}
}

void draw(void)
{
	ensureSettings();
	float screenW = 0.0f, screenH = 0.0f;
	if (!screenSize(screenW, screenH)) {
		return;
	}
	drawRings(screenW, screenH);
	if (toolbarAvailable()) {
		drawToolbar(screenW, screenH);
	} else {
		s_layout.valid = false;
	}
	drawKeyboardButton(screenW, screenH);
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
