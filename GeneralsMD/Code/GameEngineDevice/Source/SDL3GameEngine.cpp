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
** SDL3GameEngine.cpp
**
** Linux implementation of GameEngine using SDL3 for windowing/input.
**
** TheSuperHackers @feature CnC_Generals_Linux 07/02/2026
** Provides SDL3-based input and window management for Linux builds.
** Based on fighter19 reference implementation.
*/

#ifndef _WIN32

#include "SDL3GameEngine.h"
#include "OpenALAudioManager.h"
#include "SDL3Device/GameClient/SDL3Mouse.h"
#include "SDL3Device/GameClient/SDL3Keyboard.h"
#include "GameClient/Mouse.h"
#include "GameClient/Keyboard.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Gadget.h"
#include "W3DDevice/GameLogic/W3DGameLogic.h"
#include "W3DDevice/GameClient/W3DGameClient.h"
#include "W3DDevice/Common/W3DModuleFactory.h"
#include "W3DDevice/Common/W3DThingFactory.h"
#include "W3DDevice/Common/W3DFunctionLexicon.h"
#include "W3DDevice/Common/W3DRadar.h"
#include "W3DDevice/GameClient/W3DParticleSys.h"
#include "W3DDevice/GameClient/W3DWebBrowser.h"
#include "StdDevice/Common/StdLocalFileSystem.h"
#include "StdDevice/Common/StdBIGFileSystem.h"
#include "Common/FramePacer.h"
#include "Common/GameState.h"
#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "GameClient/Display.h"
#include "GameClient/InGameUI.h"
#include "GameClient/View.h"
#include "GameLogic/GameLogic.h"
#include "SDL3Device/GameClient/SDL3TouchOverlay.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// Extern globals for input devices (set by GameClient)
extern Mouse *TheMouse;
extern Keyboard *TheKeyboard;
extern GameWindowManager *TheWindowManager;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#include <atomic>

// ---------------------------------------------------------------------------
// iOS app lifecycle
//
// iOS suspends the process when the app leaves the foreground. Any GPU work
// submitted around suspension stalls on drawable acquisition (MoltenVK waits
// out a timeout per present), which surfaces as multi-second input hangs right
// after resuming. SDL warns that lifecycle events can arrive outside the
// normal poll cycle, so they are captured in an event watcher that fires
// immediately on the delivering thread; the engine update loop checks the
// flag and skips simulation + rendering while backgrounded.
// ---------------------------------------------------------------------------
// Two independent reasons to halt the render/sim loop on iOS:
//  - BACKGROUNDED (home / switched away): the process is about to be suspended.
//  - INACTIVE (multitasking switcher open, Control Center, a notification
//    banner): iOS snapshots the window and owns the CAMetalLayer drawable during
//    this window — and crucially, opening the app switcher fires resign-active
//    WITHOUT a full background transition.
// Acquiring a Metal drawable during EITHER state fights iOS for the layer; across
// repeated suspend/switcher cycles MoltenVK is driven into an unrecoverable
// surface state and the app crashes (the reported "crashes after backgrounding /
// multitasking a few times"). Pause whenever either is set.
static std::atomic<bool> s_appBackgrounded{false};
static std::atomic<bool> s_appInactive{false};

static inline bool iosShouldPauseRendering()
{
	return s_appBackgrounded.load() || s_appInactive.load();
}

// GeneralsX @feature seastwood 29/09/2026 Save a single-player game when the app is sent to the
// background: iOS may terminate a suspended app at any time to reclaim memory, which would lose
// everything since the last manual save. Written to Autosave.sav, which the Load menu lists like
// any other save. It runs synchronously in the lifecycle watcher, i.e. inside UIKit's
// will-resign/enter-background callback, because the app can be suspended before the next frame.
// The watcher fires from SDL's event pump on the main thread, between engine updates.
static void iosAutosaveOnBackground()
{
	static Uint64 s_lastAutosave = 0;
	if (!TouchOverlay::autosaveEnabled() || TheGameState == nullptr || TheGameLogic == nullptr) {
		return;
	}
	if (!TheGameLogic->isInGame() || TheGameLogic->isInShellGame() || TheGameLogic->isLoadingMap() ||
	    TheGameLogic->isInMultiplayerGame() || TheGameLogic->isInReplayGame()) {
		return;
	}
	const Uint64 now = SDL_GetTicks();
	if (s_lastAutosave != 0 && now - s_lastAutosave < 20000) {
		return;   // app switcher flicks in quick succession: one save is enough
	}
	s_lastAutosave = now;
	UnicodeString description;
	description.translate(AsciiString("Autosave"));
	const SaveCode result = TheGameState->saveGame(AsciiString("Autosave.sav"), description, SAVE_FILE_TYPE_NORMAL);
	fprintf(stderr, "INFO: autosave on leaving the app: %s (code %d)\n", result == SC_OK ? "saved" : "failed", (int)result);
}

static bool SDLCALL iosLifecycleWatcher(void *userdata, SDL_Event *event)
{
	// GeneralsX @tweak seastwood 30/09/2026 Log inactive periods: the game is paused while iOS has
	// focus (Control Center, Notification Center, app switcher), which looks like an input hang.
	static Uint64 s_inactiveSince = 0;
	switch (event->type) {
		case SDL_EVENT_WILL_ENTER_BACKGROUND:
			iosAutosaveOnBackground();
			s_appBackgrounded.store(true);
			break;
		case SDL_EVENT_DID_ENTER_BACKGROUND:
			s_appBackgrounded.store(true);
			break;
		case SDL_EVENT_DID_ENTER_FOREGROUND:
			s_appBackgrounded.store(false);
			break;
		// Resign/become active. On iOS, SDL maps applicationWillResignActive ->
		// window focus lost and applicationDidBecomeActive -> window focus gained.
		// Stay paused until fully active again (focus regained), which arrives
		// after DID_ENTER_FOREGROUND.
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			if (!s_appInactive.load()) {
				s_inactiveSince = SDL_GetTicks();
				fprintf(stderr, "INFO: app inactive (iOS took focus), game paused\n");
			}
			s_appInactive.store(true);
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			if (s_appInactive.load()) {
				fprintf(stderr, "INFO: app active again after %u ms\n", (unsigned)(SDL_GetTicks() - s_inactiveSince));
			}
			s_appInactive.store(false);
			break;
		default:
			break;
	}
	return true;
}

// ---------------------------------------------------------------------------
// iOS touch -> mouse gesture translation
//
// SDL's automatic touch-mouse synthesis is disabled on iOS (SDL3Main.cpp sets
// SDL_HINT_TOUCH_MOUSE_EVENTS=0); every mouse event the game sees on iOS is
// synthesized here, through the same SDL3Mouse::addSDLEvent path real mice use.
//
// Gestures (matching the game's stock control scheme, which is LMB-centric):
//   1 finger tap/drag     -> left button click / drag (select, command, drag-box)
//   1 finger long-press   -> right button click (deselect), if finger stays put
//   2 finger drag         -> right-button drag at the centroid (camera scroll)
//   2 finger pinch        -> mouse wheel (camera zoom)
// ---------------------------------------------------------------------------
namespace {

struct TouchState {
	enum Phase {
		IDLE,        // no fingers tracked
		PENDING,     // finger1 down, gesture identity not yet known, nothing sent
		DRAGGING,    // finger1 drag in progress, LMB held
		LONGPRESSED, // long-press fired (RMB click sent), swallow until lift
		PAN          // two-finger camera pan, RMB held
	};

	Phase phase = IDLE;
	SDL_TouchID touch = 0;                // touch device finger1 belongs to
	SDL_FingerID finger1 = 0;
	SDL_FingerID finger2 = 0;
	float downX = 0.0f, downY = 0.0f;   // finger1 down position (window points)
	float lastX = 0.0f, lastY = 0.0f;   // finger1 latest position
	float panX = 0.0f, panY = 0.0f;     // pan centroid
	float pinchDist = 0.0f;             // finger distance at last wheel step
	Uint64 downTicks = 0;
	float f1x = 0.0f, f1y = 0.0f, f2x = 0.0f, f2y = 0.0f; // normalized per finger
	Uint64 lastTapTicks = 0;              // previous clean tap, for double-tap right-click
	float lastTapX = 0.0f, lastTapY = 0.0f;
	float stillX = 0.0f, stillY = 0.0f;   // finger1 position when it last moved noticeably
	Uint64 stillSince = 0;
};

TouchState s_touch;

const Uint64 LONG_PRESS_MS = 600;
const float PINCH_STEP_RATIO = 0.06f;  // 6% distance change per wheel tick
const float TAP_DEAD_ZONE_PX = 8.0f;   // jitter below this keeps a tap a tap
// GeneralsX @feature seastwood 29/09/2026 Double-tap right-click (switchable from the toolbar's
// settings page, see SDL3TouchOverlay.cpp).
const Uint64 DOUBLE_TAP_MS = 300;
const float DOUBLE_TAP_SLOP_PX = 30.0f;
// GeneralsX @bugfix seastwood 30/09/2026 Resting fingers (see touchYieldToNewFinger).
const Uint64 TWO_FINGER_WINDOW_MS = 350; // a second finger later than this starts a new touch, not a pan
const Uint64 RESTING_FINGER_MS = 800;    // a dragging finger that stayed put this long is resting
const float STILL_RADIUS_PX = 6.0f;      // movement within this radius counts as staying put

void sendSyntheticMouse(SDL3Mouse *mouse, SDL_Window *window, Uint32 type,
                        float x, float y, Uint8 button = 0, float wheelY = 0.0f)
{
	// The windowID must be valid: SDL3Mouse::scaleMouseCoordinates() looks the
	// window up by id to map window points into the game's internal resolution,
	// and silently skips scaling when the lookup fails.
	const SDL_WindowID windowID = SDL_GetWindowID(window);

	SDL_Event ev;
	SDL_zero(ev);
	ev.type = type;
	switch (type) {
		case SDL_EVENT_MOUSE_MOTION:
			ev.motion.windowID = windowID;
			ev.motion.x = x;
			ev.motion.y = y;
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			ev.button.windowID = windowID;
			ev.button.button = button;
			ev.button.down = (type == SDL_EVENT_MOUSE_BUTTON_DOWN);
			ev.button.clicks = 1;
			ev.button.x = x;
			ev.button.y = y;
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			ev.wheel.windowID = windowID;
			ev.wheel.x = 0.0f;
			ev.wheel.y = wheelY;
			ev.wheel.mouse_x = x;
			ev.wheel.mouse_y = y;
			break;
	}
	mouse->addSDLEvent(&ev);
}

void beginPan(SDL3Mouse *mouse, SDL_Window *window, int winW, int winH)
{
	s_touch.panX = (s_touch.f1x + s_touch.f2x) * 0.5f * (float)winW;
	s_touch.panY = (s_touch.f1y + s_touch.f2y) * 0.5f * (float)winH;
	const float dx = (s_touch.f1x - s_touch.f2x) * (float)winW;
	const float dy = (s_touch.f1y - s_touch.f2y) * (float)winH;
	s_touch.pinchDist = SDL_sqrtf(dx * dx + dy * dy);
	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.panX, s_touch.panY);
	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
	                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
	s_touch.phase = TouchState::PAN;
}

