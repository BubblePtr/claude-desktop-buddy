# claude-desktop-buddy

Reference firmware for a BLE-connected Claude desktop buddy running on the
Waveshare ESP32-S3-Touch-AMOLED-1.8 board.

Claude for macOS and Windows can stream session state, permission prompts,
recent messages, and character packs to maker devices over Bluetooth LE. This
repo is the firmware side of that example: a small desk pet that sleeps when
Claude is disconnected, wakes when sessions are active, asks for permission
approval on-device, and reacts to interaction.

> Building another device? You do not need this firmware. The stable wire
> contract is [REFERENCE.md](REFERENCE.md): Nordic UART UUIDs, newline-delimited
> JSON schemas, permission responses, and folder push transport.

<p align="center">
  <img src="docs/device.jpg" alt="Waveshare AMOLED desktop buddy running the firmware" width="720">
</p>

## Current Target

The active PlatformIO environment is `waveshare-amoled-18`.

Hardware assumed by the current firmware:

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.8
- Display: 368x448 SH8601 AMOLED over QSPI
- Render model: 184x224 logical canvas, upscaled 2x to the panel
- Power management: AXP2101
- IMU: QMI8658
- RTC: PCF85063
- BLE stack: NimBLE-Arduino
- Storage: LittleFS on the 8 MB flash layout in `no_ota_8mb.csv`

Physical controls used today:

- `A`: upper right-side BOOT button
- `B`: lower right-side PWR button, read through AXP2101 power-key IRQs
- USB-C sits between `A` and `B`

The touchscreen hardware is reset by the boot sequence but is not currently
used for input.

## Port Status

The Waveshare port is functional as a button-driven reference firmware.

Implemented:

- AMOLED bring-up through Arduino_GFX and a 2x framebuffer push path
- BOOT/PWR button handling for screens, menu, approval, denial, and power off
- AXP2101 battery/VBUS status, brightness, and power-key events
- QMI8658 shake and face-down nap detection
- PCF85063 RTC time sync from the desktop bridge
- Secure BLE Nordic UART transport with passkey pairing
- Claude heartbeat parsing, permission responses, and transcript HUD
- ASCII pets, GIF character packs, and LittleFS character storage
- Pet stats for mood, fed progress, energy, level, approvals, denials, and naps
- Local debug state and USB framebuffer snapshot tooling

