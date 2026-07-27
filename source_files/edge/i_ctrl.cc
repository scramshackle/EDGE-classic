//----------------------------------------------------------------------------
//  EDGE SDL Controller Stuff
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

#include <set>

#include "dm_defs.h"
#include "dm_state.h"
#include "e_event.h"
#include "e_input.h"
#include "e_main.h"
#include "epi.h"
#include "epi_str_util.h"
#include "i_sound.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#include "r_modes.h"

// FIXME: Combine all these SDL bool vars into an int/enum'd flags structure

// Work around for alt-tabbing
bool alt_is_down;
bool eat_mouse_motion = true;

bool no_joystick;                     // what a wowser, joysticks completely disabled

bool                need_mouse_recapture = false;
std::set<SDL_JoystickID> attached_joysticks;
static std::set<SDL_JoystickID>::iterator joystick_device;
SDL_Joystick       *joystick_info        = nullptr;
SDL_Gamepad        *gamepad_info         = nullptr;
SDL_JoystickID      current_gamepad      = 0;


// Track trigger state to avoid pushing multiple unnecessary trigger events
bool right_trigger_pulled = false;
bool left_trigger_pulled  = false;

//
// Translates a key from SDL -> EDGE
// Returns -1 if no suitable translation exists.
//
int TranslateSDLKey(SDL_Scancode key)
{
    switch (key)
    {
    case SDL_SCANCODE_GRAVE:
        return kTilde;
    case SDL_SCANCODE_MINUS:
        return kMinus;
    case SDL_SCANCODE_EQUALS:
        return kEquals;

    case SDL_SCANCODE_TAB:
        return kTab;
    case SDL_SCANCODE_RETURN:
        return kEnter;
    case SDL_SCANCODE_ESCAPE:
        return kEscape;
    case SDL_SCANCODE_BACKSPACE:
        return kBackspace;

    case SDL_SCANCODE_UP:
        return kUpArrow;
    case SDL_SCANCODE_DOWN:
        return kDownArrow;
    case SDL_SCANCODE_LEFT:
        return kLeftArrow;
    case SDL_SCANCODE_RIGHT:
        return kRightArrow;

    case SDL_SCANCODE_HOME:
        return kHome;
    case SDL_SCANCODE_END:
        return kEnd;
    case SDL_SCANCODE_INSERT:
        return kInsert;
    case SDL_SCANCODE_DELETE:
        return kDelete;
    case SDL_SCANCODE_PAGEUP:
        return kPageUp;
    case SDL_SCANCODE_PAGEDOWN:
        return kPageDown;

    case SDL_SCANCODE_F1:
        return kFunction1;
    case SDL_SCANCODE_F2:
        return kFunction2;
    case SDL_SCANCODE_F3:
        return kFunction3;
    case SDL_SCANCODE_F4:
        return kFunction4;
    case SDL_SCANCODE_F5:
        return kFunction5;
    case SDL_SCANCODE_F6:
        return kFunction6;
    case SDL_SCANCODE_F7:
        return kFunction7;
    case SDL_SCANCODE_F8:
        return kFunction8;
    case SDL_SCANCODE_F9:
        return kFunction9;
    case SDL_SCANCODE_F10:
        return kFunction10;
    case SDL_SCANCODE_F11:
        return kFunction11;
    case SDL_SCANCODE_F12:
        return kFunction12;

    case SDL_SCANCODE_KP_0:
        return kKeypad0;
    case SDL_SCANCODE_KP_1:
        return kKeypad1;
    case SDL_SCANCODE_KP_2:
        return kKeypad2;
    case SDL_SCANCODE_KP_3:
        return kKeypad3;
    case SDL_SCANCODE_KP_4:
        return kKeypad4;
    case SDL_SCANCODE_KP_5:
        return kKeypad5;
    case SDL_SCANCODE_KP_6:
        return kKeypad6;
    case SDL_SCANCODE_KP_7:
        return kKeypad7;
    case SDL_SCANCODE_KP_8:
        return kKeypad8;
    case SDL_SCANCODE_KP_9:
        return kKeypad9;

    case SDL_SCANCODE_KP_PERIOD:
        return kKeypadDot;
    case SDL_SCANCODE_KP_PLUS:
        return kKeypadPlus;
    case SDL_SCANCODE_KP_MINUS:
        return kKeypadMinus;
    case SDL_SCANCODE_KP_MULTIPLY:
        return kKeypadStar;
    case SDL_SCANCODE_KP_DIVIDE:
        return kKeypadSlash;
    case SDL_SCANCODE_KP_EQUALS:
        return kKeypadEquals;
    case SDL_SCANCODE_KP_ENTER:
        return kKeypadEnter;

    case SDL_SCANCODE_PRINTSCREEN:
        return kPrintScreen;
    case SDL_SCANCODE_CAPSLOCK:
        return kCapsLock;
    case SDL_SCANCODE_NUMLOCKCLEAR:
        return kNumberLock;
    case SDL_SCANCODE_SCROLLLOCK:
        return kScrollLock;
    case SDL_SCANCODE_PAUSE:
        return kPause;

    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
        return kRightShift;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
        return kRightControl;
    case SDL_SCANCODE_LGUI:
    case SDL_SCANCODE_LALT:
        return kLeftAlt;
    case SDL_SCANCODE_RGUI:
    case SDL_SCANCODE_RALT:
        return kRightAlt;

    default:
        break;
    }

    if (key <= 0x7f)
        return epi::ToLowerASCII(SDL_GetKeyFromScancode(key, SDL_GetModState(), false));

    return -1;
}