// GeneralsX @bugfix seastwood 30/09/2026 A new touch takes over from a finger resting on the glass.
// Holding a phone in landscape, a thumb or the base of the palm often lies on the screen edge. That
// is a real touch, so SDL keeps reporting it as down and the lost-lift recovery cannot catch it: it
// became a long-press or a drag, and every other touch was ignored until it lifted.
// Input seemed to hang for as long as the hand stayed there. A finger that already finished its
// action or has stayed put is now abandoned when another finger lands (its later events no longer
// match the tracked finger and are ignored). Returns true when the translator is free for the new
// touch. A second finger soon after the first is still a two-finger pan.
bool touchYieldToNewFinger(SDL3Mouse *mouse, SDL_Window *window)
{
	const Uint64 now = SDL_GetTicks();
	bool resting = false;
	switch (s_touch.phase) {
		case TouchState::IDLE:
			return true;
		case TouchState::LONGPRESSED:
			resting = true;
			break;
		case TouchState::PENDING:
			resting = now - s_touch.downTicks >= TWO_FINGER_WINDOW_MS;
			break;
		case TouchState::DRAGGING:
			resting = now - s_touch.stillSince >= RESTING_FINGER_MS;
			break;
		default:
			break;   // two-finger pan: extra fingers are ignored until it ends
	}
	if (!resting) {
		return false;
	}
	fprintf(stderr, "INFO: touch: new touch takes over from a finger resting on the screen (gesture phase %d, down %u ms)\n",
	        (int)s_touch.phase, (unsigned)(now - s_touch.downTicks));
	if (s_touch.phase == TouchState::DRAGGING) {
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
	}
	if (s_touch.phase == TouchState::DRAGGING || s_touch.phase == TouchState::LONGPRESSED) {
		TouchOverlay::onGameGestureEnded();
	}
	s_touch.phase = TouchState::IDLE;
	return true;
}

void handleTouchEvent(SDL3Mouse *mouse, SDL_Window *window, const SDL_Event &event)
{
	int winW = 0, winH = 0;
	SDL_GetWindowSize(window, &winW, &winH);
	const float px = event.tfinger.x * (float)winW;
	const float py = event.tfinger.y * (float)winH;

	switch (event.type) {
	case SDL_EVENT_FINGER_DOWN:
		touchYieldToNewFinger(mouse, window);
		if (s_touch.phase == TouchState::IDLE) {
			// Defer all BUTTON output: a finger landing could become a tap, a
			// drag-box, a long-press, or the first finger of a camera pan. A
			// premature LMB down+up is a real click to the game (e.g. it sets a
			// rally point when a production building is selected).
			s_touch.touch = event.tfinger.touchID;
			s_touch.finger1 = event.tfinger.fingerID;
			s_touch.phase = TouchState::PENDING;
			s_touch.downX = s_touch.lastX = px;
			s_touch.downY = s_touch.lastY = py;
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.downTicks = SDL_GetTicks();
			s_touch.stillX = px;
			s_touch.stillY = py;
			s_touch.stillSince = s_touch.downTicks;
			// Move the cursor to the touch point NOW (motion clicks nothing, so the
			// deferred-tap protection is intact). This lets the GUI process hover
			// over the next frame(s) before the tap commits — hover-driven widgets
			// (e.g. the Generals Challenge general buttons, which are checkboxes
			// that ignore a click unless WIN_STATE_HILITED was set by a prior
			// mouse-enter) then accept the click. Real mice hover before clicking;
			// without this, a synthetic tap teleports + clicks in one instant and
			// the widget is never hilited, so only the default/first item responds.
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
		}
		else if (s_touch.phase == TouchState::PENDING) {
			// Second finger before the first committed to anything: pure pan,
			// no left-click ever happened.
			s_touch.finger2 = event.tfinger.fingerID;
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
			beginPan(mouse, window, winW, winH);
		}
		else if (s_touch.phase == TouchState::DRAGGING) {
			// Second finger during a live drag: finish the drag-box, then pan.
			s_touch.finger2 = event.tfinger.fingerID;
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
			                   s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
			beginPan(mouse, window, winW, winH);
		}
		// LONGPRESSED / PAN with extra fingers: ignored
		break;

	case SDL_EVENT_FINGER_MOTION:
		if (event.tfinger.fingerID == s_touch.finger1) {
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.lastX = px;
			s_touch.lastY = py;
			if (SDL_fabsf(px - s_touch.stillX) + SDL_fabsf(py - s_touch.stillY) > STILL_RADIUS_PX) {
				s_touch.stillX = px;
				s_touch.stillY = py;
				s_touch.stillSince = SDL_GetTicks();
			}
		} else if (s_touch.phase == TouchState::PAN && event.tfinger.fingerID == s_touch.finger2) {
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
		} else {
			break;
		}

		if (s_touch.phase == TouchState::PENDING && event.tfinger.fingerID == s_touch.finger1) {
			const float moved = SDL_fabsf(px - s_touch.downX) + SDL_fabsf(py - s_touch.downY);
			if (moved >= TAP_DEAD_ZONE_PX) {
				// Commit to a drag: anchor the LMB at the original touch point so
				// drag-boxes start where the finger first landed.
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
				s_touch.phase = TouchState::DRAGGING;
			}
		}
		else if (s_touch.phase == TouchState::DRAGGING && event.tfinger.fingerID == s_touch.finger1) {
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
		}
		else if (s_touch.phase == TouchState::PAN) {
			const float cx = (s_touch.f1x + s_touch.f2x) * 0.5f * (float)winW;
			const float cy = (s_touch.f1y + s_touch.f2y) * 0.5f * (float)winH;
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, cx, cy);
			s_touch.panX = cx;
			s_touch.panY = cy;

			const float dx = (s_touch.f1x - s_touch.f2x) * (float)winW;
			const float dy = (s_touch.f1y - s_touch.f2y) * (float)winH;
			const float dist = SDL_sqrtf(dx * dx + dy * dy);
			if (s_touch.pinchDist > 1.0f) {
				const float ratio = dist / s_touch.pinchDist;
				if (ratio > 1.0f + PINCH_STEP_RATIO) {
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_WHEEL, cx, cy, 0, 1.0f);
					s_touch.pinchDist = dist;
				} else if (ratio < 1.0f - PINCH_STEP_RATIO) {
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_WHEEL, cx, cy, 0, -1.0f);
					s_touch.pinchDist = dist;
				}
			}
		}
		break;

	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		if (event.tfinger.fingerID != s_touch.finger1 &&
		    !(s_touch.phase == TouchState::PAN && event.tfinger.fingerID == s_touch.finger2)) {
			break;
		}
		switch (s_touch.phase) {
			case TouchState::PENDING:
				// A CANCELED touch (incoming call, notification shade, palm
				// rejection) must not become a committed tap — that would be a
				// phantom select/command/rally-point click at the cancel point.
				if (event.type == SDL_EVENT_FINGER_CANCELED) {
					break;
				}
				{
					// Clean tap: deliver the full click at the exact press position. With
					// double-tap right-click enabled, a second tap close in time and place
					// is a right click instead (the first one was already a left click).
					const Uint64 now = SDL_GetTicks();
					const bool rightClick = TouchOverlay::doubleTapRightClickEnabled() &&
						s_touch.lastTapTicks != 0 && now - s_touch.lastTapTicks <= DOUBLE_TAP_MS &&
						SDL_fabsf(s_touch.downX - s_touch.lastTapX) + SDL_fabsf(s_touch.downY - s_touch.lastTapY) <= DOUBLE_TAP_SLOP_PX;
					const Uint8 button = rightClick ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
					                   s_touch.downX, s_touch.downY, button);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
					                   s_touch.downX, s_touch.downY, button);
					s_touch.lastTapTicks = rightClick ? 0 : now;
					s_touch.lastTapX = s_touch.downX;
					s_touch.lastTapY = s_touch.downY;
					if (winW > 0 && winH > 0) {
						TouchOverlay::addTapFeedback(s_touch.downX / (float)winW, s_touch.downY / (float)winH, rightClick);
					}
					TouchOverlay::onGameGestureEnded();
				}
				break;
			case TouchState::DRAGGING:
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, px, py, SDL_BUTTON_LEFT);
				TouchOverlay::onGameGestureEnded();
				break;
			case TouchState::LONGPRESSED:
				TouchOverlay::onGameGestureEnded();
				break;
			case TouchState::PAN:
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
				break;
			default:
				break;
		}
		s_touch.phase = TouchState::IDLE;
		break;
	}
}

// Called once per engine frame (not just per touch event): a perfectly
// stationary finger produces no SDL events, so the long-press timer must be
// polled from the frame loop or it would never fire.
void updateTouchLongPress(SDL3Mouse *mouse, SDL_Window *window)
{
	// GeneralsX @bugfix seastwood 29/09/2026 Recover from a lift that was never delivered. SDL on
	// iOS identifies a finger by the address of its UITouch object, and UIKit reuses those
	// addresses. When a finger's up event went missing, the gesture stayed in LONGPRESSED / PAN,
	// ignored every new touch, and only recovered once a later touch happened to reuse
	// the same address and lift: touch input "stopped" for a random 10-30 seconds while the game
	// kept running. Compare against the fingers SDL still tracks instead of waiting for an event.
	if (s_touch.phase != TouchState::IDLE && !TouchOverlay::fingerIsDown(s_touch.touch, s_touch.finger1)) {
		fprintf(stderr, "INFO: touch: finger lifted without an up event (gesture phase %d), resetting\n",
		        (int)s_touch.phase);
		if (s_touch.phase == TouchState::DRAGGING) {
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
			TouchOverlay::onGameGestureEnded();
		} else if (s_touch.phase == TouchState::PAN) {
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
		}
		s_touch.phase = TouchState::IDLE;
	}

	if (s_touch.phase == TouchState::PENDING) {
		const Uint64 held = SDL_GetTicks() - s_touch.downTicks;
		if (held >= LONG_PRESS_MS) {
			// No LMB was sent yet (deferred), so this is a pure right-click.
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
			                   s_touch.downX, s_touch.downY, SDL_BUTTON_RIGHT);
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
			                   s_touch.downX, s_touch.downY, SDL_BUTTON_RIGHT);
			s_touch.phase = TouchState::LONGPRESSED;
			TouchOverlay::addTapFeedback(s_touch.f1x, s_touch.f1y, true);
			TouchOverlay::haptic(0);   // the right click fired: the finger can lift
		}
	}

}

// ---------------------------------------------------------------------------
// Cursor mode (trackpad)
//
// GeneralsX @feature seastwood 29/09/2026 Optional alternative to direct touch, switched in the
// toolbar's settings page. The screen works like a laptop trackpad driving a visible cursor:
//   1 finger slide            -> move the cursor (accelerated); a flick keeps it gliding
//                                (inertia) until a finger touches the screen again
//   1 finger tap              -> left click at the cursor
//   1 finger hold, then slide -> left-button drag at the cursor (selection box, drag)
//   2 finger tap              -> right click at the cursor
//   2 finger slide            -> scroll the camera; pinch -> zoom
//   cursor at a screen edge   -> scroll the camera, like pushing the mouse against it on PC
// Positions are window points, like the gesture translator above.
// ---------------------------------------------------------------------------
struct CursorState {
	enum Phase {
		IDLE,        // no finger on the game
		ONE,         // one finger: moving the cursor, may still become a tap or a hold-drag
		HOLD,        // finger held still: a slide now drags, a lift clicks; no button sent yet
		DRAG,        // hold-drag: left button held while the finger moves the cursor
		TWO,         // two fingers: may become a right click, scrolls/zooms when they move
		WAIT_LIFT    // gesture finished, remaining finger is ignored until it lifts
	};

	Phase phase = IDLE;
	bool initialized = false;
	float x = 0.0f, y = 0.0f;          // cursor position
	float vx = 0.0f, vy = 0.0f;        // cursor velocity (points per second) for inertia
	SDL_TouchID touch = 0;
	SDL_FingerID finger1 = 0, finger2 = 0;
	float f1x = 0.0f, f1y = 0.0f, f2x = 0.0f, f2y = 0.0f;   // finger positions
	Uint64 downTicks = 0;
	Uint64 lastMoveNS = 0;             // event time of the last finger movement
	float travel = 0.0f;               // finger travel since the gesture started
	float pinchDist = 0.0f;
	Uint64 lastFrameTicks = 0;
	float stillX = 0.0f, stillY = 0.0f;   // finger1 position when it last moved noticeably
	Uint64 stillSince = 0;
};

