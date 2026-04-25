# asustor-as68xx

Linux platform driver for the **ASUSTOR Lockerstor Gen3 (AS68xxT)** NAS series.

## Supported models

| Model    | Bays | Status                                                           |
| -------- | ---- | ---------------------------------------------------------------- |
| AS6804T  | 4    | Best-effort (same board family; not verified on hardware)        |
| AS6806T  | 6    | Supported (verified on hardware)                                 |
| AS6808T  | 8    | Best-effort; disk LED pins for bays 7–8 not yet mapped           |
| AS6810T  | 10   | Best-effort; disk LED pins for bays 7–10 not yet mapped          |

All four variants share the AMD Rembrandt/Fox mainboard, so MCU fan/LED
control, LCD module, backup button, USB copy LED and LAN LED work on every
model. Only the per-bay disk activity/error LEDs for bays 7–10 are missing
until their GPIO pins are probed on hardware.

> **Have an AS6808T or AS6810T and want to help?** See
> [research/probing-disk-leds.md](research/probing-disk-leds.md) for a
> step-by-step `libgpiod` probing guide to map the missing disk LED pins.

Earlier ASUSTOR generations (AS6xx, AS61xx, AS66xx, AS67xx, FS67xx, Flashstor
Gen 2, etc.) are **not** covered by this driver — their I/O paths differ
completely.

## What this driver provides

`asustor_as68xx.ko` registers the following on a supported AS68xxT board:

- **GPIO LEDs** via a child `leds-gpio` platform device on the AMD FCH
  pin controller (`AMDI0030:00` / `pinctrl-amd`):
  - `asustor:green:usbcopy` — LED inside the USB backup button (off by
    default)
  - `asustor:blue:lan` — software-controllable LAN LED (on by default)
  - `sata{1..6}:green:disk` — per-bay activity LED, default trigger
    `disk-activity` (on by default; see *Per-bay disk activity* below
    for true per-bay blinking)
  - `sata{1..6}:red:disk` — per-bay error/fault LED (off by default)
- **USB backup button** via a child `gpio-keys-polled` platform device
  (input device `asustor-keys`, reports `KEY_COPY`, 50 ms poll)
- **MCU-controlled peripherals** (`asustor_mcu`) over `/dev/ttyS1` at
  115200 8N1:
  - hwmon device named `asustor_mcu` exposing the chassis fan as `pwm1`
    (RW, 0–255), `pwm1_enable` (RW, accepts only `1` — manual mode), and
    `fan1_input` (RO tachometer, RPM)
  - Four front-panel LED class devices that combine into mixed colours:
    - `asustor:blue:power` (default trigger `default-on`)
    - `asustor:orange:power`
    - `asustor:green:status` (default trigger `timer`)
    - `asustor:red:status` (default trigger `panic`)

    All four support `brightness` (0/1) and hardware blink via `blink_set`,
    which maps the requested period to one of four MCU-supported speeds
    (≈500/250/125/63 ms half-period).
- **Front-panel LCD module** (`asustor_lcm`) over `/dev/ttyS2` at 9600 8N1:
  - Owns the LCD controller enable line (GPIO 4) via a `gpiod_lookup_table`
    — not exposed as an LED
  - Sysfs attributes on the `asustor_lcm` platform device:
    `lcd_line0` (RW), `lcd_line1` (RW), `lcd_clear` (WO)
  - The four LCD navigation buttons are reported via a separate input
    device named `ASUSTOR LCD Buttons` (`KEY_UP`, `KEY_DOWN`, `KEY_ESC`,
    `KEY_ENTER`)

The driver also accepts a `force_device=` module parameter
(`AS6804`/`AS6806`/`AS6808`/`AS6810`) to bypass DMI/PCI auto-detection.

Temperature sensors are handled by stock in-tree kernel drivers — this
driver does not duplicate them. On the AS6806T this includes `k10temp`
(CPU package), `spd5118` (per-DIMM DDR5 temperature), `nvme` (per-SSD
composite) and `amd-xgbe` (10GbE PHY).

## Build & install

```sh
# Build against the running kernel
make

# Install to /lib/modules/$(uname -r)/kernel/drivers/platform/x86/
sudo make install
sudo modprobe asustor_as68xx
```

### DKMS

```sh
sudo make dkms          # register, build and install with DKMS
sudo make dkms_clean    # remove from DKMS
```

## Usage

After loading the module, the following interfaces are available. All paths
are stable identifiers; the leading `hwmonN`/`inputN` numbers are not, so
match by `name` when scripting.

### Fan control (hwmon)

