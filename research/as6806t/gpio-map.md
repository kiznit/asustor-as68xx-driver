# AS6806T GPIO map

GPIO chip: **`AMDI0030:00`** (`pinctrl-amd`), 256 lines total.
Source: `gpioinfo` output in [as6806t-info.txt](../../research/as6806t/as6806t-info.txt)
and pin assignments in [asustor_as68xx.c](../../asustor_as68xx.c).

## Output pins (29 total)

| Pin | Dir    | Function                | Polarity      | Notes                           |
| --- | ------ | ----------------------- | ------------- | ------------------------------- |
|   4 | out    | LCD controller enable   | ACTIVE_HIGH   | Turns the LCD text on/off; backlight is separate |
|   6 | out    | *unused*                | ?             | AS6810T disk LED candidate      |
|   9 | out    | *unused*                | ?             | AS6810T disk LED candidate      |
|  10 | out    | *unused*                | ?             | AS6810T disk LED candidate      |
|  12 | out    | *unused*                | ?             | AS6810T disk LED candidate      |
|  23 | out    | *unused*                | ?             | AS6810T disk LED candidate      |
|  26 | out    | **power backplane?**    | —             | **DO NOT TOUCH**                |
|  27 | out    | **power backplane?**    | —             | **DO NOT TOUCH**                |
|  32 | out    | *unused*                | ?             | AS6810T disk LED candidate      |
|  68 | out    | `sata1:red:disk`        | ACTIVE_LOW    |                                 |
|  69 | out    | `sata1:green:disk`      | ACTIVE_HIGH   |                                 |
|  74 | out    | `sata2:red:disk`        | ACTIVE_LOW    |                                 |
|  75 | out    | `sata2:green:disk`      | ACTIVE_HIGH   |                                 |
|  78 | out    | `sata3:red:disk`        | ACTIVE_LOW    |                                 |
|  79 | out    | `sata3:green:disk`      | ACTIVE_HIGH   |                                 |
|  80 | out    | *unused*                | ?             | AS6810T disk LED candidate      |
|  91 | out    | *unused*                | ?             | AS6810T disk LED candidate      |
| 106 | out    | `sata4:green:disk`      | ACTIVE_HIGH   |                                 |
| 107 | out    | `sata4:red:disk`        | ACTIVE_LOW    |                                 |
| 144 | out    | *unused*                | ?             | AS6810T disk LED candidate      |
| 145 | out    | `sata5:red:disk`        | ACTIVE_HIGH   | (green/red polarity swapped vs 1–4) |
| 146 | out    | `sata5:green:disk`      | ACTIVE_LOW    |                                 |
| 147 | out    | `asustor:green:usbcopy` | ACTIVE_HIGH   | USB backup button LED           |
| 148 | out    | `asustor:blue:lan`      | ACTIVE_LOW    | Software-controlled LAN LED     |
| 154 | out    | `sata6:green:disk`      | ACTIVE_HIGH   |                                 |
| 156 | out    | `sata6:red:disk`        | ACTIVE_LOW    |                                 |
| 157 | out    | *unused*                | ?             | AS6810T disk LED candidate      |

## Input pins with ACPI:Event consumers

These are routed to ACPI for system events (power button, lid, etc.) — do not
touch from Linux userspace.

| Pin | Notes                          |
| --- | ------------------------------ |
|   0 | debounce 50 ms                 |
|   2 |                                |
|  17 |                                |
|  18 |                                |
|  24 |                                |
|  54 |                                |
|  58 |                                |
|  59 |                                |
|  61 |                                |
|  62 |                                |

## Other input pins used by this driver

| Pin | Function      | Polarity    | Notes                                  |
| --- | ------------- | ----------- | -------------------------------------- |
|  89 | backup button | ACTIVE_LOW  | `gpio-keys-polled`, reports `KEY_COPY` |

## Summary of unmapped output pins

`6, 9, 10, 12, 23, 32, 80, 91, 144, 157` — 10 unassigned output-capable pins,
exactly matching the 4 additional bays needed for the AS6810T (×2 for
green/red pairs) plus 2 spares. Polarity unknown until probed on hardware.
See [research/probing-disk-leds.md](../probing-disk-leds.md) for a
contributor-facing `libgpiod` probing guide, or
[.github/prompts/probe-gpio.prompt.md](../../.github/prompts/probe-gpio.prompt.md)
for the Copilot prompt that generates a probe script.