CursorState s_cursor;
bool s_cursorModeActive = false;

const float CURSOR_TAP_SLOP = 10.0f;          // finger travel that is still a tap
const Uint64 CURSOR_TAP_MS = 300;
const Uint64 CURSOR_HOLD_DRAG_MS = 450;       // hold still this long to start a drag
const float CURSOR_GAIN = 1.2f;               // cursor points per finger point, slow moves
const float CURSOR_ACCEL_SPEED = 800.0f;      // finger speed (points/s) that doubles the gain
const float CURSOR_MAX_GAIN = 3.5f;
const float CURSOR_FRICTION = 0.93f;          // velocity kept per 1/60 s of gliding
const float CURSOR_MIN_SPEED = 20.0f;         // below this a glide stops
const float CURSOR_THROW_SPEED = 150.0f;      // release speed that starts a glide
const float CURSOR_MAX_GLIDE = 1.5f;          // fastest glide, in screen widths per second
const float CURSOR_MIN_DT = 1.0f / 240.0f;    // shortest interval used to measure finger speed
// Two-finger scroll: camera offset per display pixel of finger movement. View::scrollBy maps its
// offset through the view plane independently of resolution; this keeps the map roughly under
// the fingers at the default camera height.
const float CURSOR_PAN_GAIN = 2.5f;
const float CURSOR_EDGE = 0.01f;              // normalized edge band that scrolls the camera
const float CURSOR_EDGE_SCROLL_SPEED = 200.0f; // matches the game's keyboard scroll amount
const Uint64 CURSOR_RESTING_MS = 1000;        // a cursor finger that stayed put this long is resting

bool cursorGestureIdle()
{
	return s_cursor.phase == CursorState::IDLE;
}

void cursorClamp(int winW, int winH)
{
	const float maxX = (float)SDL_max(winW - 1, 0);
	const float maxY = (float)SDL_max(winH - 1, 0);
	if (s_cursor.x <= 0.0f || s_cursor.x >= maxX) {
		s_cursor.vx = 0.0f;
	}
	if (s_cursor.y <= 0.0f || s_cursor.y >= maxY) {
		s_cursor.vy = 0.0f;
	}
	s_cursor.x = SDL_clamp(s_cursor.x, 0.0f, maxX);
	s_cursor.y = SDL_clamp(s_cursor.y, 0.0f, maxY);
}

void cursorPublish(int winW, int winH)
{
	if (winW > 0 && winH > 0) {
		TouchOverlay::setCursorState(true, s_cursor.x / (float)winW, s_cursor.y / (float)winH,
		                             s_cursor.phase == CursorState::HOLD || s_cursor.phase == CursorState::DRAG);
	}
}

void cursorClick(SDL3Mouse *mouse, SDL_Window *window, Uint8 button, int winW, int winH)
{
	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_cursor.x, s_cursor.y);
	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN, s_cursor.x, s_cursor.y, button);
	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_cursor.x, s_cursor.y, button);
	if (winW > 0 && winH > 0) {
		TouchOverlay::addTapFeedback(s_cursor.x / (float)winW, s_cursor.y / (float)winH, button == SDL_BUTTON_RIGHT);
	}
	TouchOverlay::onGameGestureEnded();
}

bool cameraControlAvailable()
{
	return TheGameLogic != nullptr && TheGameLogic->isInGame() && !TheGameLogic->isInShellGame() &&
		TheTacticalView != nullptr && TheInGameUI != nullptr && TheInGameUI->getInputEnabled();
}

// Scroll by a finger movement in window points: the map follows the fingers.
void cursorPanCamera(float dx, float dy, int winW, int winH)
{
	if (!cameraControlAvailable() || TheDisplay == nullptr || winW <= 0 || winH <= 0) {
		return;
	}
	const float pixelsPerPointX = (float)TheDisplay->getWidth() / (float)winW;
	const float pixelsPerPointY = (float)TheDisplay->getHeight() / (float)winH;
	Coord2D offset;
	offset.x = -dx * pixelsPerPointX * CURSOR_PAN_GAIN;
	offset.y = -dy * pixelsPerPointY * CURSOR_PAN_GAIN;
	TheTacticalView->userScrollBy(&offset);
}

// Cursor resting against a screen edge scrolls the camera that way, like the mouse on PC (cursor
// mode and the controller). The game's own screen-edge scroll is off on iOS (SDL3Mouse never
// captures there), because a direct tap near an edge would leave it scrolling.
void scrollAtCursorEdge(int winW, int winH)
{
	if (!cameraControlAvailable() || winW <= 0 || winH <= 0) {
		return;
	}
	const float nx = s_cursor.x / (float)winW;
	const float ny = s_cursor.y / (float)winH;
	const float dirX = nx <= CURSOR_EDGE ? -1.0f : (nx >= 1.0f - CURSOR_EDGE ? 1.0f : 0.0f);
	const float dirY = ny <= CURSOR_EDGE ? -1.0f : (ny >= 1.0f - CURSOR_EDGE ? 1.0f : 0.0f);
	if (dirX == 0.0f && dirY == 0.0f) {
		return;
	}
	const Real fpsRatio = TheFramePacer != nullptr ? TheFramePacer->getBaseOverUpdateFpsRatio() : 1.0f;
	const Real amount = CURSOR_EDGE_SCROLL_SPEED * fpsRatio * TheGlobalData->m_keyboardScrollFactor;
	Coord2D offset;
	offset.x = dirX * TheGlobalData->m_horizontalScrollSpeedFactor * amount;
	offset.y = dirY * TheGlobalData->m_verticalScrollSpeedFactor * amount;
	TheTacticalView->userScrollBy(&offset);
}

// GeneralsX @bugfix seastwood 30/09/2026 Same resting-finger takeover as the direct-touch translator
// (see touchYieldToNewFinger). A resting thumb became a hold-drag with the left button held, and
// every other touch was ignored until it lifted.
bool cursorYieldToNewFinger(SDL3Mouse *mouse, SDL_Window *window)
{
	const Uint64 now = SDL_GetTicks();
	bool resting = false;
	switch (s_cursor.phase) {
		case CursorState::IDLE:
			return true;
		case CursorState::HOLD:
		case CursorState::WAIT_LIFT:
			resting = true;
			break;
		case CursorState::ONE:
			// A second finger normally makes a two-finger gesture, also while the first one rests on
			// a target it just moved the cursor to; only a finger left there much longer is resting.
			resting = now - s_cursor.stillSince >= CURSOR_RESTING_MS;
			break;
		case CursorState::DRAG:
			resting = now - s_cursor.stillSince >= RESTING_FINGER_MS;
			break;
		default:
			break;   // two fingers: extra fingers are ignored until they lift
	}
	if (!resting) {
		return false;
	}
	fprintf(stderr, "INFO: touch: new touch takes over from a cursor finger resting on the screen (phase %d, down %u ms)\n",
	        (int)s_cursor.phase, (unsigned)(now - s_cursor.downTicks));
	if (s_cursor.phase == CursorState::DRAG) {
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_cursor.x, s_cursor.y, SDL_BUTTON_LEFT);
		TouchOverlay::onGameGestureEnded();
	}
	s_cursor.phase = CursorState::IDLE;
	return true;
}

