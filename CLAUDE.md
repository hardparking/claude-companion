# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP-IDF (v5.x) firmware for an **M5Stack Fire (ESP32, Xtensa)** that turns the device into an
ambient status display for Claude Code. A Space Invaders alien animates on the 240×240 LCD to
reflect Claude's current activity. State is pushed to the device over plain HTTP, so any client
that can `curl` can drive it.

End-to-end flow: `Claude Code hook → scripts/claude-fire.sh → curl http://claude-fire.local/state?s=<state> → firmware → animation`.

## Build, flash, and run

This is a native ESP-IDF project (**not** Arduino / PlatformIO). It requires ESP-IDF v5.x with the
`esp32` (xtensa) toolchain installed.

```bash
cp main/wifi_secrets.h.example main/wifi_secrets.h   # then edit in WIFI_SSID / WIFI_PASS
source ~/esp/esp-idf/export.sh                        # put idf.py on PATH (required every shell)
idf.py set-target esp32                               # only needed once / after target change
idf.py -p /dev/ttyACM0 -b 921600 flash monitor        # build + flash + serial monitor
```

- `idf.py build` — build only. `idf.py monitor` — serial console only (Ctrl-] to exit).
- On boot the serial log prints the IP + mDNS name: `CLAUDE-FIRE ONLINE  ->  http://192.168.1.230/`.
- `M5GFX` and `espressif/mdns` (see `main/idf_component.yml`) are fetched from the component
  registry on first build into `managed_components/` (gitignored). `build/` is also gitignored.
- `wifi_secrets.h` is gitignored — never commit credentials; edit the local copy only.

There is **no test suite and no linter configured**. Verification is manual, over HTTP:

```bash
curl "http://claude-fire.local/state?s=working"   # set a state
curl "http://claude-fire.local/"                  # read current state + IP
```

Valid states: `idle | thinking | working | waiting | done`.

## Architecture

All firmware lives in a single file: **`main/claude_companion.cpp`**. The big picture:

- **Three concerns run concurrently** under FreeRTOS: a WiFi/event handler (STA mode, auto-reconnect),
  an ESP-IDF `httpd` HTTP server (handlers for `GET /state` and `GET /`), and a dedicated animation
  task pinned to core 1 rendering ~33 FPS.
- **State is the only shared data** between the HTTP server and the animation task — a current-state
  value plus a last-updated timestamp. The HTTP handler validates the `s=` query against the five
  legal states and updates it; the animation task reads it each frame to choose choreography.
- **Watchdog / self-healing:** the animation task relaxes `thinking`/`working` back to `idle` after
  ~12s with no update (so a crashed/offline host can never leave the display stuck), and reverts
  `done` to `idle` after its victory animation (~2.6s).
- **Rendering** draws the classic 11×8 invader bitmap (two leg-shuffle frames) scaled up into a
  240×240 sprite backed by PSRAM, then pushes it centered to the LCD. Each state has its own color
  and motion (bob, side-to-side march, hop/squash-stretch dance, heartbeat pulse, victory jumps).
- **LED bars** mirror the screen: the Fire's 10 built-in SK6812 RGB LEDs (GPIO 15, driven via the
  `espressif/led_strip` RMT component) are updated each frame in the same animation task, reusing the
  per-state palette so their color/effect tracks the alien. Brightness is capped via `LED_MAX`.
- **mDNS** advertises the device as `claude-fire.local` so the host script never needs a fixed IP.

### Host integration (outside the firmware)

- `scripts/claude-fire.sh` — fire-and-forget notifier copied to `~/.claude/claude-fire.sh`. Reads the
  target host from `~/.claude/claude-fire.host`, uses a 0.4s timeout, and **always exits 0** so it can
  never slow down or fail a Claude session if the device is off.
- `hooks.example.json` — maps Claude Code lifecycle events to states (`SessionStart`→idle,
  `UserPromptSubmit`→thinking, `PreToolUse`→working, `PostToolUse`→thinking, `Notification`→waiting,
  `Stop`→done). Merge its `hooks` block into `~/.claude/settings.json` to wire it up.

## Hardware config

`sdkconfig.defaults` pins the board profile: `esp32` target, 16MB flash, 8MB Quad PSRAM @ 80MHz
(used for the sprite framebuffer), `FREERTOS_HZ=1000` for smooth animation timing, and a larger main
task stack. Changes that affect the framebuffer, flash layout, or animation timing usually belong
here rather than in code.
