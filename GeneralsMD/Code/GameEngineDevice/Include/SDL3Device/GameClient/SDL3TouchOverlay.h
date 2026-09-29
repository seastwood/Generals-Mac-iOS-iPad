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

// GeneralsX @feature seastwood 29/09/2026 On-screen overlay controls for touch devices (iOS).
// The overlay state and touch handling live in SDL3GameEngine.cpp next to the touch gesture
// translator; W3DDisplay::draw() calls this once per frame, after the UI, so the controls
// stay on top of the game.

#pragma once

/// Draw the floating on-screen keyboard button. Only defined on iOS builds.
void SDL3TouchOverlay_Draw(void);