// Returns true when the gesture produced a left click (the engine then updates the on-screen
// keyboard for a click at the cursor, as it does for a direct tap).
bool handleCursorTouchEvent(SDL3Mouse *mouse, SDL_Window *window, const SDL_Event &event)
{
	int winW = 0, winH = 0;
	SDL_GetWindowSize(window, &winW, &winH);
	const float px = event.tfinger.x * (float)winW;
	const float py = event.tfinger.y * (float)winH;
	const Uint64 now = SDL_GetTicks();
	// When the finger event happened (iOS may deliver several per frame, or late).
	const Uint64 nowNS = SDL_GetTicksNS();
	const Uint64 eventNS = (event.common.timestamp != 0 && event.common.timestamp <= nowNS) ? event.common.timestamp : nowNS;
	bool leftClicked = false;

	switch (event.type) {
	case SDL_EVENT_FINGER_DOWN:
		cursorYieldToNewFinger(mouse, window);
		if (s_cursor.phase == CursorState::IDLE) {
			// A touch stops a gliding cursor, like putting a hand on a trackball.
			s_cursor.vx = 0.0f;
			s_cursor.vy = 0.0f;
			s_cursor.touch = event.tfinger.touchID;
			s_cursor.finger1 = event.tfinger.fingerID;
			s_cursor.f1x = px;
			s_cursor.f1y = py;
			s_cursor.downTicks = now;
			s_cursor.lastMoveNS = eventNS;
			s_cursor.travel = 0.0f;
			s_cursor.stillX = px;
			s_cursor.stillY = py;
			s_cursor.stillSince = now;
			s_cursor.phase = CursorState::ONE;
		} else if (s_cursor.phase == CursorState::ONE) {
			s_cursor.finger2 = event.tfinger.fingerID;
			s_cursor.f2x = px;
			s_cursor.f2y = py;
			s_cursor.downTicks = now;
			s_cursor.travel = 0.0f;
			s_cursor.pinchDist = SDL_sqrtf((s_cursor.f1x - px) * (s_cursor.f1x - px) + (s_cursor.f1y - py) * (s_cursor.f1y - py));
			s_cursor.phase = CursorState::TWO;
		}
		// DRAG / TWO / WAIT_LIFT with extra fingers: ignored
		break;

	case SDL_EVENT_FINGER_MOTION:
		if ((s_cursor.phase == CursorState::ONE || s_cursor.phase == CursorState::HOLD ||
		     s_cursor.phase == CursorState::DRAG) &&
		    event.tfinger.fingerID == s_cursor.finger1) {
			const float dx = px - s_cursor.f1x;
			const float dy = py - s_cursor.f1y;
			s_cursor.f1x = px;
			s_cursor.f1y = py;
			s_cursor.travel += SDL_fabsf(dx) + SDL_fabsf(dy);
			if (SDL_fabsf(px - s_cursor.stillX) + SDL_fabsf(py - s_cursor.stillY) > STILL_RADIUS_PX) {
				s_cursor.stillX = px;
				s_cursor.stillY = py;
				s_cursor.stillSince = now;
			}
			if (s_cursor.phase == CursorState::HOLD) {
				if (s_cursor.travel <= 2.0f * CURSOR_TAP_SLOP) {
					s_cursor.lastMoveNS = eventNS;
					break;   // jitter of a held finger: the cursor stays on its target
				}
				// The held finger slides: the drag starts where the cursor is.
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_cursor.x, s_cursor.y);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN, s_cursor.x, s_cursor.y, SDL_BUTTON_LEFT);
				s_cursor.phase = CursorState::DRAG;
			}
			// GeneralsX @bugfix seastwood 30/09/2026 Measure finger speed with the touch's own
			// timestamp. The screen samples fingers faster than the game polls, so several movements
			// arrive in one frame; timed by the poll they looked instantaneous (1 ms), which maxed
			// the acceleration and flung the cursor on release.
			const float dt = SDL_clamp((float)(eventNS - s_cursor.lastMoveNS) / 1e9f, CURSOR_MIN_DT, 0.1f);
			s_cursor.lastMoveNS = eventNS;
			const float fingerSpeed = SDL_sqrtf(dx * dx + dy * dy) / dt;
			const float gain = SDL_min(CURSOR_MAX_GAIN, CURSOR_GAIN * (1.0f + fingerSpeed / CURSOR_ACCEL_SPEED)) *
				TouchOverlay::cursorSensitivity();
			s_cursor.x += dx * gain;
			s_cursor.y += dy * gain;
			// Smoothed velocity for the glide after release, capped so a flick stays controllable.
			s_cursor.vx = s_cursor.vx * 0.6f + (dx * gain / dt) * 0.4f;
			s_cursor.vy = s_cursor.vy * 0.6f + (dy * gain / dt) * 0.4f;
			const float glide = SDL_sqrtf(s_cursor.vx * s_cursor.vx + s_cursor.vy * s_cursor.vy);
			const float maxGlide = CURSOR_MAX_GLIDE * (float)winW * TouchOverlay::cursorSensitivity();
			if (glide > maxGlide) {
				s_cursor.vx *= maxGlide / glide;
				s_cursor.vy *= maxGlide / glide;
			}
			cursorClamp(winW, winH);
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_cursor.x, s_cursor.y);
		} else if (s_cursor.phase == CursorState::TWO &&
		           (event.tfinger.fingerID == s_cursor.finger1 || event.tfinger.fingerID == s_cursor.finger2)) {
			const float oldCx = (s_cursor.f1x + s_cursor.f2x) * 0.5f;
			const float oldCy = (s_cursor.f1y + s_cursor.f2y) * 0.5f;
			if (event.tfinger.fingerID == s_cursor.finger1) {
				s_cursor.f1x = px;
				s_cursor.f1y = py;
			} else {
				s_cursor.f2x = px;
				s_cursor.f2y = py;
			}
			const float dx = (s_cursor.f1x + s_cursor.f2x) * 0.5f - oldCx;
			const float dy = (s_cursor.f1y + s_cursor.f2y) * 0.5f - oldCy;
			s_cursor.travel += SDL_fabsf(dx) + SDL_fabsf(dy);
			if (s_cursor.travel > CURSOR_TAP_SLOP) {
				cursorPanCamera(dx, dy, winW, winH);
			}
			const float dist = SDL_sqrtf((s_cursor.f1x - s_cursor.f2x) * (s_cursor.f1x - s_cursor.f2x) +
			                             (s_cursor.f1y - s_cursor.f2y) * (s_cursor.f1y - s_cursor.f2y));
			if (s_cursor.pinchDist > 1.0f) {
				const float ratio = dist / s_cursor.pinchDist;
				if (ratio > 1.0f + PINCH_STEP_RATIO || ratio < 1.0f - PINCH_STEP_RATIO) {
					s_cursor.travel += CURSOR_TAP_SLOP;   // a pinch is not a tap
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_WHEEL, s_cursor.x, s_cursor.y, 0,
					                   ratio > 1.0f ? 1.0f : -1.0f);
					s_cursor.pinchDist = dist;
				}
			}
		}
		break;

	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		{
			const bool canceled = event.type == SDL_EVENT_FINGER_CANCELED;
			switch (s_cursor.phase) {
			case CursorState::ONE:
				if (event.tfinger.fingerID != s_cursor.finger1) {
					break;
				}
				if (!canceled && s_cursor.travel <= CURSOR_TAP_SLOP && now - s_cursor.downTicks <= CURSOR_TAP_MS) {
					s_cursor.vx = 0.0f;
					s_cursor.vy = 0.0f;
					cursorClick(mouse, window, SDL_BUTTON_LEFT, winW, winH);
					leftClicked = true;
				} else if (canceled || eventNS - s_cursor.lastMoveNS > 60000000 ||
				           SDL_sqrtf(s_cursor.vx * s_cursor.vx + s_cursor.vy * s_cursor.vy) < CURSOR_THROW_SPEED) {
					// The finger stopped before lifting: no glide.
					s_cursor.vx = 0.0f;
					s_cursor.vy = 0.0f;
				}
				s_cursor.phase = CursorState::IDLE;
				break;
			case CursorState::HOLD:
				if (event.tfinger.fingerID != s_cursor.finger1) {
					break;
				}
				// Held and lifted without sliding: a left click.
				if (!canceled) {
					cursorClick(mouse, window, SDL_BUTTON_LEFT, winW, winH);
					leftClicked = true;
				}
				s_cursor.vx = 0.0f;
				s_cursor.vy = 0.0f;
				s_cursor.phase = CursorState::IDLE;
				break;
			case CursorState::DRAG:
				if (event.tfinger.fingerID != s_cursor.finger1) {
					break;
				}
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_cursor.x, s_cursor.y, SDL_BUTTON_LEFT);
				TouchOverlay::onGameGestureEnded();
				s_cursor.vx = 0.0f;
				s_cursor.vy = 0.0f;
				s_cursor.phase = CursorState::IDLE;
				break;
			case CursorState::TWO:
				if (event.tfinger.fingerID != s_cursor.finger1 && event.tfinger.fingerID != s_cursor.finger2) {
					break;
				}
				if (!canceled && s_cursor.travel <= CURSOR_TAP_SLOP && now - s_cursor.downTicks <= CURSOR_TAP_MS) {
					cursorClick(mouse, window, SDL_BUTTON_RIGHT, winW, winH);
				}
				// The other finger is still down: ignore it until it lifts too.
				s_cursor.finger1 = event.tfinger.fingerID == s_cursor.finger1 ? s_cursor.finger2 : s_cursor.finger1;
				s_cursor.phase = CursorState::WAIT_LIFT;
				break;
			case CursorState::WAIT_LIFT:
				if (event.tfinger.fingerID == s_cursor.finger1) {
					s_cursor.phase = CursorState::IDLE;
				}
				break;
			default:
				break;
			}
		}
		break;
	}

	cursorPublish(winW, winH);
	return leftClicked;
}

// Per frame: lost-lift recovery, hold-to-drag, inertia and edge scrolling.
void updateCursorMode(SDL3Mouse *mouse, SDL_Window *window)
{
	int winW = 0, winH = 0;
	SDL_GetWindowSize(window, &winW, &winH);
	if (winW <= 0 || winH <= 0) {
		return;
	}
	const Uint64 now = SDL_GetTicks();
	const float dt = s_cursor.lastFrameTicks != 0 ? SDL_min(0.1f, (float)(now - s_cursor.lastFrameTicks) / 1000.0f) : 0.0f;
	s_cursor.lastFrameTicks = now;

	if (!s_cursor.initialized) {
		s_cursor.initialized = true;
		s_cursor.x = (float)winW * 0.5f;
		s_cursor.y = (float)winH * 0.5f;
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_cursor.x, s_cursor.y);
	}

	// Same lost-lift recovery as the direct-touch translator (see updateTouchLongPress).
	if (s_cursor.phase != CursorState::IDLE && !TouchOverlay::fingerIsDown(s_cursor.touch, s_cursor.finger1)) {
		const bool secondStillDown = s_cursor.phase == CursorState::TWO &&
			TouchOverlay::fingerIsDown(s_cursor.touch, s_cursor.finger2);
		if (!secondStillDown) {
			fprintf(stderr, "INFO: touch: cursor finger lifted without an up event (phase %d), resetting\n",
			        (int)s_cursor.phase);
			if (s_cursor.phase == CursorState::DRAG) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_cursor.x, s_cursor.y, SDL_BUTTON_LEFT);
				TouchOverlay::onGameGestureEnded();
			}
			s_cursor.phase = CursorState::IDLE;
		}
	}

	// A held finger arms a drag; the left button goes down only once it slides, so a finger that
	// merely rests on the screen never holds the button.
	if (s_cursor.phase == CursorState::ONE && s_cursor.travel <= CURSOR_TAP_SLOP &&
	    now - s_cursor.downTicks >= CURSOR_HOLD_DRAG_MS) {
		TouchOverlay::addTapFeedback(s_cursor.x / (float)winW, s_cursor.y / (float)winH, false);
		s_cursor.phase = CursorState::HOLD;
	}

	if (s_cursor.phase == CursorState::IDLE && dt > 0.0f &&
	    (s_cursor.vx != 0.0f || s_cursor.vy != 0.0f)) {
		s_cursor.x += s_cursor.vx * dt;
		s_cursor.y += s_cursor.vy * dt;
		const float decay = SDL_powf(CURSOR_FRICTION, dt * 60.0f);
		s_cursor.vx *= decay;
		s_cursor.vy *= decay;
		if (SDL_sqrtf(s_cursor.vx * s_cursor.vx + s_cursor.vy * s_cursor.vy) < CURSOR_MIN_SPEED) {
			s_cursor.vx = 0.0f;
			s_cursor.vy = 0.0f;
		}
		cursorClamp(winW, winH);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_cursor.x, s_cursor.y);
	}

	if (s_cursor.phase != CursorState::TWO) {
		scrollAtCursorEdge(winW, winH);
	}

	cursorPublish(winW, winH);
}

// Switching modes mid-gesture: release whatever the old mode was holding.
void resetTouchModes(SDL3Mouse *mouse, SDL_Window *window)
{
	if (s_touch.phase == TouchState::DRAGGING) {
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
	} else if (s_touch.phase == TouchState::PAN) {
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
	}
	s_touch.phase = TouchState::IDLE;

	if (s_cursor.phase == CursorState::DRAG) {
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_cursor.x, s_cursor.y, SDL_BUTTON_LEFT);
	}
	s_cursor.phase = CursorState::IDLE;
	s_cursor.vx = 0.0f;
	s_cursor.vy = 0.0f;
	s_cursor.initialized = false;
	TouchOverlay::setCursorState(false, 0.0f, 0.0f, false);
}

// ---------------------------------------------------------------------------
// Game controller (iOS)
//
// GeneralsX @feature seastwood 29/09/2026 MFi / Xbox / PlayStation controllers through SDL's
// gamepad API. The controller drives the same cursor as cursor mode (it is shown while the
// controller is in use, in either touch mode):
//   right stick         cursor            left stick         scroll the camera
//   A / Cross           left button (hold + move = selection box)
//   B / Circle          right button
//   X / Square          select all of the selected type on screen
//   Y / Triangle        select all combat units (LB + Y: scatter)
//   LB (held)           Ctrl: force attack, and D-pad assigns groups
//   RB (held)           Shift: add to selection, waypoints
//   D-pad up/right/down/left   groups 1-4 (LB + D-pad: assign the selection to the group)
//   LT / RT             zoom out / in
//   Menu / Start        pause menu        View / Back        jump to the command center
//   L3                  jump to the latest radar event       R3   stop
// ---------------------------------------------------------------------------
struct GamepadState {
	SDL_Gamepad *pad = nullptr;
	SDL_JoystickID id = 0;
	bool active = false;       // the cursor is shown for the controller
	bool leftHeld = false;     // A holds the left mouse button
	bool rightHeld = false;    // B holds the right mouse button
	bool ctrlHeld = false;     // LB
	bool shiftHeld = false;    // RB
	Uint64 lastFrameTicks = 0;
	Uint64 lastZoomTicks = 0;
};

GamepadState s_pad;

const float PAD_DEAD_ZONE = 0.15f;
const float PAD_CURSOR_SPEED = 1.1f;     // screen widths per second at full tilt
const float PAD_SCROLL_SPEED = 300.0f;   // 1.5x the keyboard scroll amount, like the D-pad overlay
const float PAD_TRIGGER_THRESHOLD = 0.5f;
const Uint64 PAD_ZOOM_REPEAT_MS = 90;

float padAxis(SDL_GamepadAxis axis)
{
	return SDL_clamp((float)SDL_GetGamepadAxis(s_pad.pad, axis) / 32767.0f, -1.0f, 1.0f);
}

// Radial dead zone and a squared response: fine control near the center, full speed at the edge.
bool padStick(SDL_GamepadAxis axisX, SDL_GamepadAxis axisY, float &outX, float &outY)
{
	const float x = padAxis(axisX);
	const float y = padAxis(axisY);
	const float length = SDL_sqrtf(x * x + y * y);
	if (length <= PAD_DEAD_ZONE) {
		outX = outY = 0.0f;
		return false;
	}
	const float strength = SDL_min(1.0f, (length - PAD_DEAD_ZONE) / (1.0f - PAD_DEAD_ZONE));
	const float curved = strength * strength;
	outX = x / length * curved;
	outY = y / length * curved;
	return true;
}

