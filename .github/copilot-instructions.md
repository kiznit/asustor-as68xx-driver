# ASUSTOR AS68xxT Platform Driver

Linux platform driver for the **ASUSTOR Lockerstor Gen3 (AS68xxT)** NAS series
(AS6804T, AS6806T, AS6808T, AS6810T). Builds a single kernel module:
`asustor_as68xx.ko`. All four variants share the same Rembrandt/Fox
mainboard; the trailing digit is the bay count (4/6/8/10).

Earlier ASUSTOR generations (Intel/IT87-based) are **out of scope**.

## Source layout

| File                     | Purpose                                                               |
| ------------------------ | --------------------------------------------------------------------- |
| `asustor_as68xx_main.c`  | Platform entry point, DMI detection, PCI variant detection           |
| `asustor_gpio.c/.h`      | GPIO LEDs (disk/backup), USB-copy button, LCD enable lookup           |
| `asustor_mcu.c/.h`       | MCU serial protocol (fans, power/status LEDs) on `/dev/ttyS1`         |
| `asustor_lcm.c/.h`       | Front-panel LCD + buttons, framed protocol on `/dev/ttyS2`            |
| `Makefile`               | Builds `asustor_as68xx.ko`                                            |
| `dkms_as68xx.conf`       | DKMS package definition                                               |
| `info/`                  | Hardware dumps (DMI, SSDT, lspci, hwmon, gpioinfo) for AS68xxT boards |

All four `.o` files link into the single `asustor_as68xx.ko` module.

## Hardware summary (AS68xxT / AMD Rembrandt)

- **CPU:** AMD Ryzen Embedded V3000 series ("Rembrandt"), board codename `Fox`
  (DMI: `AMD` / `Rembrandt`).
- **GPIO:** AMD FCH via `pinctrl-amd` (`AMDI0030:00`, 256 lines).
- **Fan control:** MCU over `/dev/ttyS1` (115200 8N1). Tachometer works
  (first reading after a PWM change may be bogus).
- **Front LCD:** framed protocol on `/dev/ttyS2` (9600 8N1). LCD controller
  enable is GPIO pin 4, acquired directly by `asustor_lcm` via `gpiod_get()`
  (con_id `"enable"`). Gates the controller logic only; the backlight has a
  separate supply.
- **Temperatures:** handled by stock upstream drivers
  (`k10temp`, `spd5118`, `nvme`, `amd-xgbe`). This driver does not
  register any hwmon temperature sensor.
- **Network:** 2× RTL8126A (`r8169`) + 2× AMD XGMAC 10GbE (`amd-xgbe`).
  The 10GbE needs out-of-tree AMD patches — unrelated to this driver.

### Dangerous pins / commands — DO NOT USE

- GPIO pins 26 and 27 = power-backplane rails. One kills all LEDs + LCD
  (master LED power), the other cuts HDD backplane power for bays 4–6
  (audible relay click). The two have not been disambiguated — treat both
  as "do not touch". See [research/as6806t/gpio-map.md](../research/as6806t/gpio-map.md).
- MCU LED type 4 (`0x10 0x02 0x30/0x33`) = HDD power relay. Not an LED.
- MCU `0x10 0x01 ...` = power state. May shut the machine down.

## Adding a new AS68xxT variant

The family currently has one supported SKU (AS6806T) and one planned (AS6810T).
If another AS68xxT variant appears:

1. Add a `gpiod_lookup_table` in `asustor_gpio.c` for its LED pin map.
2. Add an `asustor_as68xx_driver_data` entry in `asustor_as68xx_main.c` with `.name` and `.num_bays`.
3. If DMI alone is insufficient, add PCI-based differentiation (e.g. count of
   ASM116x controllers).
4. Update `VALID_OVERRIDE_NAMES` in the `force_device` `MODULE_PARM_DESC`.
   Currently accepted values: `AS6804`, `AS6806`, `AS6808`, `AS6810`.

### AS6810T notes

Analysis in `info/asustor_AS6810T.txt` confirms the AS6810T uses the **same
Rembrandt/Fox board** as the AS6806T. Identical: DMI strings, GPIO chip, all
GPIO output pins, serial ports, MCU, LCD, backup button, non-disk LEDs, 4× NVMe M.2 slots.

Differences:
- SATA: 2× ASM1165 `[1b21:1165]` instead of 1× ASM1166 `[1b21:1166]`
  → usable for PCI-based differentiation.

**Blocked on hardware:** the disk LED GPIO pins for bays 7–10 cannot be
determined without toggling pins on a real AS6810T. Candidate pins from the
unused output set: `6, 9, 10, 12, 23, 32, 80, 91, 144, 157`. Their polarity
is also unknown.

## MCU Serial Protocol (`/dev/ttyS1`, 115200 8N1)

### Fan control

