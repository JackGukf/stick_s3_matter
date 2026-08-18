# Matter light for the M5Stack StickS3

A Matter **extended color light** for the M5Stack StickS3 (ESP32-S3-PICO-1-N8R8),
built on [ESP-Matter](https://docs.espressif.com/projects/esp-matter/en/latest/esp32s3/developing.html).
It commissions over BLE, runs over Wi-Fi, and pairs with the Apple Home app.

The StickS3 has no LED, so the light *is* the on-board 1.14" ST7789P3 LCD: the
panel is filled with the current color and the backlight PWM carries the
brightness level. On/off, brightness, hue/saturation, color temperature and
CIE xy all drive the display.

## Layout

| Path | What it is |
| --- | --- |
| `main/app_main.cpp` | Matter node: root node + extended color light endpoint |
| `main/app_driver.cpp` | Maps cluster attributes to the light, KEY1 button handling |
| `components/stick_s3_light/` | Board driver: PMIC power rail, ST7789P3 panel, backlight |
| `partitions.csv` | Partition table (from the esp-matter light example) |
| `sdkconfig.defaults` | Matter + StickS3 config (8 MB flash, USB-Serial/JTAG console) |

Board facts this depends on (M5Stack StickS3 PinMap + M5GFX `board_M5StickS3`):

- LCD: MOSI G39, SCK G40, DC G45, CS G41, RST G21, backlight G38; 135x240 with a
  52/40 offset, color inversion on.
- The LCD rail (L3B) is **not** wired to the SoC — it is gated by GPIO2 of the
  M5PM1 PMIC at I2C address 0x6E (SDA G47, SCL G48) and needs ~100 ms to settle
  before the panel is initialized.
- Buttons: KEY1 G11, KEY2 G12, active low. KEY1 toggles the light; holding it
  5 s factory resets.

## Build

ESP-IDF **v5.4.1** at `~/esp/esp-idf` and esp-matter **v1.4.2** at `~/esp/esp-matter`
are already installed on this machine, so each new terminal only needs:

```bash
source ~/esp/esp-idf/export.sh && source ~/esp/esp-matter/export.sh && export IDF_CCACHE_ENABLE=1
```

Then, from this directory:

```bash
idf.py build
```

`sdkconfig` is generated and git-ignored; on a fresh checkout run
`idf.py set-target esp32s3` once before building.

## Flash

The StickS3 uses the ESP32-S3's native USB-Serial/JTAG, so it enumerates as
`/dev/ttyACM0` when plugged in.

```bash
idf.py -p /dev/ttyACM0 erase-flash flash monitor
```

Under WSL2 the USB device must be attached to the Linux VM first, from an
**admin PowerShell on Windows**:

```powershell
usbipd list
usbipd attach --wsl --busid <BUSID>
```

If the board does not enter download mode on its own, hold the front button
while plugging it in.

## Pair with the Apple Home app

The firmware is built with the standard Matter **test** credentials
(vendor 0xFFF1, product 0x8000, discriminator 3840, passcode 20202021), so Home
will show an "uncertified accessory" warning — that is expected for a
development build. Pairing needs a home hub (HomePod or Apple TV) on the same
Wi-Fi.

1. Flash and boot the board. The serial monitor prints the onboarding payload.
2. In Home: **+** → *Add Accessory* → *More options…* → **My Accessory Isn't Shown Here**,
   then enter the setup code, or scan the QR code below.
3. Home commissions over BLE and hands the board your Wi-Fi credentials.

| | |
| --- | --- |
| Manual setup code | `3497-011-2332` |
| QR payload | `MT:Y.K9042C00KA0648G00` |

Render the QR at <https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT%3AY.K9042C00KA0648G00>.

Once paired, the Home tile controls power, brightness and color, and the panel
follows. KEY1 toggles it locally, and Home reflects the change.

## Reset

- **Factory reset:** hold KEY1 for 5 s, or run `matter esp factoryreset` in the
  serial console. Do this before re-pairing — a commissioned device will not
  accept a second commissioning until the fabric is removed.
- The serial console also has `matter esp wifi` and `matter esp dia` commands.

## Notes

- The build is Wi-Fi + BLE; the ESP32-S3 has no 802.15.4 radio, so Thread is not
  an option on this board.
- PSRAM is left disabled — the app fits in internal RAM (1.5 MB image, 23% free
  in a 1.9 MB OTA partition). Enable it in `menuconfig` if you extend the app.
- Confirmed on hardware: the PMIC power-up sequence brings the panel to life,
  BLE commissioning into the Apple Home app succeeds, and the Home brightness
  slider drives the backlight.
- Not yet confirmed on hardware: the color path (hue/saturation, color
  temperature, xy) and the KEY1 toggle / factory-reset long press.
- One episode of the accessory going "No Response" in Home and recovering on its
  own has been seen, cause not yet established — the serial log was not captured
  at the time. If it recurs, catch `idf.py monitor` output during the outage: a
  boot banner means the board reset, no banner means it only lost the network.