void padSendKey(SDL_Scancode scancode, bool down)
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

void padMeta(GameMessage::Type type)
{
	if (TheMessageStream != nullptr) {
		TheMessageStream->appendMessage(type);
	}
}

void padShowCursor(SDL3Mouse *mouse, SDL_Window *window, int winW, int winH)
{
	if (!s_cursor.initialized) {
		s_cursor.initialized = true;
		s_cursor.x = (float)winW * 0.5f;
		s_cursor.y = (float)winH * 0.5f;
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_cursor.x, s_cursor.y);
	}
	s_pad.active = true;
}

void padOpen(SDL_JoystickID which)
{
	if (s_pad.pad != nullptr) {
		return;   // one controller at a time
	}
	s_pad.pad = SDL_OpenGamepad(which);
	if (s_pad.pad == nullptr) {
		fprintf(stderr, "WARNING: could not open game controller: %s\n", SDL_GetError());
		return;
	}
	s_pad.id = which;
	const char *name = SDL_GetGamepadName(s_pad.pad);
	fprintf(stderr, "INFO: game controller connected: %s\n", name != nullptr ? name : "(unnamed)");
}

void padReleaseAll(SDL3Mouse *mouse, SDL_Window *window)
{
	if (s_pad.leftHeld && mouse != nullptr) {
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_cursor.x, s_cursor.y, SDL_BUTTON_LEFT);
	}
	if (s_pad.rightHeld && mouse != nullptr) {
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, s_cursor.x, s_cursor.y, SDL_BUTTON_RIGHT);
	}
	if (s_pad.ctrlHeld) {
		padSendKey(SDL_SCANCODE_LCTRL, false);
	}
	if (s_pad.shiftHeld) {
		padSendKey(SDL_SCANCODE_LSHIFT, false);
	}
	s_pad.leftHeld = s_pad.rightHeld = s_pad.ctrlHeld = s_pad.shiftHeld = false;
}

void padClose(SDL3Mouse *mouse, SDL_Window *window)
{
	padReleaseAll(mouse, window);
	if (s_pad.pad != nullptr) {
		SDL_CloseGamepad(s_pad.pad);
	}
	s_pad = GamepadState();
	if (!s_cursorModeActive) {
		TouchOverlay::setCursorState(false, 0.0f, 0.0f, false);
	}
	fprintf(stderr, "INFO: game controller disconnected\n");
}

void padButton(SDL3Mouse *mouse, SDL_Window *window, Uint8 button, bool down)
{
	int winW = 0, winH = 0;
	SDL_GetWindowSize(window, &winW, &winH);
	if (winW <= 0 || winH <= 0) {
		return;
	}
	padShowCursor(mouse, window, winW, winH);

	switch (button) {
	case SDL_GAMEPAD_BUTTON_SOUTH:
		if (down != s_pad.leftHeld) {
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_cursor.x, s_cursor.y);
			sendSyntheticMouse(mouse, window, down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP,
			                   s_cursor.x, s_cursor.y, SDL_BUTTON_LEFT);
			s_pad.leftHeld = down;
			if (down) {
				TouchOverlay::addTapFeedback(s_cursor.x / (float)winW, s_cursor.y / (float)winH, false);
			} else {
				TouchOverlay::onGameGestureEnded();
			}
		}
		break;
	case SDL_GAMEPAD_BUTTON_EAST:
		if (down != s_pad.rightHeld) {
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_cursor.x, s_cursor.y);
			sendSyntheticMouse(mouse, window, down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP,
			                   s_cursor.x, s_cursor.y, SDL_BUTTON_RIGHT);
			s_pad.rightHeld = down;
			if (down) {
				TouchOverlay::addTapFeedback(s_cursor.x / (float)winW, s_cursor.y / (float)winH, true);
			}
		}
		break;
	case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
		if (down != s_pad.ctrlHeld) {
			padSendKey(SDL_SCANCODE_LCTRL, down);
			s_pad.ctrlHeld = down;
		}
		break;
	case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
		if (down != s_pad.shiftHeld) {
			padSendKey(SDL_SCANCODE_LSHIFT, down);
			s_pad.shiftHeld = down;
		}
		break;
	default:
		if (!down) {
			break;
		}
		switch (button) {
		case SDL_GAMEPAD_BUTTON_WEST:
			padMeta(GameMessage::MSG_META_SELECT_MATCHING_UNITS);
			break;
		case SDL_GAMEPAD_BUTTON_NORTH:
			padMeta(s_pad.ctrlHeld ? GameMessage::MSG_META_SCATTER : GameMessage::MSG_META_SELECT_ALL);
			break;
		case SDL_GAMEPAD_BUTTON_DPAD_UP:
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
			{
				const Int group = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? 1 :
					(button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 2 : (button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 3 : 4));
				if (s_pad.ctrlHeld) {
					padMeta((GameMessage::Type)(GameMessage::MSG_META_CREATE_TEAM0 + group));
					SDL_RumbleGamepad(s_pad.pad, 0x6000, 0x6000, 90);   // feel that the group was saved
				} else {
					padMeta((GameMessage::Type)(GameMessage::MSG_META_SELECT_TEAM0 + group));
				}
			}
			break;
		case SDL_GAMEPAD_BUTTON_START:
			padMeta(GameMessage::MSG_META_OPTIONS);
			break;
		case SDL_GAMEPAD_BUTTON_BACK:
			padMeta(GameMessage::MSG_META_VIEW_COMMAND_CENTER);
			break;
		case SDL_GAMEPAD_BUTTON_LEFT_STICK:
			padMeta(GameMessage::MSG_META_VIEW_LAST_RADAR_EVENT);
			break;
		case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
			padMeta(GameMessage::MSG_META_STOP);
			break;
		default:
			break;
		}
		break;
	}
	TouchOverlay::setCursorState(true, s_cursor.x / (float)winW, s_cursor.y / (float)winH, s_pad.leftHeld);
}

// Per frame: sticks and triggers are read directly (they are continuous, not events).
void updateGamepad(SDL3Mouse *mouse, SDL_Window *window)
{
	if (s_pad.pad == nullptr) {
		return;
	}
	int winW = 0, winH = 0;
	SDL_GetWindowSize(window, &winW, &winH);
	if (winW <= 0 || winH <= 0) {
		return;
	}
	const Uint64 now = SDL_GetTicks();
	const float dt = s_pad.lastFrameTicks != 0 ? SDL_min(0.1f, (float)(now - s_pad.lastFrameTicks) / 1000.0f) : 0.0f;
	s_pad.lastFrameTicks = now;

	// GeneralsX @tweak seastwood 30/09/2026 Left stick scrolls the camera, right stick moves the
	// cursor (the player's preference: aim with the right thumb, like a twin-stick game).
	float moveX = 0.0f, moveY = 0.0f;
	if (padStick(SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, moveX, moveY) && dt > 0.0f) {
		padShowCursor(mouse, window, winW, winH);
		const float speed = PAD_CURSOR_SPEED * (float)winW * TouchOverlay::cursorSensitivity();
		s_cursor.vx = 0.0f;   // the stick takes over from any cursor-mode glide
		s_cursor.vy = 0.0f;
		s_cursor.x += moveX * speed * dt;
		s_cursor.y += moveY * speed * dt;
		cursorClamp(winW, winH);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_cursor.x, s_cursor.y);
	}

	float scrollX = 0.0f, scrollY = 0.0f;
	if (padStick(SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, scrollX, scrollY) && cameraControlAvailable()) {
		s_pad.active = true;
		const Real fpsRatio = TheFramePacer != nullptr ? TheFramePacer->getBaseOverUpdateFpsRatio() : 1.0f;
		const Real amount = PAD_SCROLL_SPEED * fpsRatio * TheGlobalData->m_keyboardScrollFactor;
		Coord2D offset;
		offset.x = scrollX * TheGlobalData->m_horizontalScrollSpeedFactor * amount;
		offset.y = scrollY * TheGlobalData->m_verticalScrollSpeedFactor * amount;
		TheTacticalView->userScrollBy(&offset);
	}

	const float zoomIn = padAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
	const float zoomOut = padAxis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
	if ((zoomIn > PAD_TRIGGER_THRESHOLD || zoomOut > PAD_TRIGGER_THRESHOLD) && now - s_pad.lastZoomTicks >= PAD_ZOOM_REPEAT_MS) {
		s_pad.lastZoomTicks = now;
		padShowCursor(mouse, window, winW, winH);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_WHEEL, s_cursor.x, s_cursor.y, 0,
		                   zoomIn > zoomOut ? 1.0f : -1.0f);
	}

	if (s_pad.active) {
		if (!s_cursorModeActive) {
			scrollAtCursorEdge(winW, winH);   // cursor mode already does this in updateCursorMode
		}
		TouchOverlay::setCursorState(true, s_cursor.x / (float)winW, s_cursor.y / (float)winH, s_pad.leftHeld);
	}
}

// ---------------------------------------------------------------------------
// Gyro aiming
//
// GeneralsX @feature seastwood 30/09/2026 With the Gyro setting on and a cursor in use (cursor mode
// or a controller), turning and tilting the device moves the cursor, like aiming a pointer. The
// overlay's gyro button pauses it while held so the player can re-center their grip. The sensor
// is the device's own gyroscope (SDL's CoreMotion sensor), opened only while needed.
// ---------------------------------------------------------------------------
struct GyroState {
	SDL_Sensor *sensor = nullptr;
	bool unavailable = false;   // no gyroscope found; not retried until the setting is toggled
	Uint64 lastTicks = 0;
};

GyroState s_gyro;
const float GYRO_GAIN = 1.6f;          // screen widths per radian at Speed High (Medium, x0.7: ~50 degrees per screen)
const float GYRO_DEAD_ZONE = 0.03f;    // rad/s: hand tremor and sensor noise below this are ignored

void gyroClose()
{
	if (s_gyro.sensor != nullptr) {
		SDL_CloseSensor(s_gyro.sensor);
		fprintf(stderr, "INFO: gyro aiming off\n");
	}
	s_gyro = GyroState();
	TouchOverlay::setGyroActive(false);
}

bool gyroOpen()
{
	if (s_gyro.sensor != nullptr) {
		return true;
	}
	if (s_gyro.unavailable) {
		return false;
	}
	if (!SDL_WasInit(SDL_INIT_SENSOR) && !SDL_InitSubSystem(SDL_INIT_SENSOR)) {
		fprintf(stderr, "WARNING: gyro aiming: sensors unavailable: %s\n", SDL_GetError());
		s_gyro.unavailable = true;
		return false;
	}
	int count = 0;
	SDL_SensorID *sensors = SDL_GetSensors(&count);
	for (int i = 0; sensors != nullptr && i < count && s_gyro.sensor == nullptr; ++i) {
		if (SDL_GetSensorTypeForID(sensors[i]) == SDL_SENSOR_GYRO) {
			s_gyro.sensor = SDL_OpenSensor(sensors[i]);
		}
	}
	SDL_free(sensors);
	if (s_gyro.sensor == nullptr) {
		fprintf(stderr, "WARNING: gyro aiming: no gyroscope found\n");
		s_gyro.unavailable = true;
		return false;
	}
	fprintf(stderr, "INFO: gyro aiming on\n");
	return true;
}

// Soft dead zone: rates below it are dropped, rates above it lose it (no jump at the threshold).
float gyroDeadZone(float rate)
{
	if (rate > GYRO_DEAD_ZONE) {
		return rate - GYRO_DEAD_ZONE;
	}
	if (rate < -GYRO_DEAD_ZONE) {
		return rate + GYRO_DEAD_ZONE;
	}
	return 0.0f;
}