| Command                      | Bytes                | Response               |
| ---------------------------- | -------------------- | ---------------------- |
| Set fan PWM                  | `0x30 0x00 <duty>`   | ack                    |
| Get fan PWM                  | `0x31 0x00 0x00`     | byte[5] = duty         |
| Get fan RPM (low byte half)  | `0x31 0x10 0x00`     | bytes[4:5] = RPM hi:lo |
| Get fan RPM (high byte half) | `0x31 0x11 0x00`     | bytes[4:5] = RPM hi:lo |

### LED control — `0x10 0x02 <value>`

Bicolor LEDs (power = blue+red, status = green+red). Each colour component is
a separate "type". Mode offset is added to the OFF base.

| Type | LED          | OFF  | ON   | BLINK slow | BLINK med | BLINK fast | BLINK fastest |
| ---- | ------------ | ---- | ---- | ---------- | --------- | ---------- | ------------- |
| 0    | Power blue   | 0x98 | 0x9b | 0x9c       | 0x9d      | 0x9e       | 0x9f          |
| 1    | Status red   | 0x78 | 0x7b | 0x7c       | 0x7d      | 0x7e       | 0x7f          |
| 2    | Power red    | 0x68 | 0x6b | 0x6c       | 0x6d      | 0x6e       | 0x6f          |
| 3    | Status green | 0x70 | 0x73 | 0x74       | 0x75      | 0x76       | 0x77          |

Combine types for mixed colours (status yellow = green + red ON; power purple
= blue + orange ON).

### Other queries

| Command            | Bytes            | Response                              |
| ------------------ | ---------------- | ------------------------------------- |
| MCU version        | `0x41 0x00 0x00` | bytes[4:5] = major.minor              |
| Settings bitmask   | `0x13 0x00 0x00` | byte[5] (EUP/WOL/RTC/PowerResume)     |
| Power button state | `0x11 0x05 0x00` | byte[5] ≠ 0 = pressed                 |
| Reset button state | `0x11 0x05 0x01` | byte[5] ≠ 0 = pressed                 |

## LCM Serial Protocol (`/dev/ttyS2`, 9600 8N1)

Front-panel 16×2 character LCD. Framed protocol:

```
[0xF0] [N] [CMD] [DATA_0 ... DATA_{N-1}] [CHECKSUM]
```

`CHECKSUM = sum(packet[0 : N+3]) & 0xFF`. Max packet size 22 bytes.
The LCD ACKs with `0xF1 0x01 <cmd> <status> <checksum>`.

| CMD    | N  | Description                                                 |
| ------ | -- | ----------------------------------------------------------- |
| `0x11` | 1  | LCD power on/off (0 = off, 1 = on)                          |
| `0x12` | 1  | LCD display/backlight control                               |
| `0x22` | 1  | LCD clear/reset (mode always 0)                             |
| `0x27` | 18 | `<line> <col> <16 ASCII chars>` — write a line              |

### Button events (unsolicited)

When a front-panel button is pressed, the LCD controller sends
`0xF0 0x01 0x80 <btn_id> <checksum>`. The driver reads these on a kthread and
reports them via a Linux input device. Button IDs:

- 1 = Up (top-left)
- 2 = Down (bottom-left)
- 3 = Back/ESC (top-right)
- 4 = Enter (bottom-right)

The host must ACK with `0xF1 0x01 0x80 0x00 <checksum>`.

### Init sequence

1. `asustor_as68xx` binds GPIO 4 (con_id `"enable"`) to the `asustor_lcm`
   platform device via a `gpiod_lookup_table`.
2. `asustor_lcm_init()` calls `gpiod_get(..., "enable", GPIOD_OUT_HIGH)` to
   enable the LCD controller.
3. `F0 01 11 01 03` (power on), sleep 15 ms.
4. `F0 01 22 00 13` (clear).

## Build & test

The agent **cannot build locally** and is **not on the NAS**. Do not run
`make`, `modprobe`, or `insmod` during development — all verification must be
done via static reads and greps.

For the user on hardware:

```sh
make                                             # build asustor_as68xx.ko
make install                                     # install to /lib/modules/
make dkms                                        # DKMS install
insmod asustor_as68xx.ko force_device=AS6806     # bypass DMI detection
```

## Code style

- Follow existing patterns in `asustor_gpio.c` — use `clang-format off/on`
  around lookup tables.
- GPIO lookup tables use
  `GPIO_LOOKUP_IDX(chip_label, pin, NULL, led_index, polarity)`.
- LED indices correspond to positions in the `asustor_as68xx_leds[]` array.
- Use `GPIO_ACTIVE_LOW` / `GPIO_ACTIVE_HIGH` to match hardware polarity.
- Kernel compat: pre-6.7 uses `gpiochip_find`, 6.7+ uses
  `gpio_device_find_by_label` — the existing helper handles both.
