# Development log

Running notes for the StickS3 Matter light. Append a dated entry per session;
keep **Current status** and **Open questions** rewritten to reflect reality
rather than growing a pile of stale text. User-facing build/flash/pairing
instructions belong in [README.md](README.md), not here.

## Current status

`v0.3.2`, delivered over the air. Commissions into the Apple Home app
and the level slider worked on hardware before the face landed. Local git only;
no remote configured, nothing pushed.

| Area | State |
| --- | --- |
| Build | Clean. 1.53 MB image, 22% free in the 1.9 MB OTA partition |
| PMIC power-up → panel lit | Confirmed on hardware |
| BLE commissioning into Home | Confirmed on hardware |
| Level → backlight PWM | Confirmed on hardware, then **replaced** by the bulb face |
| Bulb face (all five states) | Confirmed on hardware |
| Matter OTA, end to end | Confirmed on hardware, twice (v0.3.0 -> v0.3.1 -> v0.3.2) |
| Second fabric alongside Apple Home | Confirmed on hardware |
| Color path (hue/sat, temp, xy) | **Untested on hardware** |
| KEY1 toggle, 5 s factory reset | **Untested on hardware** |
| Stability over hours | One unexplained "No Response" episode, self-recovered |

## Environment

- ESP-IDF **v5.4.1** at `~/esp/esp-idf` — the version esp-matter v1.4.2 asks for.
  (`~/esp/esp-idf-v5.5.4` also exists; not used here.)
- esp-matter **release/v1.4.2** at `~/esp/esp-matter`, submodules checked out,
  `install.sh` run 2026-08-17. Its connectedhomeip submodule is pinned at
  `bc661692` (2025-12-02) and implements **Matter 1.4.2** — the authority for
  that is `kSpecificationVersion = 0x01040200` in
  `src/app/SpecificationDefinedRevisions.h`, which is what the device reports in
  Basic Information. Do **not** trust the `SPECIFICATION_VERSION` file at the
  CHIP repo root; it still reads `1.2.0` and is stale. The submodule is a
  `--depth 1` clone, so `git log` there cannot date anything either.
- Newer upstream exists: esp-matter has `release/v1.5` and `release/v1.6`
  branches, and connectedhomeip is tagged up to `v1.5.1.0` (no 1.6 tag yet).
  Both newer esp-matter branches want ESP-IDF **v5.5.5**. Nothing in this
  project needs them today — see Open questions before spending the upgrade.
- Per terminal:
  `source ~/esp/esp-idf/export.sh && source ~/esp/esp-matter/export.sh && export IDF_CCACHE_ENABLE=1`
- Development is in WSL2, so the board must be attached from an admin PowerShell
  with `usbipd attach --wsl --busid <BUSID>` before `/dev/ttyACM0` exists.

## Hardware facts

The board is the **M5StickS3** (ESP32-S3-PICO-1-N8R8, 8 MB flash, 8 MB PSRAM) —
*not* the ESP32-based StickC Plus2. Sources: the M5Stack StickS3 PinMap, M5GFX
`board_M5StickS3` in `src/M5GFX.cpp`, the M5PM1 register map in
`m5stack/M5PM1/src/M5PM1.h`, and a hardware-verified Zephyr board port
(`thc1006/zephyr-m5stack-sticks3`), which agree with each other.

- **No LED on the board.** The LCD is the only light-emitting part, which is why
  the stock esp-matter `ws2812` driver has nothing to drive here.
- LCD ST7789P3, 135x240: MOSI G39, SCK G40, DC G45, CS G41, RST G21, backlight
  G38. Offsets x=52, y=40, color inversion **on**.
- **The LCD rail (L3B) is not wired to the SoC.** It is gated by GPIO2 (PYG2) of
  the M5PM1 PMIC at I2C 0x6E (SDA G47, SCL G48), and needs ~100 ms to settle
  before panel init. Sequence: clear the 2-bit function field for pin 2 in 0x16
  (plain GPIO), set bit 2 in 0x10 (output), clear bit 2 in 0x13 (push-pull), set
  bit 2 in 0x11 (high), write 0x00 to 0x09 to disable I2C idle sleep.
