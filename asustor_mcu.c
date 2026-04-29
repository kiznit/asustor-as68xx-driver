// SPDX-License-Identifier: GPL-2.0
/*
 * asustor_mcu.c - MCU serial communication for ASUSTOR AS68xxT
 *                 (Lockerstor Gen3) NAS hardware.
 *
 * Communicates with the onboard AS72XXR MCU via /dev/ttyS1 at 115200 baud (8N1).
 * Provides:
 *   - hwmon interface for fan PWM control
 *   - LED class devices for power and status LEDs
 *
 * Copyright (C) 2026 Thierry Tremblay
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/workqueue.h>

#include "asustor_mcu.h"

/* MCU command bytes */
#define MCU_CMD_SET_FAN_PWM	0x30
#define MCU_CMD_GET_FAN		0x31
#define MCU_CMD_GET_FAN_RPM_LO	0x10	/* sub-cmd: RPM low byte */
#define MCU_CMD_GET_FAN_RPM_HI	0x11	/* sub-cmd: RPM high byte */
#define MCU_CMD_SET_LED		0x10
#define MCU_CMD_LED_SUBCMD_LIGHT 0x02

/* MCU LED base values (OFF byte) — add mode offset for other states */
#define MCU_LED_POWER_BLUE	0x98
#define MCU_LED_POWER_ORANGE	0x68
#define MCU_LED_STATUS_GREEN	0x70
#define MCU_LED_STATUS_RED	0x78

/* MCU LED mode offsets from base value */
#define MCU_LED_MODE_OFF		0x00
#define MCU_LED_MODE_ON			0x03
#define MCU_LED_MODE_BLINK_SLOW		0x04
#define MCU_LED_MODE_BLINK_MED		0x05
#define MCU_LED_MODE_BLINK_FAST		0x06
#define MCU_LED_MODE_BLINK_FASTEST	0x07

/* MCU communication parameters */
#define MCU_SERIAL_PORT		"/dev/ttyS1"
#define MCU_BAUD_RATE		B115200
#define MCU_RX_LEN		6	/* reply length for "get" commands */
#define MCU_ACK_LEN		3	/* reply length for "set" commands */
#define MCU_RESP_TIMEOUT_MS	20	/* deadline waiting for a reply */
#define MCU_RESP_POLL_US	1000	/* poll interval while waiting for bytes */
#define MCU_FAN_DEFAULT_PWM	0x55

struct asustor_mcu {
	struct mutex lock;	/* serializes MCU communication */
	struct file *tty_filp;
	struct platform_device *pdev;

	/* hwmon */
	struct device *hwmon_dev;
	u8 fan_pwm;		/* cached PWM value */

	struct work_struct init_work;	/* deferred fan PWM set/get + LED reg */
	unsigned int leds_registered;	/* number of LEDs successfully registered */
};

/*
 * Singleton — LED class callbacks do not carry per-device data, and LEDs are
 * registered with a NULL parent, so they reach the device state through this
 * pointer.
 */
static struct asustor_mcu *mcu_data;

/* ---- Low-level serial I/O ---- */

static struct file *mcu_serial_open(void)
{
	struct file *f;
	struct tty_struct *tty;
	struct ktermios termios;