void updateGyro(SDL3Mouse *mouse, SDL_Window *window)
{
	const bool wanted = TouchOverlay::gyroEnabled() && (s_cursorModeActive || s_pad.pad != nullptr);
	if (!wanted) {
		if (s_gyro.sensor != nullptr || s_gyro.unavailable) {
			gyroClose();
		}
		return;
	}
	if (!gyroOpen()) {
		return;
	}
	TouchOverlay::setGyroActive(true);

	int winW = 0, winH = 0;
	SDL_GetWindowSize(window, &winW, &winH);
	const Uint64 now = SDL_GetTicks();
	const float dt = s_gyro.lastTicks != 0 ? SDL_min(0.1f, (float)(now - s_gyro.lastTicks) / 1000.0f) : 0.0f;
	s_gyro.lastTicks = now;
	float rate[3] = { 0.0f, 0.0f, 0.0f };
	if (winW <= 0 || winH <= 0 || dt <= 0.0f || TouchOverlay::gyroHeld() ||
	    !SDL_GetSensorData(s_gyro.sensor, rate, 3)) {
		return;
	}

	// SDL reports rotation in the device's portrait frame (x right, y up, z toward the viewer,
	// counter-clockwise positive) whatever the screen orientation. The game runs in landscape:
	// with the right side up (LANDSCAPE) the screen's up axis is the device's +x and its right
	// axis the device's -y; the other landscape flips both.
	const SDL_DisplayOrientation orientation = SDL_GetCurrentDisplayOrientation(SDL_GetDisplayForWindow(window));
	const float flip = orientation == SDL_ORIENTATION_LANDSCAPE_FLIPPED ? -1.0f : 1.0f;
	const float aroundScreenUp = flip * rate[0];      // turning left is positive
	const float aroundScreenRight = -flip * rate[1];  // tilting the top edge toward the player is positive
	const float aroundScreenNormal = rate[2];         // rolling counter-clockwise is positive
	// Turning right or rolling clockwise moves the cursor right; pointing the device up moves it up.
	const float horizontal = -gyroDeadZone(aroundScreenUp + aroundScreenNormal);
	const float vertical = -gyroDeadZone(aroundScreenRight);
	if (horizontal == 0.0f && vertical == 0.0f) {
		return;
	}
	const float gain = GYRO_GAIN * (float)winW * TouchOverlay::cursorSensitivity();
	if (!s_cursorModeActive) {
		padShowCursor(mouse, window, winW, winH);
	}
	s_cursor.vx = 0.0f;   // the gyro takes over from any cursor-mode glide
	s_cursor.vy = 0.0f;
	s_cursor.x += horizontal * gain * dt;
	s_cursor.y += vertical * gain * dt;
	cursorClamp(winW, winH);
	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_cursor.x, s_cursor.y);
	if (s_cursorModeActive) {
		cursorPublish(winW, winH);
	} else {
		TouchOverlay::setCursorState(true, s_cursor.x / (float)winW, s_cursor.y / (float)winH, s_pad.leftHeld);
	}
}

// A direct touch takes over from the controller: hide its cursor until the controller is used again.
void padYieldToTouch()
{
	if (s_pad.active && !s_cursorModeActive && !s_pad.leftHeld && !s_pad.rightHeld) {
		s_pad.active = false;
		TouchOverlay::setCursorState(false, 0.0f, 0.0f, false);
	}
}

bool touchGestureIdle()
{
	return s_cursorModeActive ? cursorGestureIdle() : s_touch.phase == TouchState::IDLE;
}

// GeneralsX @tweak seastwood 30/09/2026 Input timeline for diagnosing input that stops for seconds
// while the game keeps running (touches, the overlay and the controller alike). Every 10 s the log
// gets one line: frames run, touch and controller events received, the longest frame, and the
// longest delay between a touch or button happening and iOS handing it to the app (from the event
// timestamp). A span where the player kept tapping but no events arrived, or events arrived late,
// shows whether iOS withheld the input or the game ignored it.
struct InputHealth {
	Uint64 windowStart = 0;
	Uint64 lastPoll = 0;
	Uint32 frames = 0;
	Uint32 touchEvents = 0;
	Uint32 touchDowns = 0;
	Uint32 padEvents = 0;
	Uint64 longestFrameMs = 0;
	Uint64 longestDelayMs = 0;
	Uint64 lastLateLog = 0;
};

// Fingers currently down, to report long holds and where they were (a hand resting on the glass).
struct HeldFinger {
	SDL_FingerID id = 0;
	Uint64 downNS = 0;
	float x = 0.0f, y = 0.0f;
	bool used = false;
};
HeldFinger s_heldFingers[10];
const Uint64 LONG_HOLD_MS = 3000;

void heldFingerDown(const SDL_Event &event, Uint64 eventNS)
{
	HeldFinger *slot = nullptr;
	for (HeldFinger &finger : s_heldFingers) {
		if (finger.used && finger.id == event.tfinger.fingerID) {
			slot = &finger;   // reused id: the old lift was lost
			break;
		}
		if (!finger.used && slot == nullptr) {
			slot = &finger;
		}
	}
	if (slot != nullptr) {
		slot->used = true;
		slot->id = event.tfinger.fingerID;
		slot->downNS = eventNS;
		slot->x = event.tfinger.x;
		slot->y = event.tfinger.y;
	}
}

void heldFingerUp(const SDL_Event &event, Uint64 eventNS)
{
	for (HeldFinger &finger : s_heldFingers) {
		if (finger.used && finger.id == event.tfinger.fingerID) {
			finger.used = false;
			const Uint64 heldMs = eventNS > finger.downNS ? (eventNS - finger.downNS) / 1000000 : 0;
			if (heldMs >= LONG_HOLD_MS) {
				fprintf(stderr, "INFO: touch: a finger was held %u ms at (%.2f, %.2f)%s\n", (unsigned)heldMs,
				        finger.x, finger.y, event.type == SDL_EVENT_FINGER_CANCELED ? ", canceled by iOS" : "");
			}
			return;
		}
	}
}

InputHealth s_inputHealth;
const Uint64 INPUT_HEALTH_WINDOW_MS = 10000;
const Uint64 INPUT_LATE_MS = 300;

void inputHealthFrame()
{
	InputHealth &h = s_inputHealth;
	const Uint64 now = SDL_GetTicks();
	if (h.lastPoll != 0) {
		const Uint64 gap = now - h.lastPoll;
		if (gap >= 1000 && !iosShouldPauseRendering()) {
			fprintf(stderr, "INFO: input: %u ms without reading input (the game was busy)\n", (unsigned)gap);
		}
		h.longestFrameMs = SDL_max(h.longestFrameMs, gap);
	}
	h.lastPoll = now;
	++h.frames;
	if (h.windowStart == 0) {
		h.windowStart = now;
	} else if (now - h.windowStart >= INPUT_HEALTH_WINDOW_MS) {
		fprintf(stderr, "INFO: input health t=%us: %u frames, %u touch events (%u downs), %u controller events, "
		        "longest frame %u ms, most delayed input %u ms\n",
		        (unsigned)(now / 1000), (unsigned)h.frames, (unsigned)h.touchEvents, (unsigned)h.touchDowns,
		        (unsigned)h.padEvents, (unsigned)h.longestFrameMs, (unsigned)h.longestDelayMs);
		const Uint64 lastLateLog = h.lastLateLog;
		h = InputHealth();
		h.windowStart = now;
		h.lastPoll = now;
		h.lastLateLog = lastLateLog;
	}
}

void inputHealthEvent(const SDL_Event &event)
{
	InputHealth &h = s_inputHealth;
	switch (event.type) {
		case SDL_EVENT_FINGER_DOWN:
			++h.touchDowns;
			++h.touchEvents;
			break;
		case SDL_EVENT_FINGER_MOTION:
		case SDL_EVENT_FINGER_UP:
		case SDL_EVENT_FINGER_CANCELED:
			++h.touchEvents;
			break;
		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
			++h.padEvents;
			break;
		default:
			return;
	}
	const Uint64 nowNS = SDL_GetTicksNS();
	const bool timed = event.common.timestamp != 0 && event.common.timestamp <= nowNS;
	const Uint64 eventNS = timed ? event.common.timestamp : nowNS;
	if (event.type == SDL_EVENT_FINGER_DOWN) {
		heldFingerDown(event, eventNS);
	} else if (event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED) {
		heldFingerUp(event, eventNS);
	}
	if (!timed) {
		return;
	}
	const Uint64 delayMs = (nowNS - eventNS) / 1000000;
	h.longestDelayMs = SDL_max(h.longestDelayMs, delayMs);
	if (delayMs >= INPUT_LATE_MS && event.type != SDL_EVENT_FINGER_MOTION && event.type != SDL_EVENT_GAMEPAD_AXIS_MOTION) {
		const Uint64 now = SDL_GetTicks();
		if (now - h.lastLateLog >= 1000) {
			h.lastLateLog = now;
			if (event.type >= SDL_EVENT_FINGER_DOWN && event.type <= SDL_EVENT_FINGER_CANCELED) {
				fprintf(stderr, "INFO: input: event 0x%x reached the game %u ms after it happened, at (%.2f, %.2f)\n",
				        (unsigned)event.type, (unsigned)delayMs, event.tfinger.x, event.tfinger.y);
			} else {
				fprintf(stderr, "INFO: input: event 0x%x reached the game %u ms after it happened\n",
				        (unsigned)event.type, (unsigned)delayMs);
			}
		}
	}
}

// Free the gesture translator for a new touch if its current finger is resting; true when idle.
bool touchGestureYield(SDL3Mouse *mouse, SDL_Window *window)
{
	return s_cursorModeActive ? cursorYieldToNewFinger(mouse, window) : touchYieldToNewFinger(mouse, window);
}

} // anonymous namespace
#endif // TARGET_OS_IPHONE

namespace {

Bool DecodeNextUtf8Codepoint(const char* text, size_t length, size_t& offset, UnsignedInt& outCodepoint)
{
	outCodepoint = 0;
	if (!text || offset >= length) {
		return false;
	}

	const unsigned char first = static_cast<unsigned char>(text[offset]);
	if (first == 0) {
		return false;
	}

	if (first < 0x80) {
		outCodepoint = first;
		offset += 1;
		return true;
	}

	if ((first & 0xE0) == 0xC0 && offset + 1 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		if ((second & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x1F) << 6) | (second & 0x3F);
			offset += 2;
			return true;
		}
	}

	if ((first & 0xF0) == 0xE0 && offset + 2 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
		if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);
			offset += 3;
			return true;
		}
	}

	if ((first & 0xF8) == 0xF0 && offset + 3 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
		const unsigned char fourth = static_cast<unsigned char>(text[offset + 3]);
		if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80 && (fourth & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x07) << 18) | ((second & 0x3F) << 12) | ((third & 0x3F) << 6) | (fourth & 0x3F);
			offset += 4;
			return true;
		}
	}

	// Invalid UTF-8 sequence: skip one byte and keep processing.
	offset += 1;
	return false;
}

}

/**
 * Constructor: Initialize SDL3 game engine state
 */
SDL3GameEngine::SDL3GameEngine()
	: GameEngine(),
	  m_SDLWindow(nullptr),
	  m_IsInitialized(false),
	  m_IsActive(false),
	  m_IsTextInputActive(false),
	  m_TextInputFocusWindow(nullptr),
	  m_TextInputDismissedFor(nullptr),
	  m_TextInputUserOpened(false)
{
	fprintf(stderr, "DEBUG: SDL3GameEngine::SDL3GameEngine() created\n");
}

/**
 * Destructor: Cleanup SDL3 resources
 */
SDL3GameEngine::~SDL3GameEngine()
{
	if (m_SDLWindow && m_IsTextInputActive) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputFocusWindow = nullptr;
	}

	if (m_IsInitialized) {
		// Window cleanup is done in reset/shutdown
	}
	fprintf(stderr, "DEBUG: SDL3GameEngine::~SDL3GameEngine() destroyed\n");
}

