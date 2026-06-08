# Contributing to Claudeputer

Thanks for your interest! Claudeputer is firmware for the M5Stack Cardputer
(1.1 and ADV) that talks to the Claude API over WiFi. Contributions of all
sizes are welcome — bug reports, fixes, features, docs.

## Ground rules

- **Never commit secrets.** `src/config.h` is git-ignored; keep your WiFi
  password and API key out of commits. Only `src/config.h.example` is tracked.
- Be kind. See the [Code of Conduct](CODE_OF_CONDUCT.md).

## Getting set up

```bash
git clone https://github.com/poppnn/claudeputer
cd claudeputer
cp src/config.h.example src/config.h     # optional: bake in credentials
```

Build & flash with [PlatformIO](https://platformio.org/):

```bash
pio run -e cardputer -t upload      # works on both Cardputer 1.1 and ADV
pio device monitor                  # serial logs @ 115200
```

The same binary auto-detects 1.1 vs ADV at runtime (via `M5.getBoard()`), so a
single build covers both boards.

## How it's organized

- `src/main.cpp` — all firmware logic (UI, WiFi/setup, streaming API, SD, etc.).
- `src/anthropic_ca.h` — embedded root CAs for TLS validation of `api.anthropic.com`.
- `webflasher/` — the [ESP Web Tools](https://esphome.github.io/esp-web-tools/) page.
- `.github/workflows/deploy.yml` — CI: builds the firmware, merges a single
  flashable `.bin`, and deploys the web flasher to GitHub Pages.

## Gotchas worth knowing

- **Arduino macros**: `INPUT`, `OUTPUT`, `ERROR` are `#define`s — don't use them
  as enum/identifier names (that's why the screen enum is PascalCase).
- **Keyboard layers**: keys have 3 layers (normal / Shift / **Fn**). When `Fn`
  is held, the fn-layer value lands in `status.hid_keys` (HID keycodes like
  `KEY_UP=0x52`), **not** in `status.word`. Arrow keys are `;`/`.`/`,`/`/`.
- **microSD**: the ESP32 SD library mounts FAT16/FAT32 only (not exFAT). Pins
  are the same on 1.1 and ADV (SCK40 / MISO39 / MOSI14 / CS12).
- **TLS**: certificate validation needs the clock synced (NTP at boot).

## Pull requests

1. Branch off `main`.
2. Keep changes focused; update the README if behavior changes.
3. Make sure it **compiles in CI** (the build job runs on every push/PR).
4. If you can, test on real hardware and say which board in the PR.
5. Fill in the [PR template](.github/PULL_REQUEST_TEMPLATE.md).

No strict style guide — match the surrounding code (2-space indent, ASCII-only
source, descriptive names). Thanks for contributing! 🤖
