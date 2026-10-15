//----------------------------------------------------------------------------
//  EDGE Default Settings
//----------------------------------------------------------------------------
//
//  Copyright (c) 1999-2024 The EDGE Team.
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//----------------------------------------------------------------------------

#pragma once

#include "dm_defs.h"

// Screen resolution
constexpr int kDefaultScreenWidth  = 1000000; // Super high number to force scaling to native res
constexpr int kDefaultScreenHeight = 1000000; // Super high number to force scaling to native res
constexpr int kDefaultScreenBits   = 32;
constexpr int kDefaultDisplayMode  = 1;

// Controls (Key/Mouse Buttons)
constexpr int kDefaultKeyFire         = kMouse1 + (kGamepadTriggerRight << 16);
constexpr int kDefaultKeySecondAttack = 'e' + (kGamepadTriggerLeft << 16);
constexpr int kDefaultKeyUse          = kSpace + (kGamepadA << 16);
constexpr int kDefaultKeyUp           = kUpArrow + ('w' << 16);
constexpr int kDefaultKeyDown         = kDownArrow + ('s' << 16);
constexpr int kDefaultKeyLeft         = kLeftArrow;
constexpr int kDefaultKeyRight        = kRightArrow;
constexpr int kDefaultKeyFlyUp        = '/' + (kGamepadLeftStick << 16);
constexpr int kDefaultKeyFlyDown      = 'c' + (kGamepadRightStick << 16);
constexpr int kDefaultKeySpeed        = kRightShift;
constexpr int kDefaultKeyStrafe       = kRightAlt + (kMouse3 << 16);
constexpr int kDefaultKeyStrafeLeft   = ',' + ('a' << 16);
constexpr int kDefaultKeyStrafeRight  = '.' + ('d' << 16);
constexpr int kDefaultKeyAutorun      = kCapsLock;

constexpr int kDefaultKeyLookUp            = kPageUp;
constexpr int kDefaultKeyLookDown          = kPageDown;
constexpr int kDefaultKeyLookCenter        = kHome;
constexpr int kDefaultKeyMouseLook         = 'm';
constexpr int kDefaultKeyZoom              = 'z' + (kGamepadUp << 16);
constexpr int kDefaultKeyMap               = kTab + (kGamepadBack << 16);
constexpr int kDefaultKeyTurn180           = 0;
constexpr int kDefaultKeyReload            = 'r' + (kGamepadX << 16);
constexpr int kDefaultKeyNextWeapon        = kMouseWheelUp + (kGamepadRightShoulder << 16);
constexpr int kDefaultKeyPreviousWeapon    = kMouseWheelDown + (kGamepadLeftShoulder << 16);
constexpr int kDefaultKeyTalk              = 't';
constexpr int kDefaultKeyConsole           = kTilde;
constexpr int kDefaultKeyAction1           = 'o' + (kGamepadY << 16);
constexpr int kDefaultKeyAction2           = 'p' + (kGamepadB << 16);
constexpr int kDefaultKeyPreviousInventory = '[' + (kGamepadLeft << 16);
constexpr int kDefaultKeyNextInventory     = ']' + (kGamepadRight << 16);
constexpr int kDefaultKeyUseInventory      = kEnter + (kGamepadDown << 16);

// Controls (Analogue)
constexpr int kDefaultMouseXAxis = 2 * kAxisTurn - 1;
constexpr int kDefaultMouseYAxis = 2 * kAxisMouselook - 1;

constexpr int kDefaultJoystickXAxis = 2 * kAxisTurn - 1;
constexpr int kDefaultJoystickYAxis = 2 * kAxisForward;

// Misc
constexpr int kDefaultMenuLanguage = 0;

// Sound and Music
constexpr int kDefaultSoundStereo = 1; // Stereo

// Video Options
constexpr int kDefaultUseSmoothing          = 0;
constexpr int kDefaultUseMipmapping         = 2;
constexpr int kDefaultUseDynamicLights      = 1;
constexpr int kDefaultDetailLevel           = 2;
constexpr int kDefaultUpscaling             = 0;
constexpr int kDefaultScreenHUD             = 0;
constexpr int kDefaultCrosshair             = 0;
constexpr int kDefaultMapOverlay            = 0;
constexpr int kDefaultRotateMap             = 0;
constexpr int kDefaultInvulnerabilityEffect = 1; // TEXTURED
constexpr int kDefaultWipeMethod            = 1;
constexpr int kDefaultPNGScreenshots        = 1;

// Gameplay Options
constexpr int kDefaultAutoaim          = 0;
constexpr int kDefaultMouseLook        = 1;
constexpr int kDefaultJump             = 1;
constexpr int kDefaultCrouch           = 1;
constexpr int kDefaultKicking          = 0;
constexpr int kDefaultWeaponSwitch     = 1;
constexpr int kDefaultMoreBlood        = 0;
constexpr int kDefaultHaveExtra        = 1;
constexpr int kDefaultTrue3DGameplay   = 1;
constexpr int kDefaultPassMissile      = 1;
constexpr int kDefaultResurrectRespawn = 1; // Resurrect Mode
constexpr int kDefaultItemRespawn      = 0;
constexpr int kDefaultFastMonsters     = 0;
constexpr int kDefaultRespawn          = 0;

constexpr int kDefaultAutomapKeyDoorBlink = 1;
constexpr int kDefaultAutomapKeyDoorText  = 0;

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
