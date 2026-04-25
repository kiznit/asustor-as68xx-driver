// SPDX-License-Identifier: GPL-2.0
/*
 * asustor_as68xx_main.c - platform driver for the ASUSTOR Lockerstor Gen3
 *                         (AS68xxT) series of NAS devices.
 *
 * Supported models: AS6804T, AS6806T (verified), AS6808T, AS6810T.
 * All four variants share the AMD Rembrandt/Fox mainboard; the trailing
 * digit indicates the number of HDD bays (4/6/8/10). Only the GPIO pin
 * map for bays 1-6 is known; disk LEDs for bays 7-10 are registered only
 * once their GPIO pins have been probed on real hardware.
 *
 * The AS68xxT series is built on an AMD Rembrandt SoC with the following
 * board-specific quirks:
 *   - GPIO is provided by the AMD FCH (pinctrl-amd / AMDI0030:00).
 *   - Disk activity/error LEDs, the LCD controller enable line, and the
 *     USB backup button are wired to that GPIO controller -- see
 *     asustor_gpio.c.
 *   - Power, status and LAN LEDs, as well as fan control, are driven by
 *     an onboard MCU (AS72XXR_MCU) over /dev/ttyS1 at 115200 baud -- see
 *     asustor_mcu.c.
 *   - The front-panel 16x2 LCD module and its four navigation buttons
 *     use a framed serial protocol on /dev/ttyS2 at 9600 baud -- see
 *     asustor_lcm.c.
 *   - Temperature sensors come from stock kernel drivers (nct7802,
 *     k10temp, spd5118, nvme).
 *
 * Copyright (C) 2026 Thierry Tremblay
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/dmi.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>

#include "asustor_gpio.h"
#include "asustor_lcm.h"
#include "asustor_mcu.h"

struct asustor_as68xx_driver_data {
	const char *name;
	int num_bays;
};

/*
 * The four AS68xxT variants share the same mainboard, MCU, LCD, backup
 * button and non-disk LED wiring. They differ only in bay count and in
 * which add-on SATA/PCIe controllers are fitted.
 */
static struct asustor_as68xx_driver_data asustor_as6804_driver_data = {
	.name     = "AS6804",
	.num_bays = 4,
};

static struct asustor_as68xx_driver_data asustor_as6806_driver_data = {
	.name     = "AS6806",
	.num_bays = 6,
};

static struct asustor_as68xx_driver_data asustor_as6808_driver_data = {
	.name     = "AS6808",
	.num_bays = 8,
};

static struct asustor_as68xx_driver_data asustor_as6810_driver_data = {
	.name     = "AS6810",
	.num_bays = 10,
};

static struct asustor_as68xx_driver_data * const asustor_as68xx_variants[] = {
	&asustor_as6804_driver_data,
	&asustor_as6806_driver_data,
	&asustor_as6808_driver_data,
	&asustor_as6810_driver_data,
};

/*
 * All AS68xxT boards expose the same DMI vendor/product ("AMD" / "Rembrandt"),
 * so DMI alone cannot distinguish variants. Matching here only confirms
 * that this is an AS68xxT; the actual variant is chosen at runtime by
 * asustor_as68xx_detect_variant() (PCI-based) or via force_device=.
 */
static const struct dmi_system_id asustor_as68xx_systems[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "AMD"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "Rembrandt"),
		},
		/* driver_data filled in at runtime */
	},
	{}
};
MODULE_DEVICE_TABLE(dmi, asustor_as68xx_systems);

#define PCI_VENDOR_ID_ASMEDIA	0x1b21
#define PCI_DEVICE_ID_ASM1165	0x1165
#define PCI_DEVICE_ID_ASM1166	0x1166

static unsigned int asustor_as68xx_count_pci(u16 vendor, u16 device)
{
	struct pci_dev *pdev = NULL;
	unsigned int count = 0;

	while ((pdev = pci_get_device(vendor, device, pdev)) != NULL)
		count++;
	return count;
}

