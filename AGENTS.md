# AGENTS.md

This file applies to the entire repository.

## Context

- This repo is a reference firmware implementation for a BLE-connected desktop buddy device.
- The desktop-to-device contract lives in `REFERENCE.md`. Treat that file as the stable protocol surface.
- The firmware is a PlatformIO + Arduino C++ project targeting ESP32 hardware.
- The current PlatformIO environment is defined in `platformio.ini`.

## Working Style

- Act like a high-performing senior engineer: concise, direct, and execution-focused.
- Prefer simple, maintainable, production-friendly solutions.
- Keep APIs small, behavior explicit, and naming clear.
- Avoid overengineering, heavy abstractions, and unnecessary dependencies.
- Make the smallest change that solves the real problem.

## Change Scope

- Respect the repo's intent as a reference implementation, not a feature sandbox.
- Good changes:
  - Bug fixes that keep the reference firmware working
  - Protocol doc corrections in `REFERENCE.md`
  - Fixes for pairing, rendering, boot, BLE transport, or data parsing regressions
- Avoid unless explicitly requested:
  - New features, new pets, new screens, or speculative UX changes
  - Broad refactors, style-only edits, or dependency churn
  - Reorganizing files or introducing new architecture layers

## Code Guidelines

- Follow the existing straightforward C++ style in `src/`.
- Prefer clear control flow over clever abstractions.
- Keep logic close to where it is used unless reuse is obvious and repeated.
- Do not add indirection for small one-off behaviors.
- Preserve existing file responsibilities:
  - `src/main.cpp`: loop, state machine, screens
  - `src/ble_bridge.cpp`: BLE transport
  - `src/character.cpp`: GIF decode/render
  - `src/data.h`: protocol/data parsing
  - `src/buddy.cpp` and `src/buddies/`: buddy selection and per-species rendering

## Validation

- Use repo-native commands for verification.
- Default build check:

```bash
pio run
```

- For clangd or other static-analysis tools, generate `compile_commands.json` with:

```bash
pio run -t compiledb
```

- If filesystem assets are changed, also verify with:

```bash
pio run -t uploadfs
```

- If a change affects flashing behavior, document the expected command instead of guessing:

```bash
pio run -t upload
```

- Do not claim hardware behavior was validated unless you actually tested on device.

## Files To Read Before Bigger Changes

- `README.md` for product and workflow context
- `REFERENCE.md` for protocol expectations
- `platformio.ini` for build target and dependency truth
- `CONTRIBUTING.md` for contribution boundaries

## Collaboration Notes

- Be explicit about assumptions, especially when hardware validation is not possible.
- Prefer source-backed conclusions over generic embedded advice.
- If README, code, and PlatformIO config disagree, treat the current code and `platformio.ini` as the implementation truth and call out the mismatch.
