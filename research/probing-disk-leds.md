# Probing disk LED GPIOs on AS6808T / AS6810T

If you own an **AS6808T** (8 bays) or **AS6810T** (10 bays) and want to help
finish driver support, the only missing piece is the per-bay disk LED GPIO
mapping for the extra bays:

- AS6808T: bays **7–8** (4 LEDs: green + red per bay)
- AS6810T: bays **7–10** (8 LEDs: green + red per bay)

The board is identical to the AS6806T, so all candidate pins are already
known — they just need to be toggled one at a time on real hardware while
someone watches the front of the chassis.

This guide walks through doing that safely with `libgpiod`.

---

## ⚠️ Read this first — pins you must NOT touch

The AMD FCH GPIO controller on this board exposes **256 lines**. A few of
them are wired to things that will ruin your day:

| Pin     | What it does                                                 |
| ------- | ------------------------------------------------------------ |
| **26**  | **Power-backplane rail** — DO NOT TOUCH                      |
| **27**  | **Power-backplane rail** — DO NOT TOUCH                      |

We have not fully disambiguated 26 vs 27. One of them kills every
front-panel LED + the LCD (master LED power). The other **cuts HDD
backplane power for bays 4–6** with an audible relay click — i.e. it can
yank power out from under running drives. Treat both as off-limits.

The candidate pin list below has 26 and 27 already excluded. Don't go
exploring outside it.

---

## Candidate pins

From `gpioinfo` on a known-good AS6806T, these output-capable lines are
**unused** by firmware/ACPI and are the candidates for the missing bay LEDs:

```
6, 9, 10, 12, 23, 32, 80, 91, 144, 157
```

That's exactly 10 pins, which matches what the AS6810T needs (8 LEDs for
bays 7–10, plus 2 spares). For the AS6808T only 4 of these will be in use.

Polarity (`ACTIVE_HIGH` vs `ACTIVE_LOW`) is also unknown and will be
discovered while probing.

For full context on already-mapped pins, see
[research/as6806t/gpio-map.md](as6806t/gpio-map.md).

---

## Prerequisites

You need root and `libgpiod` userspace tools on the NAS:

### Debian / Ubuntu / TrueNAS SCALE

```sh
sudo apt update
sudo apt install gpiod
```

### ADM (stock ASUSTOR firmware)

The stock OS does not ship `libgpiod`. Easiest path is to boot a Linux
live USB or install Debian/TrueNAS to a spare drive for probing — then put
the original boot drive back when you're done.

### Verify the GPIO chip is visible

```sh
sudo gpiodetect
```

You should see a line like:

```
gpiochip0 [AMDI0030:00] (256 lines)
```

If `AMDI0030:00` shows up, you're on the right hardware. If not, stop —
this guide doesn't apply to your machine.

The driver does **not** need to be loaded for probing. In fact it's
easier with the driver unloaded so it doesn't fight you for the pins:

```sh
sudo rmmod asustor_as68xx 2>/dev/null
```

---

## Check which pins are free

```sh
sudo gpioinfo -c gpiochip0 6 9 10 12 23 32 80 91 144 157
```

For each candidate pin, the line should show `output` with **no
`consumer=...`**. If it shows `consumer=sata*:disk` or similar, the driver
is loaded and has already claimed it — `rmmod asustor_as68xx` first.
Skip any pin held by something else (e.g. `ACPI:Event`).

> **libgpiod v1 fallback:** `gpioinfo gpiochip0` (positional chip arg) and
> grep manually.

---

## Toggling a single pin

`gpioset` drives a line and **holds it until the process exits** — by
default it never exits on its own. The chip is selected with `-c`. To hold
the line for a fixed observation window, wrap the call in `timeout`:

```sh
# Set pin 80 HIGH for 3 seconds, then release
sudo timeout 3 gpioset -c gpiochip0 80=1
```

You can also use the built-in `-t` (toggle) option with a trailing `0`
period to make `gpioset` exit on its own:

```sh
# Hold HIGH for 3 s, then toggle to LOW and exit
sudo gpioset -c gpiochip0 -t 3s,0 80=1
```

(`gpioset --help` documents `-t period[,period]...` — when the last period
is `0`, the program exits.)

> **libgpiod v1 (older distros).** v1 has no `-c` and uses
> `--mode=time --sec=N`. Quick check: `gpioset --version`. If you don't
> see `-c` in `gpioset --help`, you're on v1; substitute
> `gpioset --mode=time --sec=3 gpiochip0 80=1` for the v2 forms above.

