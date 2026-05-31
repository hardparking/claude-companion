# claude-companion 👾

Turn an [M5Stack Fire](https://docs.m5stack.com/en/core/fire) (ESP32) into an ambient
status display for [Claude Code](https://claude.com/claude-code). A dancing 8-bit
Space Invaders alien on the LCD reflects what Claude is doing in real time — marching
while it thinks, hopping while it works, victory-jumping when it finishes. The Fire's
built-in RGB LED bars glow along in matching colors.

State is pushed to the device over plain HTTP, so anything that can `curl` can drive it.

## How it works

```
Claude Code hooks ──curl──▶ http://claude-fire.local/state?s=working ──▶ M5Stack Fire
```

The firmware connects to WiFi, runs a tiny HTTP server, and animates the alien. A host
script (`scripts/claude-fire.sh`) is wired to Claude Code lifecycle hooks so the display
tracks each session automatically.

## States

| State | Alien animation | LED bars | Color |
|-----------|-------------------------------------------|----------------------------|--------|
| `idle`    | gentle bob, slow leg shuffle              | slow breathe               | cyan   |
| `thinking`| marches side to side                      | comet sweeps back and forth| orange |
| `working` | full dance: struts, hops, squash & stretch| fast pulse                 | orange |
| `waiting` | tense heartbeat pulse                     | heartbeat throb            | amber  |
| `done`    | victory jumps, then auto-reverts to idle  | decaying victory burst     | green  |

The screen and the 10 built-in SK6812 LEDs (two side bars, GPIO 15) are driven from the
same per-state palette, so their color and motion always match.

A firmware watchdog relaxes `thinking`/`working` back to `idle` after ~12s with no update,
so the display can never get stuck.

## Build & flash

Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/)
with the **esp32** (xtensa) toolchain installed (`./install.sh esp32`).

```bash
cp main/wifi_secrets.h.example main/wifi_secrets.h   # then edit in your WiFi
source ~/esp/esp-idf/export.sh
idf.py set-target esp32
idf.py -p /dev/ttyACM0 -b 921600 flash monitor
```

On boot the serial log prints the device IP and mDNS name:

```
CLAUDE-FIRE ONLINE  ->  http://192.168.1.230/
```

`M5GFX`, `espressif/mdns`, and `espressif/led_strip` are pulled automatically from the
component registry on first build (into `managed_components/`, which is gitignored).

## Control it over HTTP

```bash
curl "http://claude-fire.local/state?s=working"   # set a state
curl "http://claude-fire.local/"                  # read current state + IP
```

Valid states: `idle | thinking | working | waiting | done`.

## Wire it into Claude Code

```bash
cp scripts/claude-fire.sh ~/.claude/claude-fire.sh
chmod +x ~/.claude/claude-fire.sh
echo -n claude-fire.local > ~/.claude/claude-fire.host   # or the device IP
```

Then merge the `hooks` block from `hooks.example.json` into `~/.claude/settings.json`.
The notifier is fire-and-forget (0.4s timeout, always exits 0), so it never slows Claude
down or fails a session if the device is off.

## Layout

```
main/claude_companion.cpp     firmware: WiFi, HTTP server, alien animation, LED bars
main/wifi_secrets.h.example   template for your (gitignored) credentials
sdkconfig.defaults            board config: 16MB flash, 8MB PSRAM, esp32 target
scripts/claude-fire.sh        host-side fire-and-forget notifier
hooks.example.json            Claude Code hook wiring
```

## License

MIT — see [LICENSE](LICENSE).