/**
 * From GameEngine: init() - initialize subsystems
 * 
 * GeneralsX @bugfix felipebraz 16/02/2026
 * Simplified to follow fighter19 pattern - SDL3/Vulkan initialized in SDL3Main.cpp
 * before GameEngine is created. This init() only delegates to parent GameEngine::init().
 * ApplicationHWnd and TheSDL3Window are already set by main() before this is called.
 */
void SDL3GameEngine::init(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::init() starting\n");

	if (TheGlobalData && TheGlobalData->m_headless) {
		// GeneralsX @bugfix Copilot 17/05/2026 Allow headless replay path to initialize engine subsystems without an SDL window.
		fprintf(stderr, "INFO: SDL3GameEngine::init() headless mode - skipping SDL window binding\n");
		m_SDLWindow = nullptr;
		m_IsInitialized = true;
		m_IsActive = true;
		GameEngine::init();
		return;
	}

	// Verify window was created by SDL3Main.cpp
	extern SDL_Window* TheSDL3Window;
	extern HWND ApplicationHWnd;
	
	if (!TheSDL3Window || !ApplicationHWnd) {
		fprintf(stderr, "FATAL: SDL3 window not initialized before GameEngine::init()\n");
		fprintf(stderr, "FATAL: TheSDL3Window=%p, ApplicationHWnd=%p\n", TheSDL3Window, ApplicationHWnd);
		return;
	}

	// Store window reference locally
	m_SDLWindow = TheSDL3Window;
	m_IsInitialized = true;
	m_IsActive = true;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Lifecycle events can fire outside the poll cycle on iOS; catch them
	// immediately so rendering halts before the process is suspended.
	SDL_AddEventWatch(iosLifecycleWatcher, nullptr);
#endif

	fprintf(stderr, "INFO: SDL3GameEngine using pre-initialized window\n");

	// Call parent init to initialize game subsystems
	GameEngine::init();
}

/**
 * From GameEngine: reset() - reset system to starting state
 */
void SDL3GameEngine::reset(void)
{
	fprintf(stderr, "DEBUG: SDL3GameEngine::reset()\n");
	if (m_SDLWindow && m_IsTextInputActive) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputFocusWindow = nullptr;
	}
	GameEngine::reset();
}

/**
 * From GameEngine: update() - per-frame update
 */
void SDL3GameEngine::update(void)
{
	pollSDL3Events();
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Pause sim + render while backgrounded OR inactive (see iosLifecycleWatcher).
	// Acquiring a Metal drawable in these windows fights iOS for the layer and,
	// across repeated suspend/switcher cycles, crashes MoltenVK. Keep polling so
	// we still catch the resume events; just don't touch the GPU.
	if (iosShouldPauseRendering()) {
		SDL_Delay(50);
		return;
	}
#endif
	GameEngine::update();
}

/**
 * From GameEngine: execute() - main game loop
 */
void SDL3GameEngine::execute(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::execute() - entering main loop\n");
	GameEngine::execute();
	fprintf(stderr, "INFO: SDL3GameEngine::execute() - exited main loop\n");
}

/**
 * From GameEngine: serviceWindowsOS() - native OS service
 * On Linux, process SDL3 events
 */
void SDL3GameEngine::serviceWindowsOS(void)
{
	pollSDL3Events();
}

/**
 * Check if game has OS focus
 */
Bool SDL3GameEngine::isActive(void)
{
	return m_IsActive;
}

/**
 * Set OS focus status
 */
void SDL3GameEngine::setIsActive(Bool isActive)
{
	m_IsActive = isActive;
}

/**
 * Poll and process SDL3 events
 * Handles keyboard, mouse, window, and quit events
 */
void SDL3GameEngine::pollSDL3Events(void)
{
	if (!m_SDLWindow) {
		return;
	}

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Touches are only read here, so a busy frame (loading, a save, a renderer stall) shows as an
	// input hang; see InputHealth.
	inputHealthFrame();
#endif

	updateTextInputState();

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		inputHealthEvent(event);
#endif
		switch (event.type) {
			case SDL_EVENT_QUIT:
				m_quitting = true;
				break;

			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				m_quitting = true;
				break;

			case SDL_EVENT_WINDOW_FOCUS_GAINED:
				m_IsActive = true;
				if (TheMouse) {
					TheMouse->regainFocus();
					TheMouse->refreshCursorCapture();
				}
				break;

			case SDL_EVENT_WINDOW_FOCUS_LOST:
				m_IsActive = false;
				if (m_IsTextInputActive) {
					SDL_StopTextInput(m_SDLWindow);
					m_IsTextInputActive = false;
					m_TextInputFocusWindow = nullptr;
				}
				if (TheMouse) {
					TheMouse->loseFocus();
				}
				break;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			// App suspension/resume: mirror the desktop focus handling so audio
			// and mouse state pause cleanly (the render gate lives in update()).
			case SDL_EVENT_DID_ENTER_BACKGROUND:
				m_IsActive = false;
				if (TheMouse) {
					TheMouse->loseFocus();
				}
				break;

			case SDL_EVENT_DID_ENTER_FOREGROUND:
				m_IsActive = true;
				if (TheMouse) {
					TheMouse->regainFocus();
					TheMouse->refreshCursorCapture();
				}
				break;
#endif

			case SDL_EVENT_WINDOW_MOUSE_ENTER:
				if (TheMouse) {
					TheMouse->onCursorMovedInside();
				}
				break;

			case SDL_EVENT_WINDOW_MOUSE_LEAVE:
				if (TheMouse) {
					TheMouse->onCursorMovedOutside();
				}
				break;

			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:
				// Fighter19 pattern: direct addSDLEvent() call
				// GeneralsX @refactor felipebraz 16/02/2026 Simplified event routing
				if (TheKeyboard) {
					SDL3Keyboard* keyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
					if (keyboard) {
						keyboard->addSDLEvent(&event);
					}
				}
				break;

			case SDL_EVENT_TEXT_INPUT:
				forwardTextInputEvent(event.text.text);
				break;

			case SDL_EVENT_MOUSE_MOTION:
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
			case SDL_EVENT_MOUSE_WHEEL:
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
				// Belt-and-braces: drop SDL's own touch-synthesized mouse events.
				// The gesture translator owns all touch->mouse conversion; double
				// delivery would produce phantom second clicks.
				if (event.motion.which == SDL_TOUCH_MOUSEID) {
					break;
				}
#endif
				// Fighter19 pattern: direct addSDLEvent() call with raw SDL_Event
				// GeneralsX @refactor felipebraz 16/02/2026 Simplified event routing
				if (TheMouse) {
					SDL3Mouse* mouse = dynamic_cast<SDL3Mouse*>(TheMouse);
					if (mouse) {
						mouse->addSDLEvent(&event);
					}
				}
				break;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			case SDL_EVENT_FINGER_DOWN:
			case SDL_EVENT_FINGER_MOTION:
			case SDL_EVENT_FINGER_UP:
			case SDL_EVENT_FINGER_CANCELED:
				{
					// The floating keyboard button claims touches that start on it.
					bool toggleKeyboard = false;
					bool gestureIdle = touchGestureIdle();
					if (!gestureIdle && event.type == SDL_EVENT_FINGER_DOWN && TouchOverlay::controlAt(event)) {
						SDL3Mouse* yieldMouse = TheMouse ? dynamic_cast<SDL3Mouse*>(TheMouse) : nullptr;
						if (yieldMouse) {
							gestureIdle = touchGestureYield(yieldMouse, m_SDLWindow);
						}
					}
					if (TouchOverlay::handleFingerEvent(event, gestureIdle, toggleKeyboard)) {
						if (toggleKeyboard) {
							toggleOnScreenKeyboard();
						}
						break;
					}
				}
				if (s_cursorModeActive) {
					// Cursor mode: the finger is not where the click lands, so the keyboard follows
					// clicks at the cursor instead of touch-downs.
					if (TheMouse && m_SDLWindow) {
						SDL3Mouse* mouse = dynamic_cast<SDL3Mouse*>(TheMouse);
						if (mouse && handleCursorTouchEvent(mouse, m_SDLWindow, event)) {
							int winW = 0, winH = 0;
							SDL_GetWindowSize(m_SDLWindow, &winW, &winH);
							if (winW > 0 && winH > 0) {
								updateKeyboardForTouch(s_cursor.x / (float)winW, s_cursor.y / (float)winH);
							}
						}
					}
					break;
				}
				if (event.type == SDL_EVENT_FINGER_DOWN) {
					padYieldToTouch();
					updateKeyboardForTouch(event.tfinger.x, event.tfinger.y);
				}
				if (TheMouse && m_SDLWindow) {
					SDL3Mouse* mouse = dynamic_cast<SDL3Mouse*>(TheMouse);
					if (mouse) {
						handleTouchEvent(mouse, m_SDLWindow, event);
					}
				}
				break;
#endif

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			case SDL_EVENT_GAMEPAD_ADDED:
				padOpen(event.gdevice.which);
				break;

			case SDL_EVENT_GAMEPAD_REMOVED:
				if (s_pad.pad != nullptr && event.gdevice.which == s_pad.id) {
					padClose(TheMouse ? dynamic_cast<SDL3Mouse*>(TheMouse) : nullptr, m_SDLWindow);
				}
				break;

			case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
			case SDL_EVENT_GAMEPAD_BUTTON_UP:
				if (s_pad.pad != nullptr && event.gbutton.which == s_pad.id && TheMouse && m_SDLWindow) {
					SDL3Mouse* padMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
					if (padMouse) {
						padButton(padMouse, m_SDLWindow, event.gbutton.button, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
					}
				}
				break;
#endif

			case SDL_EVENT_WINDOW_RESIZED:
				handleWindowEvent(event.window);
				break;

			default:
				// Ignore other events for now
				break;
		}

		updateTextInputState();
	}

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Poll the long-press timers every frame; a stationary finger emits no events.
	TouchOverlay::update();
	if (TheMouse && m_SDLWindow) {
		SDL3Mouse* touchMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (touchMouse) {
			const bool cursorMode = TouchOverlay::cursorModeEnabled();
			if (cursorMode != s_cursorModeActive) {
				resetTouchModes(touchMouse, m_SDLWindow);
				s_cursorModeActive = cursorMode;
			}
			if (s_cursorModeActive) {
				updateCursorMode(touchMouse, m_SDLWindow);
			} else {
				updateTouchLongPress(touchMouse, m_SDLWindow);
			}
			updateGamepad(touchMouse, m_SDLWindow);
			updateGyro(touchMouse, m_SDLWindow);
		}
	}
#endif
}