A single chassis fan PWM and its tachometer are exposed by the MCU:

```sh
# Find the asustor_mcu hwmon node
HWMON=$(grep -l '^asustor_mcu$' /sys/class/hwmon/hwmon*/name | xargs dirname)

# Read RPM
cat $HWMON/fan1_input

# Read current duty cycle (0..255)
cat $HWMON/pwm1

# Set duty cycle to ~50%
echo 128 | sudo tee $HWMON/pwm1
```

Notes:
- Only manual mode is supported (`pwm1_enable` accepts `1` only).
- The first `fan1_input` reading after a PWM change can be bogus; poll a
  second time if the value looks wrong.
- An example `fancontrol(8)` configuration is provided in
  [`examples/fancontrol`](examples/fancontrol). Copy it to `/etc/fancontrol`
  and adjust the `hwmon` numbers if needed (they are not stable across
  boots — `pwmconfig` will regenerate them).

### LEDs

LED class devices appear under `/sys/class/leds/`. Use the standard sysfs
interface (write `brightness`, attach a `trigger`, etc.).

| Name                       | Source | Notes                                |
| -------------------------- | ------ | ------------------------------------ |
| `asustor:blue:power`       | MCU    | Front-panel power LED (blue)         |
| `asustor:orange:power`     | MCU    | Front-panel power LED (orange)   |
| `asustor:green:status`     | MCU    | Front-panel status LED (green); default trigger `timer` |
| `asustor:red:status`       | MCU    | Front-panel status LED (red); default trigger `panic`   |
| `asustor:green:usbcopy`    | GPIO   | LED inside the USB backup button     |
| `asustor:blue:lan`         | GPIO   | Software-controllable LAN LED        |
| `sataN:green:disk`         | GPIO   | Per-bay activity LED (default trigger `disk-activity`; see below) |
| `sataN:red:disk`           | GPIO   | Per-bay error/fault LED              |

#### Per-bay disk activity

The kernel's `disk-activity` trigger (the default attached to every
`sataN:green:disk` LED) is **global**: it blinks every LED bound to it
on any block-device I/O, so all six green LEDs blink together. It
serves as a "something is happening" indicator but cannot show which
specific bay is active.

For real per-bay blinking, use the `blkdev` trigger
(`CONFIG_LEDS_TRIGGER_BLKDEV`, ≥ 6.2) and bind each bay's LED to the
corresponding `/dev/disk/by-path/...-ata-N` device. A ready-made udev
rule is provided in
[examples/disk-activity-leds/60-asustor-disk-leds.rules](examples/disk-activity-leds/60-asustor-disk-leds.rules):

```sh
sudo cp examples/disk-activity-leds/60-asustor-disk-leds.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=block --action=add
```

Installing the rule atomically replaces the `disk-activity` trigger on
each LED, so there's no conflict with the default.

#### ZFS-driven per-bay error LEDs

The red `sataN:red:disk` LEDs have no kernel-side meaning — they're an
output you set deliberately. To make them reflect ZFS vdev health
(*solid red* on FAULTED/UNAVAIL/REMOVED, *slow blink* on DEGRADED, off
on ONLINE), this repo ships a generic bay→LED helper plus ZFS glue:

| File                                                | Role                                                                |
| --------------------------------------------------- | ------------------------------------------------------------------- |
| `examples/asustor-bay-led`                           | Generic helper: `asustor-bay-led <bay-or-device> <off\|on\|blink>`. Resolves a `/dev/sdX`, `/dev/disk/by-path/...`, `/dev/disk/by-id/...` or bay number to the right `sataN:red:disk` LED. Not ZFS-specific — equally useful for mdadm/smartd/manual locate. |
| `examples/zfs-error-leds/asustor-zfs-sync-leds`      | Walks `zpool status` and reconciles every bay's red LED with current vdev state. Run at boot and on demand. |
| `examples/zfs-error-leds/statechange-led.sh`         | ZED zedlet that updates the LED on live vdev `statechange` events. |
| `examples/zfs-error-leds/asustor-zfs-leds.service`   | systemd oneshot that calls `asustor-zfs-sync-leds` after pool import. |

The boot-time sync is needed because ZED only fires on state
*transitions*. If a pool is imported already-degraded (after a reboot
or with a drive pulled while powered off), no `statechange` event ever
fires and the live zedlet alone would leave the LED dark.

Install (Debian/Proxmox; needs `zfs-zed` package):