/*
 * Pick the AS68xxT variant by counting add-on ASMedia SATA controllers.
 *   - 1x ASM1166           -> AS6806T (verified on hardware)
 *   - 2x ASM1165           -> AS6810T (verified from hardware dmesg)
 *   - anything else        -> warn and fall back to AS6806T (safe: only
 *                             the known-good bay 1-6 LEDs are registered).
 *
 * AS6804T and AS6808T have no PCI signature recorded yet; users on those
 * boxes must pass force_device=.
 */
static struct asustor_as68xx_driver_data *asustor_as68xx_detect_variant(void)
{
	unsigned int n_1166 = asustor_as68xx_count_pci(PCI_VENDOR_ID_ASMEDIA,
						       PCI_DEVICE_ID_ASM1166);
	unsigned int n_1165 = asustor_as68xx_count_pci(PCI_VENDOR_ID_ASMEDIA,
						       PCI_DEVICE_ID_ASM1165);

	if (n_1166 == 1 && n_1165 == 0)
		return &asustor_as6806_driver_data;
	if (n_1165 == 2 && n_1166 == 0)
		return &asustor_as6810_driver_data;

	pr_warn("no AS68xxT PCI signature matched (ASM1165=%u ASM1166=%u); defaulting to AS6806. Use force_device=AS6804|AS6806|AS6808|AS6810 to override.\n",
		n_1165, n_1166);
	return &asustor_as6806_driver_data;
}

static struct asustor_as68xx_driver_data *driver_data;

static char *force_device = "";
module_param(force_device, charp, 0444);
MODULE_PARM_DESC(force_device,
		 "Bypass DMI/PCI detection and force a specific AS68xxT model. Valid values: AS6804, AS6806, AS6808, AS6810");

static int __init asustor_as68xx_init(void)
{
	const struct dmi_system_id *system;
	int ret, i;

	driver_data = NULL;

	if (force_device && *force_device) {
		for (i = 0; i < ARRAY_SIZE(asustor_as68xx_variants); i++) {
			struct asustor_as68xx_driver_data *dd =
				asustor_as68xx_variants[i];
			if (dd && dd->name &&
			    strcmp(force_device, dd->name) == 0) {
				driver_data = dd;
				break;
			}
		}
		if (!driver_data) {
			pr_err("force_device parameter set to invalid value \"%s\"!\n",
			       force_device);
			return -EINVAL;
		}
		pr_info("force_device parameter set to \"%s\"\n", force_device);
	} else {
		system = dmi_first_match(asustor_as68xx_systems);
		if (!system) {
			pr_info("No supported ASUSTOR AS68xxT mainboard found\n");
			return -ENODEV;
		}
		driver_data = asustor_as68xx_detect_variant();
		pr_info("Found %s (%s/%s, %d-bay)\n", driver_data->name,
			system->matches[0].substr, system->matches[1].substr,
			driver_data->num_bays);
	}

	ret = asustor_gpio_init(driver_data->num_bays);
	if (ret)
		return ret;

	/* MCU: fan control, power/status/LAN LEDs */
	ret = asustor_mcu_init();
	if (ret)
		pr_warn("MCU init failed: %d (non-fatal)\n", ret);

	/* Front-panel LCD (ttyS2, framed 0xF0 protocol) */
	ret = asustor_lcm_init();
	if (ret)
		pr_warn("LCM init failed: %d (non-fatal)\n", ret);

	return 0;
}

static void __exit asustor_as68xx_cleanup(void)
{
	asustor_lcm_cleanup();
	asustor_mcu_cleanup();
	asustor_gpio_cleanup();
}

module_init(asustor_as68xx_init);
module_exit(asustor_as68xx_cleanup);

MODULE_AUTHOR("Thierry Tremblay");
MODULE_DESCRIPTION("Platform driver for ASUSTOR Lockerstor Gen3 (AS68xxT) NAS hardware");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:asustor_as68xx");
MODULE_SOFTDEP("pre: pinctrl-amd"
	       " leds-gpio"
	       " gpio-keys-polled"
	       " ledtrig-default-on"
	       " ledtrig-timer"
	       " ledtrig-disk"
	       " ledtrig-panic");
