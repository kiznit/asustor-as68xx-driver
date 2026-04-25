---
description: "Use when probing GPIO pins on an ASUSTOR AS68xxT NAS (AS6806T / AS6810T) to discover disk LED mappings for new bays. Generates a bash script to toggle output-capable pins on the AMD FCH pin controller one by one."
agent: "agent"
tools: [read, search, edit]
argument-hint: "List of candidate pin numbers to test on AMDI0030:00"
---
Generate a bash script to systematically probe GPIO pins on an ASUSTOR AS68xxT
(Lockerstor Gen3) NAS for LED discovery. The board uses the AMD FCH pin
controller, so the GPIO chip label is always `AMDI0030:00`.

Primary use case: finding the disk LED pins for bays 7–10 on the AS6810T.

The script should:

1. Accept a list of pin numbers to test on `AMDI0030:00`.
2. For each pin:
   - Use `gpioset` / `gpioget` from libgpiod (preferred) or `/sys/class/gpio/`.
   - Configure as output.
   - Toggle HIGH for 2 s (user observes), then LOW for 1 s.
   - Prompt the user to describe what changed (which LED, colour, on/off).
   - Log the result to a file.
3. At the end, output a summary table mapping pin numbers to observed LED
   functions.
4. Safety:
   - **Refuse** pins 26 and 27 unconditionally (both are power-backplane
     rails — one cuts HDD power for bays 4–6 with an audible relay click,
     the other kills all front-panel LEDs and the LCD).
   - Skip pins already claimed (check `gpioinfo` for `[used]`).
   - Restore the original direction/value on exit (trap SIGINT).

Reference `info/as6806t-info.txt` for the AS6806T `gpioinfo` dump. Candidate
unused output pins on the AS6810T (from `info/asustor_AS6810T.txt`) include:
`6, 9, 10, 12, 23, 32, 80, 91, 144, 157`.

Output format — a markdown table suitable for pasting into a PR description
or `info/` note:

```
| GPIO Pin | Observed LED        | Colour | Active Polarity | Notes |
| -------- | ------------------- | ------ | --------------- | ----- |
```

Once the mapping is confirmed, the results feed into a new
`gpiod_lookup_table` and `asustor_as68xx_driver_data` entry in
`asustor_as68xx.c` for the AS6810T.