Watch the front of the NAS while the command runs. Note which LED (if any)
lit up or went dark, on which bay, and what colour.

Then try LOW:

```sh
sudo timeout 3 gpioset -c gpiochip0 80=0
```

The combination tells you both **which LED** the pin drives and its
**polarity**:

- LED **on at HIGH, off at LOW**  →  `ACTIVE_HIGH`
- LED **on at LOW, off at HIGH**  →  `ACTIVE_LOW`

> **Note:** by convention on this board, green disk LEDs (activity) are
> usually `ACTIVE_HIGH` and red disk LEDs (fault) are usually `ACTIVE_LOW`,
> but bay 5 has them swapped, so don't assume — verify each one.

---

## Suggested probe loop

Work through one pin at a time. Keep a log as you go. A useful pattern
in a script (libgpiod v2):

```sh
PINS="6 9 10 12 23 32 80 91 144 157"
for p in $PINS; do
    echo "=== GPIO $p ==="
    echo "Setting HIGH for 3s — watch the chassis"
    sudo timeout 3 gpioset -c gpiochip0 $p=1
    read -p "What changed (e.g. 'bay7 green ON')? " note_high
    echo "Setting LOW for 3s — watch the chassis"
    sudo timeout 3 gpioset -c gpiochip0 $p=0
    read -p "What changed (e.g. 'bay7 green OFF')? " note_low
    echo "$p HIGH=$note_high LOW=$note_low" >> probe-results.txt
    echo
done
```

You can also just run the `gpioset` commands manually one at a time — that's
arguably safer and lets you take breaks.

---

## Filling in the results table

Capture results in this format and attach them to a GitHub issue / PR:

| GPIO | Bay | Colour | Polarity      | Notes                           |
| ---- | --- | ------ | ------------- | ------------------------------- |
|   6  |  ?  |   ?    | ACTIVE_?      |                                 |
|   9  |  ?  |   ?    | ACTIVE_?      |                                 |
|  10  |  ?  |   ?    | ACTIVE_?      |                                 |
|  12  |  ?  |   ?    | ACTIVE_?      |                                 |
|  23  |  ?  |   ?    | ACTIVE_?      |                                 |
|  32  |  ?  |   ?    | ACTIVE_?      |                                 |
|  80  |  ?  |   ?    | ACTIVE_?      |                                 |
|  91  |  ?  |   ?    | ACTIVE_?      |                                 |
| 144  |  ?  |   ?    | ACTIVE_?      |                                 |
| 157  |  ?  |   ?    | ACTIVE_?      |                                 |

Pins that don't drive anything visible can be left as "unused".

---

## Translating results into the driver

Once the table is filled in, the changes needed in
[`asustor_gpio.c`](../asustor_gpio.c) are mechanical. Each disk LED is one
entry in a `gpiod_lookup_table` of the form:

```c
GPIO_LOOKUP_IDX("AMDI0030:00", <pin>, NULL, <led_index>, <polarity>),
```

- `<pin>` — the GPIO line number you mapped
- `<led_index>` — index into the `asustor_as68xx_leds[]` array for that
  LED (e.g. `sata7:green:disk`, `sata7:red:disk`, …)
- `<polarity>` — `GPIO_ACTIVE_HIGH` or `GPIO_ACTIVE_LOW`

For an AS6810T entry, also add an `asustor_as68xx_driver_data` row with
`.name = "AS6810"` and `.num_bays = 10`, and add `AS6810` to
`VALID_OVERRIDE_NAMES` in the `force_device` `MODULE_PARM_DESC`.

See the existing AS6806T entries in [`asustor_gpio.c`](../asustor_gpio.c)
for the exact pattern.

---

## Verifying afterwards

Once the new lookup table is in place, build and load the driver, then:

```sh
ls /sys/class/leds/ | grep '^sata'
# expect sata1..sata8 or sata1..sata10 depending on model

# Light each disk LED briefly, e.g. bay 8 red
echo 1 | sudo tee /sys/class/leds/sata8:red:disk/brightness
sleep 1
echo 0 | sudo tee /sys/class/leds/sata8:red:disk/brightness
```

If every bay lights the correct LED in the correct colour, you're done.
Open a PR with the GPIO map table and the `asustor_gpio.c` diff — and
mention which model you tested on (AS6808T vs AS6810T).
