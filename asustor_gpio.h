// SPDX-License-Identifier: GPL-2.0
#ifndef ASUSTOR_GPIO_H
#define ASUSTOR_GPIO_H

/*
 * GPIO-backed hardware on the AS68xxT mainboard:
 *   - disk activity / error LEDs (bays 1-6)
 *   - backup-button LED and LAN LED
 *   - USB-copy/backup button (gpio-keys-polled)
 *   - LCD controller enable line (consumed by asustor_lcm)
 *
 * Power/status/LAN-bicolor LEDs and fan control are on the MCU
 * (see asustor_mcu.c). Front-panel LCD nav buttons are on the LCM
 * controller (see asustor_lcm.c).
 *
 * Disk LEDs are registered only for bays whose GPIO pins are known.
 * Bays 7-10 on AS6808T/AS6810T are not yet mapped.
 */
int asustor_gpio_init(unsigned int num_bays);
void asustor_gpio_cleanup(void);

#endif /* ASUSTOR_GPIO_H */