- Buttons KEY1 G11 / KEY2 G12, active low with pull-up.
- Console is the ESP32-S3 **native USB-Serial/JTAG**, not a USB-UART bridge, so
  the build sets `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` and the port is
  `/dev/ttyACM0`.

## Design decisions

- **The LCD is the light.** The panel is filled with the current color and the
  backlight PWM carries the Matter level. Colors are always converted at full
  value so the level lives in exactly one place; the backlight never drops below
  a floor duty while on, so Matter level 1 stays visible.
- **esp-matter's `device_hal` board layer was dropped.** Its `device` component
  resolves board sources relative to `device_hal/device`, so supporting a new
  board through it means writing into the esp-matter checkout. A project-local
  `components/stick_s3_light` keeps everything here instead. The color
  conversions are still reused from `device_hal/led_driver/utils/color_format.c`
  by absolute path, so color behavior matches the stock example.
- Built for Wi-Fi + BLE. The ESP32-S3 has no 802.15.4 radio, so Thread is not an
  option on this board.
- PSRAM left disabled — the app fits in internal RAM, and octal PSRAM adds a
  boot-failure risk that buys nothing yet.

## Gotchas hit and resolved

- **`/home/jackgu` is a git repo with zero commits**, and the `esp_insights`
  managed component calls `git_describe`, which walks up into it and fails CMake
  configure with `file failed to open for reading .../git-data/head-ref`. Fixed
  by `git init` + one commit in the project directory, which shadows it. This
  will hit any IDF project created under the home directory.
- **The vendor color helpers disagree on scale.** `hsv_to_rgb()` returns 0-100
  per channel while `xy_to_rgb()` returns 0-255 and wants its brightness
  argument on a 0-255 scale. Mixing them silently makes hue/saturation colors
  ~2.5x too dim. `hs_to_rgb()` in the driver normalizes everything to 0-255.
- `GPIO_NUM_11` needs an explicit `#include <driver/gpio.h>`; the stock example
  got it transitively through `device.h`.

## Commissioning

Standard Matter **test** credentials: vendor 0xFFF1, product 0x8000,
discriminator 3840, passcode 20202021. Apple Home shows an "uncertified
accessory" warning, which is expected. Manual code `3497-011-2332`, QR payload
`MT:Y.K9042C00KA0648G00` (derived from the config and cross-checked against the
published value for these parameters). Pairing needs a HomePod or Apple TV as
hub.

## Open questions

1. **The "No Response" episode.** The accessory went unresponsive in Home and
   recovered on its own; no serial log was captured. The USB also dropped off
   usbip around then, but that is weak evidence — an unplug or a stopped
   `usbipd` looks identical from inside WSL. Next time it happens, capture
   `idf.py monitor` **during** the outage: a boot banner and reset reason means
   the board reset (suspects: the Matter task blocking in a full-screen redraw,
   or a brownout on the backlight current spike); no banner means it only lost
   Wi-Fi or its mDNS advertisement.
2. **The color path has never run.** It is the largest untested piece, and it is
   exactly where the scale mismatch above was found. Setting a color from Home
   exercises it in seconds.
3. **No git remote.** `v0.1.0` exists locally only; `gh` is not installed on
   this machine.

## Next steps

- Set a color from Home and confirm the hue is roughly right.
- Press KEY1 (toggle) and hold it 5 s (factory reset); confirm Home follows.
- Decide where to push, then `git remote add origin <URL>` and
  `git push -u origin main --follow-tags`.
- Flash the bulb face and confirm it on hardware: the 1 % sliver, the level
  readout tracking a Home slider drag, and the footer's build string.
- Decide the two open design questions the face raised: whether footer line 1
  should carry the Basic Information `NodeLabel` instead of VID/PID/EP, and
  whether the build string should be the Matter `SoftwareVersion` (still 0)
  rather than `git describe`.