	f = filp_open(MCU_SERIAL_PORT, O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
	if (IS_ERR(f)) {
		pr_err("failed to open %s: %ld\n", MCU_SERIAL_PORT, PTR_ERR(f));
		return f;
	}

	tty = ((struct tty_file_private *)f->private_data)->tty;
	if (!tty) {
		pr_err("failed to get tty struct\n");
		filp_close(f, NULL);
		return ERR_PTR(-ENODEV);
	}

	/* Configure 115200 8N1 raw */
	termios = tty->termios;
	termios.c_iflag = 0;
	termios.c_oflag = 0;
	termios.c_cflag = MCU_BAUD_RATE | CS8 | CREAD | CLOCAL;
	termios.c_lflag = 0;
	termios.c_cc[VMIN] = 0;
	termios.c_cc[VTIME] = 5;	/* 500ms timeout */
	tty_set_termios(tty, &termios);

	return f;
}

static void mcu_serial_close(struct file *f)
{
	if (!IS_ERR_OR_NULL(f))
		filp_close(f, NULL);
}

/*
 * Send a command and read the MCU's reply.
 *
 * "Get" commands (resp != NULL) reply with a full 6-byte frame.
 * "Set" commands (resp == NULL) reply with a short 3-byte ack.
 * The caller's resp pointer tells us which kind we sent, so we know
 * exactly how many bytes to consume -- no need to time-out short reads.
 *
 * The tty was opened O_NONBLOCK, so kernel_read() returns -EAGAIN when no
 * data is available. We poll on a ~1 ms tick. Typical round-trips are a
 * few ms (measured 1.4-2 ms for GETs on a Rembrandt board), well under
 * the timeout -- important for any tight loop hammering the MCU (e.g. a
 * software LED blink).
 */
static int mcu_send_cmd(struct asustor_mcu *mcu, const u8 *cmd, int cmd_len,
			u8 *resp, int resp_len)
{
	struct file *f = mcu->tty_filp;
	u8 ack[MCU_ACK_LEN];
	u8 *buf;
	int len;
	loff_t pos = 0;
	unsigned long deadline;
	int got = 0;
	int ret;

	if (IS_ERR_OR_NULL(f))
		return -ENODEV;

	if (resp && resp_len > 0) {
		buf = resp;
		len = resp_len;
		memset(resp, 0, resp_len);
	} else {
		buf = ack;
		len = MCU_ACK_LEN;
	}

	ret = kernel_write(f, cmd, cmd_len, &pos);
	if (ret < 0)
		return ret;
	if (ret != cmd_len)
		return -EIO;

	pos = 0;
	deadline = jiffies + msecs_to_jiffies(MCU_RESP_TIMEOUT_MS);
	while (got < len) {
		ret = kernel_read(f, buf + got, len - got, &pos);
		if (ret > 0) {
			got += ret;
			continue;
		}
		if (time_after(jiffies, deadline))
			break;
		usleep_range(MCU_RESP_POLL_US, 2 * MCU_RESP_POLL_US);
	}

	if (resp && resp_len > 0)
		return got;		/* caller validates length */

	return (got == MCU_ACK_LEN) ? 0 : -EIO;
}

static int mcu_cmd_locked(struct asustor_mcu *mcu, const u8 *cmd, int cmd_len,
			  u8 *resp, int resp_len)
{
	int ret;

	mutex_lock(&mcu->lock);
	ret = mcu_send_cmd(mcu, cmd, cmd_len, resp, resp_len);
	mutex_unlock(&mcu->lock);

	return ret;
}

/* ---- hwmon (fan control) ---- */

static int mcu_fan_get_pwm(struct asustor_mcu *mcu)
{
	u8 cmd[] = { MCU_CMD_GET_FAN, 0x00, 0x00 };
	u8 resp[MCU_RX_LEN];
	int ret;

	ret = mcu_cmd_locked(mcu, cmd, sizeof(cmd), resp, sizeof(resp));
	if (ret >= MCU_RX_LEN) {
		mcu->fan_pwm = resp[5];
		return resp[5];
	}
	return ret < 0 ? ret : -EIO;
}

static int mcu_fan_get_rpm(struct asustor_mcu *mcu)
{
	u8 cmd_lo[] = { MCU_CMD_GET_FAN, MCU_CMD_GET_FAN_RPM_LO, 0x00 };
	u8 cmd_hi[] = { MCU_CMD_GET_FAN, MCU_CMD_GET_FAN_RPM_HI, 0x00 };
	u8 resp[MCU_RX_LEN];
	int ret;
	u8 lo, hi;

	mutex_lock(&mcu->lock);

	ret = mcu_send_cmd(mcu, cmd_lo, sizeof(cmd_lo), resp, sizeof(resp));
	if (ret < MCU_RX_LEN) {
		mutex_unlock(&mcu->lock);
		return ret < 0 ? ret : -EIO;
	}
	lo = resp[5];

	ret = mcu_send_cmd(mcu, cmd_hi, sizeof(cmd_hi), resp, sizeof(resp));
	if (ret < MCU_RX_LEN) {
		mutex_unlock(&mcu->lock);
		return ret < 0 ? ret : -EIO;
	}
	hi = resp[5];

	mutex_unlock(&mcu->lock);

	return (hi << 8) | lo;
}

static int mcu_fan_set_pwm(struct asustor_mcu *mcu, u8 duty)
{
	u8 cmd[] = { MCU_CMD_SET_FAN_PWM, 0x00, duty };
	int ret;

	ret = mcu_cmd_locked(mcu, cmd, sizeof(cmd), NULL, 0);
	if (ret < 0)
		return ret;
	mcu->fan_pwm = duty;
	return 0;
}

static umode_t asustor_mcu_hwmon_is_visible(const void *data,
					    enum hwmon_sensor_types type,
					    u32 attr, int channel)
{
	if (type == hwmon_pwm) {
		if (attr == hwmon_pwm_input || attr == hwmon_pwm_enable)
			return 0644;
		return 0;
	}
	if (type == hwmon_fan) {
		if (attr == hwmon_fan_input)
			return 0444;
		return 0;
	}
	return 0;
}

static int asustor_mcu_hwmon_read(struct device *dev,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel, long *val)
{
	struct asustor_mcu *mcu = dev_get_drvdata(dev);

	if (type == hwmon_fan) {
		switch (attr) {
		case hwmon_fan_input:
			*val = mcu_fan_get_rpm(mcu);
			if (*val < 0)
				return *val;
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	}

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_pwm_input:
		*val = mcu_fan_get_pwm(mcu);
		if (*val < 0)
			return *val;
		return 0;
	case hwmon_pwm_enable:
		*val = 1;	/* always manual mode */
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int asustor_mcu_hwmon_write(struct device *dev,
				   enum hwmon_sensor_types type,
				   u32 attr, int channel, long val)
{
	struct asustor_mcu *mcu = dev_get_drvdata(dev);

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_pwm_input:
		if (val < 0 || val > 255)
			return -EINVAL;
		return mcu_fan_set_pwm(mcu, val);
	case hwmon_pwm_enable:
		/* Only manual mode supported */
		if (val != 1)
			return -EINVAL;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info *asustor_mcu_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_ops asustor_mcu_hwmon_ops = {
	.is_visible = asustor_mcu_hwmon_is_visible,
	.read       = asustor_mcu_hwmon_read,
	.write      = asustor_mcu_hwmon_write,
};

static const struct hwmon_chip_info asustor_mcu_hwmon_chip_info = {
	.ops  = &asustor_mcu_hwmon_ops,
	.info = asustor_mcu_hwmon_info,
};

/* ---- LED class devices ---- */

struct mcu_led {
	struct led_classdev cdev;
	u8 base_val;	/* MCU OFF byte — add MCU_LED_MODE_* for other states */
	bool blinking;	/* true if MCU is currently in a hardware blink mode */
	bool last_valid;	/* @last_brightness reflects a real prior write */
	enum led_brightness last_brightness;	/* last value we programmed */
};

/*
 * The MCU keeps a single state per LED, so any subsequent ON/OFF write
 * silently cancels an active hardware blink. The kernel LED core (and
 * userspace via sysfs) routinely calls brightness_set(1) right after
 * blink_set() — see issue #10. Track whether we last programmed a blink
 * mode and absorb any non-zero brightness write while the LED is blinking;
 * a brightness=0 write still takes effect (turn the LED off, exit blink).
 *
 * Userspace status daemons commonly re-assert the same brightness on every
 * poll cycle (e.g. asustor-status writing brightness=1 to green:status and
 * brightness=0 to red:status every few seconds, regardless of any state
 * change). Each MCU command costs a serial round-trip and risks a -EIO
 * timeout if the MCU is busy with concurrent work (LCD frames, fan PWM).
 * Skip the round-trip entirely when the requested brightness already
 * matches what we last programmed and we are not in a blink mode.
 */
static int mcu_led_brightness_set(struct led_classdev *cdev,
				  enum led_brightness brightness)
{
	struct mcu_led *led = container_of(cdev, struct mcu_led, cdev);
	u8 mode = brightness ? MCU_LED_MODE_ON : MCU_LED_MODE_OFF;
	u8 cmd[] = { MCU_CMD_SET_LED, MCU_CMD_LED_SUBCMD_LIGHT,
		     led->base_val + mode };
	int ret;

	if (brightness && led->blinking)
		return 0;

	if (led->last_valid && !led->blinking &&
	    led->last_brightness == brightness)
		return 0;

	ret = mcu_cmd_locked(mcu_data, cmd, sizeof(cmd), NULL, 0);
	if (ret < 0)
		return ret;

	led->last_brightness = brightness;
	led->last_valid = true;
	led->blinking = false;
	return 0;
}

/* MCU hardware blink speeds — symmetric 50% duty cycle, measured on AS6806T */
static const struct {
	unsigned long delay;	/* half-period in ms */
	u8 mode;
} mcu_blink_speeds[] = {
	{ 500, MCU_LED_MODE_BLINK_SLOW },
	{ 250, MCU_LED_MODE_BLINK_MED },
	{ 125, MCU_LED_MODE_BLINK_FAST },
	{  63, MCU_LED_MODE_BLINK_FASTEST },
};

static int mcu_led_blink_set(struct led_classdev *cdev,
			     unsigned long *delay_on,
			     unsigned long *delay_off)
{
	struct mcu_led *led = container_of(cdev, struct mcu_led, cdev);
	unsigned long period = *delay_on + *delay_off;
	unsigned long best_diff = ~0UL;
	int best = 0;
	int i;
	int ret;
	u8 cmd[3];

	/* Default to slow blink if both delays are zero */
	if (period == 0)
		period = 1000;

	for (i = 0; i < ARRAY_SIZE(mcu_blink_speeds); i++) {
		unsigned long hw_period = 2 * mcu_blink_speeds[i].delay;
		unsigned long diff = (period > hw_period) ?
			period - hw_period : hw_period - period;

		if (diff < best_diff) {
			best = i;
			best_diff = diff;
		}
	}

	*delay_on = mcu_blink_speeds[best].delay;
	*delay_off = mcu_blink_speeds[best].delay;

	cmd[0] = MCU_CMD_SET_LED;
	cmd[1] = MCU_CMD_LED_SUBCMD_LIGHT;
	cmd[2] = led->base_val + mcu_blink_speeds[best].mode;

	ret = mcu_cmd_locked(mcu_data, cmd, sizeof(cmd), NULL, 0);
	if (ret < 0)
		return ret;

	led->blinking = true;
	return 0;
}

static struct mcu_led mcu_leds[] = {
	{
		.cdev = {
			.name			 = "asustor:blue:power",
			.brightness_set_blocking = mcu_led_brightness_set,
			.blink_set		 = mcu_led_blink_set,
			.max_brightness		 = 1,
			.default_trigger	 = "default-on",
		},
		.base_val = MCU_LED_POWER_BLUE,
	},
	{
		.cdev = {
			.name			 = "asustor:orange:power",
			.brightness_set_blocking = mcu_led_brightness_set,
			.blink_set		 = mcu_led_blink_set,
			.max_brightness		 = 1,
		},
		.base_val = MCU_LED_POWER_ORANGE,
	},
	{
		.cdev = {
			.name			 = "asustor:green:status",
			.brightness_set_blocking = mcu_led_brightness_set,
			.blink_set		 = mcu_led_blink_set,
			.max_brightness		 = 1,
			.default_trigger	 = "timer",
		},
		.base_val = MCU_LED_STATUS_GREEN,
	},
	{
		.cdev = {
			.name			 = "asustor:red:status",
			.brightness_set_blocking = mcu_led_brightness_set,
			.blink_set		 = mcu_led_blink_set,
			.max_brightness		 = 1,
			.default_trigger	 = "panic",
		},
		.base_val = MCU_LED_STATUS_RED,
	},
};

/* ---- Init / Cleanup ---- */

/*
 * Setting and reading back the fan PWM at boot is the slowest part of MCU
 * init (~2 s in total). The MCU is responsive the whole time — each command
 * is just slow — so we defer this to a workqueue and let module init return
 * immediately. Any concurrent hwmon access from userspace simply queues
 * behind mcu->lock as normal.
 *
 * LED registration is also deferred so that LED default triggers (which
 * fire synchronously inside led_classdev_register and issue MCU commands)
 * don't extend insmod time. Until this worker finishes, the LED sysfs
 * nodes simply don't exist; userspace gets a clean -ENOENT instead of
 * blocking on mcu->lock for ~2 s.
 */
static void mcu_init_work(struct work_struct *work)
{
	struct asustor_mcu *mcu = container_of(work, struct asustor_mcu,
					       init_work);
	int ret, i;

	/*
	 * Register LEDs first so they appear in sysfs as soon as possible
	 * (and the panic trigger on red:status gets wired up early). Their
	 * default triggers may issue MCU commands synchronously, but that's
	 * fine here off the init path.
	 */
	for (i = 0; i < ARRAY_SIZE(mcu_leds); i++) {
		ret = led_classdev_register(NULL, &mcu_leds[i].cdev);
		if (ret) {
			dev_warn(&mcu->pdev->dev,
				 "failed to register LED %s: %d\n",
				 mcu_leds[i].cdev.name, ret);
			break;
		}
	}
	mcu->leds_registered = i;

	ret = mcu_fan_set_pwm(mcu, MCU_FAN_DEFAULT_PWM);
	if (ret < 0)
		dev_warn(&mcu->pdev->dev,
			 "failed to set default fan PWM: %d\n", ret);
	ret = mcu_fan_get_pwm(mcu);
	if (ret < 0)
		dev_warn(&mcu->pdev->dev,
			 "failed to read initial fan PWM: %d\n", ret);

	dev_info(&mcu->pdev->dev,
		 "MCU ready, fan PWM: %d, %u/%zu LEDs registered\n",
		 mcu->fan_pwm, mcu->leds_registered, ARRAY_SIZE(mcu_leds));
}

int __init asustor_mcu_init(void)
{
	struct asustor_mcu *mcu;
	struct file *f;
	struct platform_device *pdev;
	int ret;

	pr_info("initializing MCU on %s\n", MCU_SERIAL_PORT);

	mcu = kzalloc(sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	mutex_init(&mcu->lock);
	INIT_WORK(&mcu->init_work, mcu_init_work);

	/* Open serial port */
	f = mcu_serial_open();
	if (IS_ERR(f)) {
		ret = PTR_ERR(f);
		goto err_free;
	}
	mcu->tty_filp = f;
	mcu_data = mcu;

	/* Create a platform device as parent for hwmon */
	pdev = platform_device_register_simple("asustor_mcu", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		pr_err("failed to register platform device: %d\n", ret);
		goto err_serial;
	}
	mcu->pdev = pdev;

	/* Register hwmon for fan control */
	mcu->hwmon_dev = hwmon_device_register_with_info(&pdev->dev, "asustor_mcu",
			mcu, &asustor_mcu_hwmon_chip_info, NULL);
	if (IS_ERR(mcu->hwmon_dev)) {
		ret = PTR_ERR(mcu->hwmon_dev);
		dev_err(&pdev->dev, "failed to register hwmon: %d\n", ret);
		goto err_pdev;
	}

	/*
	 * Apply the default fan speed, read it back, and register the LEDs
	 * asynchronously so module init doesn't block on slow MCU round-trips
	 * (~2 s) or on synchronous LED default-trigger commands.
	 */
	schedule_work(&mcu->init_work);

	dev_info(&pdev->dev, "MCU initialized\n");

	return 0;

err_pdev:
	platform_device_unregister(mcu->pdev);
err_serial:
	mcu_serial_close(mcu->tty_filp);
err_free:
	mutex_destroy(&mcu->lock);
	mcu_data = NULL;
	kfree(mcu);
	return ret;
}

void __exit asustor_mcu_cleanup(void)
{
	struct asustor_mcu *mcu = mcu_data;
	int i;

	if (!mcu)
		return;

	/* Make sure the deferred init work isn't still touching the MCU. */
	cancel_work_sync(&mcu->init_work);

	for (i = 0; i < mcu->leds_registered; i++)
		led_classdev_unregister(&mcu_leds[i].cdev);

	hwmon_device_unregister(mcu->hwmon_dev);
	platform_device_unregister(mcu->pdev);
	mcu_serial_close(mcu->tty_filp);
	mutex_destroy(&mcu->lock);
	mcu_data = NULL;
	kfree(mcu);

	pr_info("MCU cleaned up\n");
}