Known gaps are tracked in [Roadmap](#roadmap).

## Build And Flash

Install PlatformIO Core, then build:

```bash
pio run
```

Flash firmware:

```bash
pio run -t upload
```

If the board has stale data from another firmware, erase first:

```bash
pio run -t erase
pio run -t upload
```

If you changed filesystem assets under `data/`, upload LittleFS:

```bash
pio run -t uploadfs
```

The device also has an on-device factory reset path:

```text
hold A -> settings -> reset -> factory reset -> tap B twice
```

## Pair With Claude

1. In Claude for macOS or Windows, enable developer mode:
   `Help -> Troubleshooting -> Enable Developer Mode`.
2. Open `Developer -> Open Hardware Buddy...`.
3. Click `Connect`.
4. Pick the device advertising as `Claude...`.
5. Enter the 6-digit passkey shown on the device when the OS asks.

Once paired, the desktop bridge reconnects when both sides are awake.

If discovery fails:

- Wake the device with either button.
- Confirm the device is advertising in serial logs: `[ble] advertising as ...`.
- Use the device settings menu to confirm Bluetooth is enabled.
- If the host has stale bonds, use Hardware Buddy `Forget` or send
  `{"cmd":"unpair"}`.

## Controls

| Input | Normal | Pet | Info | Approval |
| --- | --- | --- | --- | --- |
| `A` short press | next screen | next screen | next screen | approve |
| `B` short press | scroll transcript | next page | next page | deny |
| hold `A` | menu | menu | menu | menu |
| hold `A` + tap `B` | next ASCII pet | next ASCII pet | next ASCII pet | - |
| hold `B` about 6s | hard power off | hard power off | hard power off | - |
| shake | dizzy animation | dizzy animation | dizzy animation | - |
| face-down | nap | nap | nap | - |

The screen powers off after 30 seconds of inactivity on battery. A permission
prompt keeps the display awake. The first button press after screen-off only
wakes the screen; it does not also trigger the normal action.

## UI Modes

Primary modes:

- `Normal`: pet/character plus Claude session HUD.
- `Pet`: stats and help pages.
- `Info`: about, buttons, Claude, device, Bluetooth, and credits pages.

Transient overlays:

- `Approval`: replaces the normal HUD while a permission prompt is pending.
- `Menu`: settings, turn off, help, about, demo, close.
- `Settings`: brightness, sound, bluetooth, wifi, led, transcript, ascii pet,
  reset, back.
- `Reset`: delete char, factory reset, back. Destructive actions require a
  second confirmation tap.
- `Passkey`: shown during BLE pairing.
- `Clock`: shown on USB power while idle, if RTC time is valid.
- `Nap`: entered by face-down detection; display dims until face-up.
- `Screen off`: entered after idle timeout on battery.

Render priority is:

- passkey
- clock
- info or pet mode
- normal HUD
- reset/settings/menu overlay on top

## Pet System

The firmware has seven persona states:

| State | Trigger | Feel |
| --- | --- | --- |
| `sleep` | bridge not connected | eyes closed, slow breathing |
| `idle` | connected, nothing urgent | blinking, looking around |
| `busy` | sessions actively running | working, sweating, focused |
| `attention` | permission prompt pending | alert, impatient |
| `celebrate` | level up or completed turn | confetti, bouncing |
| `dizzy` | shake | spiral eyes, wobble |
| `heart` | approved in under 5 seconds | floating hearts |

Pet stats are local to the device:

- `Level`: cumulative output tokens, 50K tokens per level.
- `Fed`: current-level token progress, one pip per 5K tokens.
- `Mood`: approval response speed, reduced by a high denial ratio.
- `Energy`: starts at 3/5, refills after a face-down nap, drains over time.
- `Naps`: cumulative face-down time persisted in NVS.

The desktop protocol does not send mood, fed, energy, or level directly. It
sends tokens, session counts, transcript entries, and permission prompts; the
firmware derives pet behavior from those inputs.

## ASCII Pets

The built-in ASCII renderer includes eighteen species. Each species implements
the seven persona states above. Use `hold A + tap B` or
`settings -> ascii pet` to cycle species. The selected species is stored in NVS.

## GIF Characters

Custom GIF character packs can be pushed from the Hardware Buddy window by
dropping a folder onto the drop target. The firmware writes the pack to
LittleFS and switches to GIF mode live.

A character pack is a flat folder with `manifest.json` and 96 px wide GIFs:

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_0.gif", "idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

State values may be a filename or an array. Arrays rotate after a GIF loop
ends, which is useful for idle variation.

Keep character art around 96 px wide. Heights up to roughly 140 px fit the
current portrait layout. Crop tightly around the character; transparent margins
waste screen area and make the sprite feel small.

For local iteration, stage the example pack and upload it over USB:

```bash
tools/flash_character.py characters/bufo
```

That script prepares `data/characters/bufo` and runs `pio run -t uploadfs`.

Use `settings -> reset -> delete char` to remove the current GIF character and
return to ASCII mode.

## Local Debugging

The preview page can drive ASCII pet states and pull low-frequency framebuffer
snapshots over USB.

Start a local server:

```bash
python3 -m http.server 8000
```

Open:

```text
http://localhost:8000/docs/preview.html
```

Repo-local debug command for forced state:

```json
{"cmd":"debug_state","state":"auto|sleep|idle|busy|attention|celebrate|dizzy|heart"}
```

Repo-local USB screenshot command:

```json
{"cmd":"screenshot"}
```

The screenshot path streams the current 184x224 framebuffer as chunked `rgb332`
data. These debug commands are intentionally outside the stable `REFERENCE.md`
contract.

## Roadmap

Remaining Waveshare-specific adaptation work:

- Touch input: add FT3168 support or remove touchscreen assumptions from docs.
  The current firmware resets touch hardware but does not read touch events.
- Settings cleanup: either implement real behavior for `sound` and `wifi`, or
  hide those settings. `sound` currently maps to an empty `beep()` stub, and
  `wifi` only persists a preference.
- Battery/status polish: document that AXP2101 does not expose instantaneous
  battery current here, or replace the `mA` field with values this board can
  report reliably.
- Current-device media: replace inherited images that still describe the old
  device with Waveshare AMOLED photos/screenshots.
- Hardware validation checklist: keep a short release checklist for build,
  flash, BLE pairing, permission approve/deny, GIF push, reset, nap, shake, and
  screen-off behavior on the real board.
- Touch-oriented UX, if desired: decide whether the touchscreen should be part
  of the reference interaction model. If yes, map simple taps/swipes to existing
  A/B actions before adding new screens.

Out of scope unless deliberately expanded:

- Supporting both the old M5StickC Plus and the Waveshare board in one codebase.
- Adding WiFi features.
- Adding new pet progression systems beyond the existing stats/animations.
- Reintroducing landscape clock rendering.

## Project Layout

```text
src/
  main.cpp       - hardware setup, loop, state machine, UI screens
  buddy.cpp      - ASCII species dispatch and render helpers
  buddies/       - one file per ASCII species
  ble_bridge.cpp - secure Nordic UART Service over NimBLE
  character.cpp  - GIF decode and render from LittleFS
  data.h         - heartbeat JSON parse and state derivation
  xfer.h         - folder push receiver and command acks
  stats.h        - NVS-backed stats, settings, owner, species choice
characters/      - example GIF character packs
docs/            - preview/debug pages and port notes
tools/           - character prep and upload helpers
```

## Availability

The Hardware Buddy BLE API is only available when Claude desktop developer mode
is enabled. It is intended for makers and developers and is not an officially
supported product feature.