- KEY2 (G12) is still unused. It could wake a plain info screen while the light
  is off, since identity and build are invisible on a dark panel.

## Sessions

### 2026-08-27 (later) — sdkconfig drift guard, second OTA

Closed the last of the three traps. The other two were already fixed and proven
by the v0.3.1 update; this one had only been documented.

- `tools/check_sdkconfig.py` compares every symbol in sdkconfig.defaults against
  the generated sdkconfig at configure time and fails the build naming the
  symbol. It only fires when the defaults are edited with an sdkconfig already
  present -- the one case that previously built successfully while ignoring the
  edit. Verified by reproducing the trap deliberately.
- Shipped it as v0.3.2 over the air. The board took it from ota_1 back into
  ota_0 (0x20000), alternating slots as intended, and stayed there across a
  reboot, so rollback confirmation holds on both slots.

### 2026-08-27 — Matter OTA, end to end

Delivered v0.3.1 to the board over Wi-Fi. Tagged `v0.2.0` first as a restore
point. Three things were wrong, none of them the OTA Requestor, which was
already compiled in and on endpoint 0 the whole time.

- **`CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER` is inert in this project.** The chip
  component reads it only under `CONFIG_APP_PROJECT_VER_FROM_CONFIG`, and
  otherwise falls through to `PROJECT_VER_NUMBER` -- hardcoded to 1 in the
  esp-matter example this project came from. The board reported SoftwareVersion
  1 while calling itself v0.3.0, and `build/esp-idf/chip/args.gn` confirmed
  `chip_config_software_version_number = 1`. An update would still have applied,
  but the device would have kept reporting 1 afterwards, so the provider would
  have offered the same image in a loop forever. Now derived from version.txt.
- **`sdkconfig.defaults` only seeds a fresh `sdkconfig`.** The first attempt
  silently kept the old values; `rm sdkconfig` is required after editing it.
- **Rollback needs a call nobody makes.** Neither esp-matter nor CHIP's ESP32
  platform calls `esp_ota_mark_app_valid_cancel_rollback()`; CHIP's
  `ConfirmCurrentImage()` only compares version numbers. Enabling rollback
  without adding that call would revert every OTA on the next reboot. app_main
  now makes it on IP acquisition. Verified by rebooting after the update: the
  board still loaded from `0x200000` (ota_1) rather than reverting.

Host side: built `chip-ota-provider-app`, commissioned it as node 1, and added
the light to that fabric as node 2 via Home's *Turn On Pairing Mode* -- Apple
Home was unaffected.

Torn down afterwards: `default-otaproviders` cleared, the chip-tool fabric
removed with `operationalcredentials remove-fabric`, and the provider stopped.
The board is back to three fabrics and one free slot. Identifying them needed
the Matter DCL, since two look alike at a glance:

| Index | VendorID | Who |
| --- | --- | --- |
| 1 | 0x1349 | Apple Home |
| 2 | 0x1384 | Apple Keychain -- also Apple, *not* spare |
| 3 | 0xFFF1 | the dashboard (test vendor ID, so not in the DCL) |

Removing the chip-tool fabric mattered rather than just walking away from it:
chip-tool's credentials live in `/tmp`, which systemd empties on boot
(`D /tmp` in `/usr/lib/tmpfiles.d/tmp.conf`). Abandoning the fabric would have
left a slot consumed by a controller that no longer exists, and the device only
supports five. Next time, pair chip-tool again through Home's pairing mode.

**The WSL blocker.** The device discovered the provider and then silently failed
to reach it. Cause: WSL's Hyper-V firewall has `DefaultInboundAction: Block`,
and its only inbound Allow rules are ICMP and mDNS (UDP 5353) -- so discovery
worked and the CASE handshake on UDP 5565 was dropped. One
`New-NetFirewallHyperVRule` for 5565 fixed it. See README for the command.

Note `announce-otaprovider` alone is not enough: it is a one-shot hint, and the
requestor needs `default-otaproviders` written first or it has nowhere to query.