void HandleFocusGain(void)
{
    // Hide cursor and grab input
    GrabCursor(true);

    // Ignore any pending mouse motion
    eat_mouse_motion = true;

    // Now active again
    app_state |= kApplicationActive;
}

void HandleFocusLost(void)
{
    GrabCursor(false);

    EdgeIdle();

    // No longer active
    app_state &= ~kApplicationActive;
}

void HandleKeyEvent(SDL_Event *ev)
{
    if (ev->type != SDL_EVENT_KEY_DOWN && ev->type != SDL_EVENT_KEY_UP)
        return;

    SDL_Scancode sym = ev->key.scancode;

    InputEvent event;
    event.value.key.sym = TranslateSDLKey(sym);

    // handle certain keys which don't behave normally
    if (sym == SDL_SCANCODE_CAPSLOCK || sym == SDL_SCANCODE_NUMLOCKCLEAR)
    {
        if (ev->type != SDL_EVENT_KEY_DOWN)
            return;
        event.type = kInputEventKeyDown;
        PostEvent(&event);

        event.type = kInputEventKeyUp;
        PostEvent(&event);
        return;
    }

    event.type = (ev->type == SDL_EVENT_KEY_DOWN) ? kInputEventKeyDown : kInputEventKeyUp;

    if (event.value.key.sym < 0)
    {
        // No translation possible for SDL symbol and no unicode value
        return;
    }

    if (event.value.key.sym == kTab && alt_is_down)
    {
        alt_is_down = false;
        return;
    }

#ifndef EDGE_WEB // Not sure if this is desired on the web build
    if (event.value.key.sym == kEnter && alt_is_down)
    {
        alt_is_down = false;
        ToggleFullscreen();
        if (current_window_mode == kWindowModeWindowed)
        {
            GrabCursor(false);
        }
        return;
    }
#endif

    if (event.value.key.sym == kLeftAlt)
        alt_is_down = (event.type == kInputEventKeyDown);

    PostEvent(&event);
}

void HandleMouseButtonEvent(SDL_Event *ev)
{
    InputEvent event;

    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        event.type = kInputEventKeyDown;
    else if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP)
        event.type = kInputEventKeyUp;
    else
        return;

    switch (ev->button.button)
    {
    case 1:
        event.value.key.sym = kMouse1;
        break;
    case 2:
        event.value.key.sym = kMouse2;
        break;
    case 3:
        event.value.key.sym = kMouse3;
        break;
    case 4:
        event.value.key.sym = kMouse4;
        break;
    case 5:
        event.value.key.sym = kMouse5;
        break;
    case 6:
        event.value.key.sym = kMouse6;
        break;

    default:
        return;
    }

    PostEvent(&event);
}

void HandleMouseWheelEvent(SDL_Event *ev)
{
    InputEvent event;
    InputEvent release;

    event.type   = kInputEventKeyDown;
    release.type = kInputEventKeyUp;

    if (ev->wheel.y > 0)
    {
        event.value.key.sym   = kMouseWheelUp;
        release.value.key.sym = kMouseWheelUp;
    }
    else if (ev->wheel.y < 0)
    {
        event.value.key.sym   = kMouseWheelDown;
        release.value.key.sym = kMouseWheelDown;
    }
    else
    {
        return;
    }
    PostEvent(&event);
    PostEvent(&release);
}