// GeneralsX @bugfix felipebraz 01/04/2026 Enable SDL text input only while an entry gadget owns focus.
void SDL3GameEngine::updateTextInputState(void)
{
	if (!m_SDLWindow || !TheWindowManager) {
		return;
	}

	GameWindow* focusedWindow = TheWindowManager->winGetFocus();
	const Bool wantsTextInput =
		focusedWindow != nullptr && BitIsSet(focusedWindow->winGetStyle(), GWS_ENTRY_FIELD);

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// GeneralsX @feature seastwood 29/09/2026 Let the user close the on-screen keyboard. SDL stops
	// text input itself when the keyboard is hidden (Return with SDL_HINT_RETURN_KEY_HIDES_IME, or
	// the iPad hide key). Respect that instead of reopening it every frame: keep it closed until
	// focus moves to another window or the floating keyboard button reopens it.
	if (m_IsTextInputActive && !SDL_TextInputActive(m_SDLWindow)) {
		m_IsTextInputActive = false;
		m_TextInputUserOpened = false;
		m_TextInputDismissedFor = focusedWindow;
	}
	if (m_TextInputDismissedFor != nullptr && m_TextInputDismissedFor != focusedWindow) {
		m_TextInputDismissedFor = nullptr;
	}
	const Bool keepTextInput =
		(wantsTextInput && focusedWindow != m_TextInputDismissedFor) || m_TextInputUserOpened;
#else
	const Bool keepTextInput = wantsTextInput;
#endif

	if (keepTextInput) {
		if (!m_IsTextInputActive) {
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			// Tell SDL where the entry field is so it can slide the view up and keep the field
			// visible above the keyboard. The rect is in window points; the game draws in pixels.
			if (wantsTextInput && TheDisplay != nullptr && TheDisplay->getWidth() > 0 && TheDisplay->getHeight() > 0) {
				int winW = 0, winH = 0;
				SDL_GetWindowSize(m_SDLWindow, &winW, &winH);
				Int fieldX = 0, fieldY = 0, fieldW = 0, fieldH = 0;
				focusedWindow->winGetScreenPosition(&fieldX, &fieldY);
				focusedWindow->winGetSize(&fieldW, &fieldH);
				const float scaleX = (float)winW / (float)TheDisplay->getWidth();
				const float scaleY = (float)winH / (float)TheDisplay->getHeight();
				SDL_Rect area;
				area.x = (int)((float)fieldX * scaleX);
				area.y = (int)((float)fieldY * scaleY);
				area.w = (int)((float)fieldW * scaleX);
				area.h = (int)((float)fieldH * scaleY);
				SDL_SetTextInputArea(m_SDLWindow, &area, 0);
			} else {
				SDL_SetTextInputArea(m_SDLWindow, nullptr, 0);
			}
#endif
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			// GeneralsX @feature seastwood 29/09/2026 Plain keyboard: no autocorrect, spell checking
			// or predictive text bar, and no automatic capitals (a capital would reach hotkeys as
			// Shift+key). SDL maps autocorrect=false to UITextAutocorrectionTypeNo, which also
			// removes the QuickType suggestion bar.
			static SDL_PropertiesID s_textInputProps = 0;
			if (s_textInputProps == 0) {
				s_textInputProps = SDL_CreateProperties();
				SDL_SetNumberProperty(s_textInputProps, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_TEXT);
				SDL_SetNumberProperty(s_textInputProps, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER, SDL_CAPITALIZE_NONE);
				SDL_SetBooleanProperty(s_textInputProps, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN, false);
				SDL_SetBooleanProperty(s_textInputProps, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, false);
			}
			const bool started = SDL_StartTextInputWithProperties(m_SDLWindow, s_textInputProps);
#else
			const bool started = SDL_StartTextInput(m_SDLWindow);
#endif
			if (started) {
				m_IsTextInputActive = true;
			}
		}
	} else {
		if (m_IsTextInputActive) {
			SDL_StopTextInput(m_SDLWindow);
			m_IsTextInputActive = false;
		}
	}
	m_TextInputFocusWindow = wantsTextInput ? focusedWindow : nullptr;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	float fieldTop = -1.0f;
	if (m_IsTextInputActive && wantsTextInput && TheDisplay != nullptr && TheDisplay->getHeight() > 0) {
		Int fieldX = 0, fieldY = 0;
		focusedWindow->winGetScreenPosition(&fieldX, &fieldY);
		fieldTop = (float)fieldY / (float)TheDisplay->getHeight();
	}
	TouchOverlay::setKeyboardState(m_IsTextInputActive, fieldTop);
#endif
}

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
// GeneralsX @feature seastwood 29/09/2026 Touches drive the on-screen keyboard like a native text
// field: a touch anywhere except the entry field being typed into closes it, and a touch on that
// field reopens it after it was closed. A touch on another entry field opens the keyboard for it
// through the focus change the click causes.
void SDL3GameEngine::updateKeyboardForTouch(float x, float y)
{
	if (!m_SDLWindow || TheDisplay == nullptr || TheWindowManager == nullptr) {
		return;
	}

	GameWindow* focusedWindow = TheWindowManager->winGetFocus();
	Bool insideField = FALSE;
	if (focusedWindow != nullptr && BitIsSet(focusedWindow->winGetStyle(), GWS_ENTRY_FIELD)) {
		Int fieldX = 0, fieldY = 0, fieldW = 0, fieldH = 0;
		focusedWindow->winGetScreenPosition(&fieldX, &fieldY);
		focusedWindow->winGetSize(&fieldW, &fieldH);
		const float touchX = x * (float)TheDisplay->getWidth();
		const float touchY = y * (float)TheDisplay->getHeight();
		insideField = touchX >= (float)fieldX && touchX <= (float)(fieldX + fieldW) &&
		              touchY >= (float)fieldY && touchY <= (float)(fieldY + fieldH);
	}

	if (m_IsTextInputActive && !insideField) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputUserOpened = false;
		m_TextInputDismissedFor = focusedWindow;
	} else if (!m_IsTextInputActive && insideField && m_TextInputDismissedFor == focusedWindow) {
		m_TextInputDismissedFor = nullptr;
	} else {
		return;
	}
	updateTextInputState();
}
#endif

// GeneralsX @feature seastwood 29/09/2026 Floating keyboard button: show or hide the on-screen
// keyboard regardless of which window has focus. With no entry field focused, typed keys still
// reach the game as key presses (hotkeys, chat).
void SDL3GameEngine::toggleOnScreenKeyboard(void)
{
	if (!m_SDLWindow) {
		return;
	}

	if (m_IsTextInputActive) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputUserOpened = false;
		m_TextInputDismissedFor = TheWindowManager ? TheWindowManager->winGetFocus() : nullptr;
	} else {
		m_TextInputDismissedFor = nullptr;
		m_TextInputUserOpened = true;
	}
	updateTextInputState();
}

// GeneralsX @bugfix felipebraz 01/04/2026 Forward SDL UTF-8 text input through existing GWM_IME_CHAR path.
void SDL3GameEngine::forwardTextInputEvent(const char* utf8Text)
{
	if (!utf8Text || !TheWindowManager) {
		return;
	}

	// GeneralsX @bugfix felipebraz 01/04/2026 Use tracked text-input focus window to keep SDL text delivery stable.
	GameWindow* targetWindow = m_TextInputFocusWindow;
	if (!targetWindow || !BitIsSet(targetWindow->winGetStyle(), GWS_ENTRY_FIELD)) {
		return;
	}

	const size_t textLength = strlen(utf8Text);
	size_t offset = 0;
	while (offset < textLength) {
		UnsignedInt codepoint = 0;
		if (!DecodeNextUtf8Codepoint(utf8Text, textLength, offset, codepoint)) {
			continue;
		}

		// GeneralsX @bugfix felipebraz 01/04/2026 Clamp IME char forwarding to BMP and reject UTF-16 surrogate range.
		if (codepoint == 0 || codepoint > 0x10FFFFU) {
			continue;
		}

		if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
			continue;
		}

		if (codepoint > 0xFFFFU) {
			continue;
		}

		const WideChar wideCharacter = static_cast<WideChar>(codepoint);
		TheWindowManager->winSendInputMsg(targetWindow, GWM_IME_CHAR, static_cast<WindowMsgData>(wideCharacter), 0);
	}
}

/**
 * Handle keyboard event -dispatch to Keyboard manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleKeyboardEvent(const SDL_KeyboardEvent& event)
{
	// Dispatch to SDL3Keyboard if available
	if (TheKeyboard) {
		SDL3Keyboard* sdlKeyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
		if (sdlKeyboard) {
			sdlKeyboard->addSDL3KeyEvent(event);
		}
	}
}

/**
 * Handle mouse motion event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseMotionEvent(const SDL_MouseMotionEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseMotionEvent(event);
		}
	}
}

/**
 * Handle mouse button event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseButtonEvent(const SDL_MouseButtonEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseButtonEvent(event);
		}
	}
}

/**
 * Handle mouse wheel event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseWheelEvent(const SDL_MouseWheelEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseWheelEvent(event);
		}
	}
}

/**
 * Handle window event (resize, etc.)
 */
void SDL3GameEngine::handleWindowEvent(const SDL_WindowEvent& event)
{
	// TODO: Phase 2 - Handle window resize, notify graphics subsystem
	// fprintf(stderr, "DEBUG: Window event (type=%d)\n", event.type);
}

/**
 * Factory Methods for GameEngine subsystems
 * TheSuperHackers @build felipebraz 13/02/2026
 * Implementations in .cpp to provide complete type definitions and avoid circular includes
 */

LocalFileSystem *SDL3GameEngine::createLocalFileSystem(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createLocalFileSystem() -> StdLocalFileSystem\n");
	return NEW StdLocalFileSystem;
}

ArchiveFileSystem *SDL3GameEngine::createArchiveFileSystem(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createArchiveFileSystem() -> StdBIGFileSystem\n");
	return NEW StdBIGFileSystem;
}

GameLogic *SDL3GameEngine::createGameLogic(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createGameLogic() -> W3DGameLogic\n");
	return NEW W3DGameLogic;
}

GameClient *SDL3GameEngine::createGameClient(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createGameClient() -> W3DGameClient\n");
	return NEW W3DGameClient;
}

ModuleFactory *SDL3GameEngine::createModuleFactory(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createModuleFactory() -> W3DModuleFactory\n");
	return NEW W3DModuleFactory;
}

ThingFactory *SDL3GameEngine::createThingFactory(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createThingFactory() -> W3DThingFactory\n");
	return NEW W3DThingFactory;
}

FunctionLexicon *SDL3GameEngine::createFunctionLexicon(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createFunctionLexicon() -> W3DFunctionLexicon\n");
	return NEW W3DFunctionLexicon;
}

// GeneralsX @bugfix Copilot 15/04/2026 Match upstream GameEngine pure-virtual signature after sync.
Radar *SDL3GameEngine::createRadar(Bool dummy)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Respect headless mode and create dummy radar.
	// Upstream reference: Win32GameEngine headless factory behavior, TheSuperHackers/GeneralsGameCode
	// https://github.com/TheSuperHackers/GeneralsGameCode
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> RadarDummy (headless)\n");
		return NEW RadarDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> W3DRadar\n");
	return NEW W3DRadar;
}

// GeneralsX @bugfix Copilot 24/03/2026 Match upstream GameEngine pure-virtual signature after sync.
ParticleSystemManager* SDL3GameEngine::createParticleSystemManager(Bool dummy)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Respect headless mode and create dummy particle manager.
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> ParticleSystemManagerDummy (headless)\n");
		return NEW ParticleSystemManagerDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> W3DParticleSystemManager\n");
	return NEW W3DParticleSystemManager;
}

WebBrowser *SDL3GameEngine::createWebBrowser(void)
{
	// WebBrowser uses Windows COM (CComObject<W3DWebBrowser>)
	// Not available on Linux - return nullptr
	fprintf(stderr, "WARNING: WebBrowser not available on Linux platform\n");
	return nullptr;
}

/**
 * Factory method: AudioManager
 * Select audio backend based on compile flags
 * GeneralsX @bugfix Copilot 15/04/2026 Match upstream GameEngine pure-virtual signature after sync.
 */
AudioManager *SDL3GameEngine::createAudioManager(Bool dummy)
{
	(void)dummy;
	fprintf(stderr, "INFO: SDL3GameEngine::createAudioManager()\n");

#ifdef SAGE_USE_OPENAL
	fprintf(stderr, "INFO: Creating OpenAL audio backend\n");
	return new OpenALAudioManager();
#else
	fprintf(stderr, "INFO: Audio backend not available (SAGE_USE_OPENAL not defined)\n");
	fprintf(stderr, "WARNING: Falls back to parent implementation or silent mode\n");
	return GameEngine::createAudioManager();  // Call parent (may return stub)
#endif
}

#endif // !_WIN32