### 2026-08-25 → 2026-08-26 — the bulb face

Replaced the whole-panel color fill with a drawn light bulb, after reviewing a
GUI design of the five states first.

- The level no longer rides on the backlight: it is the bulb's fill height and a
  percentage readout. The backlight is now just on or off, following `OnOff`,
  and colour tints the bulb instead of the whole panel.
- `stick_s3_light_set_brightness(0-100)` became `stick_s3_light_set_level(1-254)`
  — the face shows the raw Matter level, so the remap belongs on the panel side.
- Split the component: `face.c` draws, `stick_s3_light.c` owns the hardware,
  state and the render task. `tools/facepreview.c` links the real `face.c` on
  the host and writes a PPM per state, which is how the layout was checked
  without hardware.
- Rejected LVGL (~250 kB against a 1.92 MB slot) and a framebuffer (63 kB, no
  PSRAM on this target). The face is flat spans plus a generated 5x7 font
  (`tools/genfont.py`, 665 bytes) rendered into a 30-line band buffer.
- Level writes are coalesced over 40 ms on a dedicated render task, so a slider
  drag repaints once rather than flooding SPI.
- `PROJECT_VER` was pinned to `"1.0"` from the example; unpinned it so ESP-IDF
  derives it from `git describe`, which is what the footer shows.
- The first render hid the 1 % fill: the neck is drawn over the bottom of the
  glass, so a level measured to the circle's bottom was invisible. The fill is
  now measured to the top of the neck.
- Image grew 2.5 kB to 1,527,200 bytes; 22 % of the OTA slot still free.
- Tagged `v0.1.1` at the previous head (documentation-only commits) so the build
  string had a clean base.

**On hardware** (user), across four flashes:

- The face works, but the first build showed the level as several overlapping
  partial numbers. Cause: `esp_lcd_panel_draw_bitmap()` queues the colour
  transfer and returns while DMA is still reading the band buffer
  (`esp_lcd_panel_io_spi.c:394`), so the next band was drawn over data still in
  flight. The old whole-panel fill had the same race but every band was an
  identical solid colour, so it never showed. Fixed with an
  `on_color_trans_done` callback and a semaphore the render loop waits on.
- Small text read as blurry on the physical panel: single-pixel strokes plus dim
  greys, worse now the backlight sits at full duty instead of tracking the level.
  Dropped the `lvl n/254` line, the device type and the VID/PID/EP line, then
  spent the space on larger, brighter type.
- The screw base's two black thread lines read as stray artifacts; removed, and
  the neck recoloured to match the base so the stem is one piece.
- Bulb was too small and sat too high: radius 30 -> 40, ring 2 -> 3 px, moved
  down, percentage back to 5x scale to pay for the room.
- `git describe` put `-dirty` on the panel, which is git plumbing rather than a
  build number. Replaced with `version.txt` (version and RC) plus a CMake-
  appended short commit, drawn as two lines split at the last `-`.

### 2026-08-17 → 2026-08-18 — initial bring-up

Set up the project under `stick_s3` from the esp-matter light example, targeting
the Apple Home app.

- Found the toolchain already installed and version-matched; only esp-matter's
  `install.sh` still needed running.
- Identified the board as the M5StickS3 and established the pin map, the PMIC
  LCD power sequence and the panel offsets from the vendor sources above.
- Wrote `components/stick_s3_light` (PMIC rail, ST7789P3 over SPI, LEDC
  backlight) and rewired `main/app_driver.cpp` onto it, dropping `device_hal`.
- Worked around the empty home-directory git repo; build went clean.
- Computed the pairing codes and wrote the README.
- Commits: `1035df0`, `cadbe7d`, `90c7094`; tagged `v0.1.0`.

**On hardware** (user): the panel lit, the board commissioned into Home on the
first try, and the brightness slider drove the backlight. One "No Response"
episode occurred and cleared on its own, uninvestigated — see Open questions.
