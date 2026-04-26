# asustor-status — front-panel status LED + LCD daemon

A single Bash daemon + systemd unit that drives the AS68xxT
front-panel **status LED** and **16x2 LCD** with meaningful state
across the system lifecycle:

| Phase    | Status LED                | LCD                                     |
|----------|---------------------------|------------------------------------------|
| Booting  | green blink               | `Booting...` + currently-running unit    |
| Ready    | green solid               | rotates: pool / HDD+CPU temps / fan RPM  |
| Warning  | yellow (green + red solid)| `... WARN` + offending value             |
| Error    | red blink                 | `... CRIT` / `FAN FAILURE` / `ZFS FAULTED` |
| Shutdown | green blink               | `Shutting down` + currently-stopping unit|
| Panic    | red solid (kernel)        | (last LCD frame; the driver leaves it)   |

The kernel `panic` trigger on `asustor:red:status` is preserved
whenever red isn't actively used, so a real kernel panic still
lights the LED red even if the daemon is dead.

The MCU persists LED state across power-off, so leaving green-blink
at shutdown means the next power-on already shows the correct
"booting" state from t=0 — before the driver even loads. No
misleading red flash from a stale shutdown state.

## Status conditions monitored

- **ZFS pool health** — worst state across all imported pools.
  `DEGRADED` -> warn; `FAULTED`/`UNAVAIL`/`REMOVED`/`OFFLINE` -> error.
- **HDD temperature** — max across all SATA disks reported by the
  kernel `drivetemp` hwmon driver. Standby drives return -EAGAIN
  and are skipped without being woken, so HDD spindown is
  preserved. Default thresholds: 60 / 65 deg C.
- **CPU temperature** — `k10temp` `temp1_input`. Default: 80 / 90.
- **Fan failure** — tach reads 0 RPM while PWM > 0 for ~30 s
  (10 samples).

For per-bay red error LEDs, install the separate
[`zfs-error-leds`](../zfs-error-leds/) helpers — they cover the
"which specific bay" question; this daemon only summarizes pool
health on the central status LED + LCD.

## Configuration

All settings have sensible defaults; override via the systemd
`EnvironmentFile=` (see below) or by editing the script.

| Variable                    | Default                  | Meaning                                        |
|-----------------------------|--------------------------|------------------------------------------------|
| `ASUSTOR_TEMP_HDD_WARN/CRIT`| `60 / 65`                | HDD temperature thresholds, deg C              |
| `ASUSTOR_TEMP_CPU_WARN/CRIT`| `80 / 90`                | CPU temperature thresholds, deg C              |
| `ASUSTOR_FAN_BAD_THRESHOLD` | `10`                     | samples of tach==0 + PWM>0 before "fan failure" |
| `ASUSTOR_POOL_LABEL`        | `tank`                   | label shown on the LCD pool screen             |

## Install

The daemon reads HDD temperatures from the kernel `drivetemp` hwmon
driver. Make sure it loads at boot:

```sh
echo drivetemp | sudo tee /etc/modules-load.d/drivetemp.conf
sudo modprobe drivetemp
```

Then install the daemon and unit:

```sh
sudo install -m 0755 asustor-status            /usr/local/sbin/
sudo install -m 0644 asustor-status.service    /etc/systemd/system/
# Optional: override defaults
# sudo install -m 0644 /dev/stdin /etc/default/asustor-status <<'EOF'
# ASUSTOR_TEMP_HDD_WARN=58
# EOF
sudo systemctl daemon-reload
sudo systemctl enable --now asustor-status.service
journalctl -u asustor-status -f
```

## Verify

```sh
# Ready state: green solid, red on panic-trigger, brightness 0.
cat /sys/class/leds/asustor:green:status/{trigger,brightness}
cat /sys/class/leds/asustor:red:status/{trigger,brightness}

# LCD content
cat /sys/devices/platform/asustor_lcm/lcd_line0
cat /sys/devices/platform/asustor_lcm/lcd_line1
```

To exercise the warn/error paths without real hardware faults,
temporarily lower a threshold via `/etc/default/asustor-status`
and `systemctl restart asustor-status`.

## Dependencies

- `systemctl`, `awk`, `bash`, `sed` (always present on systemd hosts)
- Kernel `drivetemp` hwmon driver (`CONFIG_SENSORS_DRIVETEMP`) for
  HDD temperature — enabled by default in mainline / Debian / PVE
  kernels; just needs `modprobe drivetemp`.
- `zfsutils-linux` for pool status (`zpool list`)
- The driver's `asustor_mcu` hwmon device for fan RPM/PWM (auto-detected)
- The driver's `k10temp` style CPU sensor (auto-detected; AMD-only)

If `zpool` isn't installed, the pool-state probe returns `ONLINE`
and the daemon skips the pool screen on rotation gracefully.
If `drivetemp` is unavailable, HDD-temperature monitoring is
disabled (max temp reads as 0) but everything else still works.

## Limitations

- No per-disk CPU-temperature awareness — assumes `k10temp temp1_input`
  is "the" CPU temperature. AS68xxT variants all use AMD Rembrandt/Fox
  so this is fine; for non-AMD ports adjust `find_hwmon`.
- HDD temp samples roughly every 3 s (one full ready-cycle). Quick
  spikes between samples won't be caught.
- During very long shutdowns (>20 s of unit-stopping) the LCD line
  freezes on the last unit it managed to read before SIGKILL.
- The four LCD navigation buttons (`KEY_UP/DOWN/ESC/ENTER`) are not
  used — wiring them up to e.g. cycle screens is a future enhancement.
