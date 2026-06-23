# R36S / RK3326 button maps

This device exposes its gamepad through **two different layers**, and TubeLite's
two processes read different ones. Keep them straight when editing input code.

## 1. Raw evdev codes — used by the **daemon** (`src/daemon.cpp`)

The background daemon reads `/dev/input/event*` directly, so it matches on raw
Linux `EV_KEY` codes (see `namespace btn` in `daemon.cpp`):

| Button | evdev code               |
| ------ | ------------------------ |
| A      | BTN_EAST (305)           |
| B      | BTN_SOUTH (304)          |
| X      | BTN_NORTH (307)          |
| Y      | BTN_WEST (308)           |
| L1     | BTN_TL (310)             |
| R1     | BTN_TR (311)             |
| L2     | BTN_TL2 (312)            |
| R2     | BTN_TR2 (313)            |
| Select | BTN_TRIGGER_HAPPY1 (704) |
| Start  | BTN_TRIGGER_HAPPY2 (705) |
| L3     | BTN_TRIGGER_HAPPY3 (706) |
| R3     | BTN_TRIGGER_HAPPY4 (707) |
| Fn     | BTN_TRIGGER_HAPPY5 (708) |

## 2. SDL joystick indices — used by the **app** (`src/app.cpp`)

The main app reads the pad through SDL's **raw joystick** layer
(`handleJoyButton`), **never** as an SDL game controller. This is deliberate:
some ArkOS images ship a `gamecontrollerdb` entry that would remap the pad
(swapping A/B and dropping Select/Start), and others don't — opening it as a
joystick gives one deterministic map on every card. These indices are what
`handleJoyButton`'s `switch` matches on:

| Index | Button | Index | Button       |
| ----- | ------ | ----- | ------------ |
| 0     | B      | 9     | D-Pad Down   |
| 1     | A      | 10    | D-Pad Left   |
| 2     | X      | 11    | D-Pad Right  |
| 3     | Y      | 12    | Select       |
| 4     | L1     | 13    | Start        |
| 5     | R1     | 14    | L3           |
| 6     | L2     | 15    | R3           |
| 7     | R2     | 16    | Fn           |
| 8     | D-Pad Up |     |              |

> ⚠️ Do **not** reintroduce `SDL_GameControllerOpen` in `App::openController()`.
> It re-enables SDL's mapping layer and brings back the A/B swap + dead
> Select/Start on cards whose `gamecontrollerdb` matches this pad, and causes
> double events (one press → a controller event *and* a joystick event).
