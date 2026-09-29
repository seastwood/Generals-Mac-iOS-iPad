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

// GeneralsX @feature seastwood 29/09/2026 On-screen overlay controls for touch devices (iOS):
// the floating keyboard button, the collapsible hotkey toolbar with its settings page, and tap
// feedback rings. SDL3GameEngine feeds it touch events before its gesture translator and calls
// update() once per frame; W3DDisplay::draw() calls draw() after the UI so the controls stay on
// top. Settings are saved to Documents/touch-overlay.ini. Everything here is only defined on iOS
// builds.

#pragma once

#include <SDL3/SDL.h>

namespace TouchOverlay {

/// Offer a finger event to the overlay controls. Returns true when the event belongs to them (the
/// gesture translator must not see it). gestureIdle tells whether the gesture translator is
/// tracking no finger; new overlay presses only start then. Sets toggleKeyboard when a tap on the
/// keyboard button should show or hide the on-screen keyboard.
bool handleFingerEvent(const SDL_Event &event, bool gestureIdle, bool &toggleKeyboard);

/// Per-frame work: long-press timers, toolbar auto-collapse, releasing one-shot modifiers.
void update(void);

/// Draw the overlay controls on top of the frame.
void draw(void);

/// Mirror the engine's on-screen keyboard state. fieldTop is the normalized top of the entry field
/// being typed into, or a negative value when none is.
void setKeyboardState(bool open, float fieldTop);

/// A tap, drag or long-press on the game has finished: one-shot Ctrl/Shift are released.
void onGameGestureEnded(void);

/// Show a feedback ring where a click was delivered (normalized screen coordinates).
void addTapFeedback(float x, float y, bool rightClick);

/// True while SDL still tracks the finger as touching the screen. iOS identifies touches by the
/// address of their UITouch object and reuses those addresses, so a finger whose lift was never
/// delivered must be detected this way, not by waiting for an event with its id.
bool fingerIsDown(SDL_TouchID touchID, SDL_FingerID fingerID);

/// Settings read by the gesture translator.
bool doubleTapRightClickEnabled(void);
bool edgePanEnabled(void);
bool cursorModeEnabled(void);

bool autosaveEnabled(void);

/// Internal render resolution as a fraction of the screen (1.0, 0.75 or 0.5). The first call
/// fixes the value for this run: SDL3Main reads it at launch to choose -xres/-yres.
float renderScale(void);

/// Short haptic tap if enabled in the settings: 0 light, 1 medium, 2 heavy.
void haptic(int strength);

/// Where to draw the cursor (normalized screen coordinates, cursor mode or a game controller), and
/// whether the left button is held.
void setCursorState(bool visible, float x, float y, bool buttonHeld);

} // namespace TouchOverlay