```sh
sudo install -m 0755 examples/asustor-bay-led                      /usr/local/sbin/
sudo install -m 0755 examples/zfs-error-leds/asustor-zfs-sync-leds /usr/local/sbin/
sudo install -m 0755 examples/zfs-error-leds/statechange-led.sh    /etc/zfs/zed.d/
sudo install -m 0644 examples/zfs-error-leds/asustor-zfs-leds.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now asustor-zfs-leds.service
sudo systemctl restart zed
```

Verify the helper before relying on the rest:

```sh
# Should turn the bay-1 red LED on, then blink, then off.
sudo /usr/local/sbin/asustor-bay-led 1 on   ; sleep 1
sudo /usr/local/sbin/asustor-bay-led 1 blink; sleep 3
sudo /usr/local/sbin/asustor-bay-led 1 off
```

Force a sync at any time (e.g. after `zpool clear`):

```sh
sudo systemctl reload asustor-zfs-leds.service
# or just:
sudo /usr/local/sbin/asustor-zfs-sync-leds
```

The four MCU LEDs combine to produce mixed colours: power blue + orange =
purple, status green + red = yellow.

```sh
# Solid blue power LED, status off
echo 1 | sudo tee /sys/class/leds/asustor:blue:power/brightness
echo 0 | sudo tee /sys/class/leds/asustor:green:status/brightness
echo 0 | sudo tee /sys/class/leds/asustor:red:status/brightness

# Slow-blink status red (driver maps the requested period to one of four
# hardware-supported speeds: 500/250/125/63 ms half-period)
echo timer | sudo tee /sys/class/leds/asustor:red:status/trigger
echo 500 | sudo tee /sys/class/leds/asustor:red:status/delay_on
echo 500 | sudo tee /sys/class/leds/asustor:red:status/delay_off
```

#### MCU LED hardware quirks

Behaviour observed on AS6806T hardware (not driver bugs — the MCU
acknowledges every blink command):

- The **blue power** and **red status** LEDs have a slow turn-off
  response. Blinks at 1 Hz (500 ms half-period) are clearly visible, but
  at 2 Hz and faster they smear into a continuous glow.
- The **orange power** LED rarely produces a visible blink. It shares
  the bicolor power element with blue, and the MCU appears to suppress
  blinking on this colour. Treat it as on/off only.
- The **green status** LED has a sharp response and blinks crisply at
  every supported speed.

### USB backup button

Reported as `KEY_COPY` from an input device named `asustor-keys`:

```sh
# Identify the device
grep -l asustor-keys /sys/class/input/input*/name

# Watch presses (use the matching /dev/input/eventN)
sudo evtest /dev/input/eventN
```

### Front-panel LCD (16×2)

Three sysfs attributes under `/sys/devices/platform/asustor_lcm/`:

| Attribute    | Mode | Description                                         |
| ------------ | ---- | --------------------------------------------------- |
| `lcd_line0`  | RW   | Top row text (up to 16 chars, padded with spaces)   |
| `lcd_line1`  | RW   | Bottom row text (up to 16 chars, padded with spaces)|
| `lcd_clear`  | W    | Write any value to blank both rows                  |

```sh
LCD=/sys/devices/platform/asustor_lcm

echo "Lockerstor Gen3" | sudo tee $LCD/lcd_line0
echo "uptime: $(uptime -p)" | sudo tee $LCD/lcd_line1

# Read back what was last written
cat $LCD/lcd_line0

# Blank the display
echo 1 | sudo tee $LCD/lcd_clear
```

The display only renders ASCII; non-ASCII bytes are passed through to the
controller as-is. The backlight is on a separate, always-on supply and is
not controlled by this driver.

### LCD navigation buttons

The four buttons around the LCD are exposed as a Linux input device
(`ASUSTOR LCD Buttons`). Each press emits a single key event:

| Physical position | Key code    |
| ----------------- | ----------- |
| Top-left          | `KEY_UP`    |
| Bottom-left       | `KEY_DOWN`  |
| Top-right         | `KEY_ESC`   |
| Bottom-right      | `KEY_ENTER` |

```sh
grep -l 'ASUSTOR LCD Buttons' /sys/class/input/input*/name
sudo evtest /dev/input/eventN
```

## Debugging

Force model detection (useful if PCI auto-detection fails — e.g. on
AS6804T or AS6808T which have no recorded PCI signature yet):

```sh
sudo modprobe asustor_as68xx force_device=AS6806
```

Valid values: `AS6804`, `AS6806`, `AS6808`, `AS6810`.

The MCU and LCM init paths are non-fatal: if either serial port fails to
open, the driver still loads and exposes whatever did succeed. Check
`dmesg | grep asustor_as68xx` for per-subsystem status.

## License

GPL-2.0. See [LICENSE](LICENSE).