static void HandleGamepadButtonEvent(SDL_Event *ev)
{
    // ignore other gamepads;
    if (ev->gbutton.which != current_gamepad)
        return;

    InputEvent event;

    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        event.type = kInputEventKeyDown;
    else if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_UP)
        event.type = kInputEventKeyUp;
    else
        return;

    if (ev->gbutton.button >= SDL_GAMEPAD_BUTTON_COUNT) // How would this happen? - Dasho
        return;

    event.value.key.sym = kGamepadA + ev->gbutton.button;

    PostEvent(&event);
}

static void HandleGamepadTriggerEvent(SDL_Event *ev)
{
    // ignore other gamepads
    if (ev->gaxis.which != current_gamepad)
        return;

    Uint8 current_axis = ev->gaxis.axis;

    // ignore non-trigger axes
    if (current_axis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER && current_axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
        return;

    InputEvent event;

    int thresh = RoundToInteger(*joystick_deadzones[current_axis] * 32767.0f);
    int input  = ev->gaxis.value;

    if (current_axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
    {
        event.value.key.sym = kGamepadTriggerLeft;
        if (input < thresh)
        {
            if (!left_trigger_pulled)
                return;
            event.type          = kInputEventKeyUp;
            left_trigger_pulled = false;
        }
        else
        {
            if (left_trigger_pulled)
                return;
            event.type          = kInputEventKeyDown;
            left_trigger_pulled = true;
        }
    }
    else
    {
        event.value.key.sym = kGamepadTriggerRight;
        if (input < thresh)
        {
            if (!right_trigger_pulled)
                return;
            event.type           = kInputEventKeyUp;
            right_trigger_pulled = false;
        }
        else
        {
            if (right_trigger_pulled)
                return;
            event.type           = kInputEventKeyDown;
            right_trigger_pulled = true;
        }
    }

    PostEvent(&event);
}

void HandleMouseMotionEvent(SDL_Event *ev)
{
    int dx, dy;

    dx = ev->motion.xrel;
    dy = ev->motion.yrel;

    if (dx || dy)
    {
        InputEvent event;

        event.type           = kInputEventKeyMouse;
        event.value.mouse.dx = dx;
        event.value.mouse.dy = -dy; // -AJA- positive should be "up"

        PostEvent(&event);
    }
}

int JoystickGetAxis(int n) // n begins at 0
{
    if (no_joystick || !joystick_info || !gamepad_info)
        return 0;

    return SDL_GetGamepadAxis(gamepad_info, (SDL_GamepadAxis)n);
}

static void I_OpenJoystick(SDL_JoystickID id)
{
    EPI_ASSERT(attached_joysticks.find(id) != attached_joysticks.end());

    joystick_info = SDL_OpenJoystick(id);
    if (!joystick_info)
    {
        LogPrint("Unable to open joystick %d (SDL error)\n", id);
        return;
    }

    gamepad_info = SDL_OpenGamepad(id);

    if (!gamepad_info)
    {
        LogPrint("Unable to open joystick %s as a gamepad!\n", SDL_GetJoystickName(joystick_info));
        SDL_CloseJoystick(joystick_info);
        joystick_info = nullptr;
        return;
    }

    current_gamepad = id;

    const char *name = SDL_GetGamepadName(gamepad_info);
    if (!name)
        name = "(UNKNOWN)";

    int gp_total_joysticksticks = 0;
    int gp_num_triggers         = 0;
    int gp_num_buttons          = 0;

    if (SDL_GamepadHasAxis(gamepad_info, SDL_GAMEPAD_AXIS_LEFTX) &&
        SDL_GamepadHasAxis(gamepad_info, SDL_GAMEPAD_AXIS_LEFTY))
        gp_total_joysticksticks++;
    if (SDL_GamepadHasAxis(gamepad_info, SDL_GAMEPAD_AXIS_RIGHTX) &&
        SDL_GamepadHasAxis(gamepad_info, SDL_GAMEPAD_AXIS_RIGHTY))
        gp_total_joysticksticks++;
    if (SDL_GamepadHasAxis(gamepad_info, SDL_GAMEPAD_AXIS_LEFT_TRIGGER))
        gp_num_triggers++;
    if (SDL_GamepadHasAxis(gamepad_info, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
        gp_num_triggers++;
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
    {
        if (SDL_GamepadHasButton(gamepad_info, (SDL_GamepadButton)i))
            gp_num_buttons++;
    }

    LogPrint("Opened gamepad %d : %s\n", id, name);
    LogPrint("Sticks:%d Triggers: %d Buttons: %d Touchpads: %d\n", gp_total_joysticksticks, gp_num_triggers,
             gp_num_buttons, SDL_GetNumGamepadTouchpads(gamepad_info));
}

void CheckJoystickChanged(void)
{
    // likely invalidated anyway
    attached_joysticks.clear();

    int new_total_joysticks = 0;
    
    SDL_JoystickID *new_joystick_ids = SDL_GetJoysticks(&new_total_joysticks);

    if (new_total_joysticks == 0 || !new_joystick_ids)
    {
        if (gamepad_info)
        {
            SDL_CloseGamepad(gamepad_info);
            gamepad_info = nullptr;
        }
        if (joystick_info)
        {
            SDL_CloseJoystick(joystick_info);
            joystick_info = nullptr;
        }
        joystick_device  = attached_joysticks.end();
        current_gamepad  = 0;
        if (new_joystick_ids)
            SDL_free(new_joystick_ids);
        return;
    }

    // repopulate with what's attached
    for (int i = 0; i < new_total_joysticks; ++i)
    {
        attached_joysticks.emplace(new_joystick_ids[i]);
    }

    SDL_free(new_joystick_ids);

    joystick_device = attached_joysticks.find(current_gamepad);

    // still attached?
    if (joystick_device != attached_joysticks.end())
        return;

    // if not, fall back to the first (in JoystickID order) gamepad

    joystick_device = attached_joysticks.begin();

    if (joystick_info)
    {
        if (gamepad_info)
        {
            SDL_CloseGamepad(gamepad_info);
            gamepad_info = nullptr;
        }
        SDL_CloseJoystick(joystick_info);
        joystick_info = nullptr;
        current_gamepad = 0;
    }

    I_OpenJoystick(*joystick_device);
}

const char *GetJoystickName(void)
{
    if (!gamepad_info)
        return "None";
    else
    {
        const char *name = SDL_GetGamepadName(gamepad_info);
        return name ? name : "None";
    }
}

void SwitchJoystick(int key_pressed)
{
    if (attached_joysticks.empty()) return;

    if (key_pressed == kLeftArrow || key_pressed == kGamepadLeft)
    {
        if (joystick_device == attached_joysticks.begin())
            joystick_device = attached_joysticks.end();
        joystick_device--;
    }
    else if (key_pressed == kRightArrow || key_pressed == kGamepadRight)
    {
        joystick_device++;
        if (joystick_device == attached_joysticks.end())
            joystick_device = attached_joysticks.begin();
    }

    if (current_gamepad != *joystick_device)
    {
        if (gamepad_info)
        {
            SDL_CloseGamepad(gamepad_info);
            gamepad_info = nullptr;
        }
        if (joystick_info)
        {
            SDL_CloseJoystick(joystick_info);
            joystick_info = nullptr;
        }
        current_gamepad  = 0;
        I_OpenJoystick(*joystick_device);
    }
}

//
// Event handling while the application is active
//
void ActiveEventProcess(SDL_Event *sdl_ev)
{
    switch (sdl_ev->type)
    {
#ifdef EDGE_WEB
    case SDL_EVENT_WINDOW_RESIZED:
    {
        printf("SDL window resize event %i %i\n", sdl_ev->window.data1, sdl_ev->window.data2);
        current_screen_width  = sdl_ev->window.data1;
        current_screen_height = sdl_ev->window.data2;
        current_screen_depth  = 24;
        current_window_mode   = kWindowModeWindowed;
        DeterminePixelAspect();
        break;
    }
#endif

    case SDL_EVENT_WINDOW_FOCUS_LOST:
        HandleFocusLost();
        break;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        HandleKeyEvent(sdl_ev);
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
#ifdef EDGE_WEB
        // On web, we don't want clicks coming through when changing pointer
        // lock Otherwise, menus will be selected, weapons fired,
        // unexpectedly
        if (SDL_ShowCursor(SDL_QUERY) == SDL_DISABLE)
            HandleMouseButtonEvent(sdl_ev);
#else
        if (need_mouse_recapture)
        {
            GrabCursor(true);
            break;
        }
        HandleMouseButtonEvent(sdl_ev);
#endif
        break;

    case SDL_EVENT_MOUSE_WHEEL:
        if (!need_mouse_recapture)
            HandleMouseWheelEvent(sdl_ev);
        break;

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        HandleGamepadButtonEvent(sdl_ev);
        break;

    // Analog triggers should be the only thing handled here -Dasho
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        HandleGamepadTriggerEvent(sdl_ev);
        break;

    case SDL_EVENT_MOUSE_MOTION:
        if (eat_mouse_motion)
        {
            eat_mouse_motion = false; // One motion needs to be discarded
            break;
        }
        if (!need_mouse_recapture)
            HandleMouseMotionEvent(sdl_ev);
        break;

    case SDL_EVENT_QUIT:
        // Note we deliberate clear all other flags here. Its our method of
        // ensuring nothing more is done with events.
        app_state = kApplicationPendingQuit;
        break;

    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        CheckJoystickChanged();
        break;

    case SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED:
        SoundDeviceFormatChanged();
        break;

    default:
        break; // Don't care
    }
}

//
// Event handling while the application is not active
//
void InactiveEventProcess(SDL_Event *sdl_ev)
{
    switch (sdl_ev->type)
    {
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        if (!(app_state & kApplicationPendingQuit))
            HandleFocusGain();
        break;

    case SDL_EVENT_QUIT:
        // Note we deliberate clear all other flags here. Its our method of
        // ensuring nothing more is done with events.
        app_state = kApplicationPendingQuit;
        break;

    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        CheckJoystickChanged();
        break;

    case SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED:
        SoundDeviceFormatChanged();
        break;

    default:
        break; // Don't care
    }
}

void ShowGamepads(void)
{
    if (no_joystick)
    {
        LogPrint("Gamepad system is disabled.\n");
        return;
    }

    if (attached_joysticks.empty())
    {
        LogPrint("No gamepads found.\n");
        return;
    }

    LogPrint("Gamepads:\n");

    for (std::set<SDL_JoystickID>::iterator iter = attached_joysticks.begin(), iter_end = attached_joysticks.end(); iter != iter_end;
        ++iter)
    {
        const char *name = SDL_GetGamepadNameForID(*iter);
        if (!name)
            name = "(UNKNOWN)";

        LogPrint("  %2d : %s\n", *iter, name);
    }
}

void StartupJoystick(void)
{
    attached_joysticks.clear();
    joystick_device  = attached_joysticks.end();
    current_gamepad = 0;

    if (FindArgument("no_joystick") > 0)
    {
        LogPrint("StartupControl: Gamepad system disabled.\n");
        no_joystick = true;
        return;
    }

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
    {
        LogPrint("StartupControl: Couldn't init SDL GAMEPAD!\n");
        no_joystick = true;
        return;
    }

    SDL_SetGamepadEventsEnabled(true);

    int total_joysticks = 0;
    SDL_JoystickID *joystick_ids = SDL_GetJoysticks(&total_joysticks);

    LogPrint("StartupControl: %d gamepads found.\n", total_joysticks);

    if (total_joysticks == 0)
    {
        if (joystick_ids)
            SDL_free(joystick_ids);
        return;
    }
    else if (!joystick_ids)
    {
        total_joysticks = 0;
        return;
    }
    else
    {
        for (int i = 0; i < total_joysticks; ++i)
        {
            attached_joysticks.emplace(joystick_ids[i]);
        }
        SDL_free(joystick_ids);
        joystick_device = attached_joysticks.begin(); // Automatically set to first detected gamepad
        I_OpenJoystick(*joystick_device);
    }
}

/****** Input Event Generation ******/

void StartupControl(void)
{
    alt_is_down = false;

    StartupJoystick();
}

void ControlGetEvents(void)
{
    SDL_Event sdl_ev;

    while (SDL_PollEvent(&sdl_ev))
    {
        if (app_state & kApplicationActive)
            ActiveEventProcess(&sdl_ev);
        else
            InactiveEventProcess(&sdl_ev);
    }
}

void ShutdownControl(void)
{
    if (SDL_WasInit(SDL_INIT_GAMEPAD))
    {
        SDL_SetGamepadEventsEnabled(false);
        if (gamepad_info)
        {
            SDL_CloseGamepad(gamepad_info);
            gamepad_info = nullptr;
        }
        if (joystick_info)
        {
            SDL_CloseJoystick(joystick_info);
            joystick_info = nullptr;
        }
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
}

int GetTime(void)
{
    Uint32 t = SDL_GetTicks();

    // more complex than "t*35/1000" to give more accuracy
    return (t / 1000) * kTicRate + (t % 1000) * kTicRate / 1000;
}

int GetMilliseconds(void)
{
    return SDL_GetTicks();
}

//--- editor settings ---
// vi:ts=4:sw=4:noexpandtab
