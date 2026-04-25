// SPDX-License-Identifier: GPL-2.0
/*
 * asustor_lcm.c - Front-panel LCD module for ASUSTOR AS68xxT
 *                 (Lockerstor Gen3) NAS hardware.
 *
 * Communicates with the front-panel LCD controller via /dev/ttyS2 at 9600 baud
 * (8N1) using a framed packet protocol (0xF0 header + checksum).
 *
 * The LCD is a 16x2 character display. Its controller must be enabled
 * before use via a dedicated GPIO (acquired here as con_id "enable"; the
 * pin mapping is installed by asustor_as68xx.c as a gpiod_lookup_table).
 * The backlight has a separate supply and is not controlled by this line.
 *
 * The LCD module also has 4 navigation buttons. When pressed, the LCD controller
 * sends unsolicited F0 packets with CMD 0x80 containing the button ID. A receive
 * thread reads these packets and reports button events via a Linux input device.
 *
 * Provides sysfs attributes:
 *   /sys/devices/platform/asustor_lcm/lcd_line0  — top line text (16 chars)
 *   /sys/devices/platform/asustor_lcm/lcd_line1  — bottom line text (16 chars)
 *   /sys/devices/platform/asustor_lcm/lcd_clear  — write-only, clears display
 *
 * Copyright (C) 2026 Thierry Tremblay
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/workqueue.h>

#include "asustor_lcm.h"

/* LCD serial port parameters */
#define LCM_SERIAL_PORT		"/dev/ttyS2"
#define LCM_BAUD_RATE		B9600

/* LCD framed protocol constants */
#define LCM_FRAME_HEADER	0xF0
#define LCM_FRAME_ACK_HEADER	0xF1
#define LCM_FRAME_MAX_SIZE	22

/* LCD commands (sent by host) */
#define LCM_CMD_POWER		0x11
#define LCM_CMD_CLEAR		0x22
#define LCM_CMD_TEXT		0x27

/* LCD commands (received from LCD controller) */
#define LCM_CMD_BUTTON		0x80	/* button press event */

/* LCD button IDs (data byte in CMD 0x80 packets) */
#define LCM_BTN_UP		1
#define LCM_BTN_DOWN		2
#define LCM_BTN_BACK		3
#define LCM_BTN_ENTER		4

/* LCD display dimensions */
#define LCM_WIDTH		16
#define LCM_NUM_LINES		2

/* Maximum time to wait for an ACK from the LCD controller */
#define LCM_ACK_TIMEOUT_MS	500

/*
 * Upper bound on cold-boot probe time. The controller takes ~2 s to come
 * up after the enable GPIO is raised; we poll with 500 ms ACK timeouts
 * for up to 5 s. Wait a little longer here so callers always observe a
 * definitive result rather than a spurious timeout.
 */
#define LCM_PROBE_WAIT_MS	6000

struct asustor_lcm {
	struct mutex write_lock;	/* serializes writes to the tty */
	struct file *tty_filp;
	struct platform_device *pdev;
	struct gpio_desc *enable_gpio;	/* LCD controller enable (GPIO 4 on AS6806T) */
	char line_cache[LCM_NUM_LINES][LCM_WIDTH + 1]; /* cached display text */
	struct input_dev *input_dev;	/* input device for LCD buttons */
	struct task_struct *rx_thread;	/* sole reader of the tty */
	struct completion ack_done;	/* fired by rx thread on F1 ACK */
	u8 ack_status;			/* status byte from last F1 ACK */
	unsigned int rx_errs;		/* count of rx protocol/checksum errors */
	struct work_struct probe_work;	/* async cold-boot probe */
	struct completion probe_done;	/* fired when probe_work finishes */
	bool probe_ok;			/* true if controller ACKed during probe */
};

static struct asustor_lcm *lcm_data;

/* ---- Low-level serial I/O ---- */

static struct file *lcm_serial_open(void)
{
	struct file *f;
	struct tty_struct *tty;
	struct ktermios termios;

	/*
	 * Open with O_NONBLOCK so filp_open() doesn't stall waiting for
	 * carrier detect, then clear it so subsequent reads honor VMIN /
	 * VTIME instead of returning -EAGAIN immediately when the rx
	 * buffer is momentarily empty.
	 */
	f = filp_open(LCM_SERIAL_PORT, O_RDWR | O_NOCTTY | O_NONBLOCK, 0);
	if (IS_ERR(f)) {
		pr_err("failed to open %s: %ld\n", LCM_SERIAL_PORT, PTR_ERR(f));
		return f;
	}
	f->f_flags &= ~O_NONBLOCK;

	tty = ((struct tty_file_private *)f->private_data)->tty;
	if (!tty) {
		pr_err("failed to get tty struct\n");
		filp_close(f, NULL);
		return ERR_PTR(-ENODEV);
	}

	/* Configure 9600 8N1 raw */
	termios = tty->termios;
	termios.c_iflag = 0;
	termios.c_oflag = 0;
	termios.c_cflag = LCM_BAUD_RATE | CS8 | CREAD | CLOCAL;
	termios.c_lflag = 0;
	termios.c_cc[VMIN] = 0;
	termios.c_cc[VTIME] = 1;	/* 100ms inter-byte timeout */
	tty_set_termios(tty, &termios);

	return f;
}

static void lcm_serial_close(struct file *f)
{
	if (!IS_ERR_OR_NULL(f))
		filp_close(f, NULL);
}

/* Compute 8-bit checksum: sum of all bytes in the packet */
static u8 lcm_checksum(const u8 *buf, int len)
{
	u8 sum = 0;
	int i;

	for (i = 0; i < len; i++)
		sum += buf[i];
	return sum;
}

/* Send a raw buffer to the tty. Caller must hold write_lock. */
static int lcm_write_raw(struct asustor_lcm *lcm, const u8 *buf, int len)
{
	loff_t pos = 0;
	int ret;

	ret = kernel_write(lcm->tty_filp, buf, len, &pos);
	if (ret < 0)
		return ret;
	if (ret != len)
		return -EIO;
	return 0;
}

/*
 * Send a framed packet to the LCD controller and wait for the F1 ACK.
 *
 * The rx thread is the sole reader of the tty; it parses incoming bytes
 * and fires lcm->ack_done when it sees an F1 frame. We just need to
 * write our frame and wait for the completion.
 *
 * NB: this is the low-level path. It does NOT wait for the async probe
 * to finish, so the probe worker can use it directly. User-facing
 * callers (sysfs) must go through lcm_send_frame() which gates on
 * lcm->probe_done first.
 *
 * @quiet suppresses the timeout warning; the probe worker uses it to
 * avoid spamming dmesg while polling a controller that hasn't woken up.
 */
static int __lcm_send_frame(struct asustor_lcm *lcm, u8 cmd,
			    const u8 *data, int data_len, bool quiet)
{
	u8 frame[LCM_FRAME_MAX_SIZE];
	int frame_len;
	int ret;

	frame_len = data_len + 4;	/* header + length + cmd + data + checksum */
	if (frame_len > LCM_FRAME_MAX_SIZE)
		return -EINVAL;

	frame[0] = LCM_FRAME_HEADER;
	frame[1] = data_len;
	frame[2] = cmd;
	if (data_len > 0)
		memcpy(&frame[3], data, data_len);
	frame[frame_len - 1] = lcm_checksum(frame, frame_len - 1);

	mutex_lock(&lcm->write_lock);
	reinit_completion(&lcm->ack_done);

	ret = lcm_write_raw(lcm, frame, frame_len);
	if (ret) {
		mutex_unlock(&lcm->write_lock);
		return ret;
	}

	if (!wait_for_completion_timeout(&lcm->ack_done,
					 msecs_to_jiffies(LCM_ACK_TIMEOUT_MS))) {
		mutex_unlock(&lcm->write_lock);
		if (!quiet)
			pr_warn_ratelimited("timeout waiting for ACK to cmd 0x%02x\n",
					    cmd);
		return -ETIMEDOUT;
	}
	mutex_unlock(&lcm->write_lock);

	return 0;
}

/*
 * User-facing send path. Blocks until the async probe has finished, so
 * callers never race the controller's ~2 s cold-boot wake-up. Returns
 * -ENODEV if the controller never ACKed during the probe window.
 */
static int lcm_send_frame(struct asustor_lcm *lcm, u8 cmd,
			  const u8 *data, int data_len)
{
	long rc;

	rc = wait_for_completion_interruptible_timeout(&lcm->probe_done,
		msecs_to_jiffies(LCM_PROBE_WAIT_MS));
	if (rc < 0)
		return rc;		/* -ERESTARTSYS */
	if (rc == 0)
		return -ETIMEDOUT;
	if (!lcm->probe_ok)
		return -ENODEV;

	return __lcm_send_frame(lcm, cmd, data, data_len, false);
}

/* Send an F1 ACK response for a received F0 frame. Takes write_lock. */
static void lcm_send_ack(struct asustor_lcm *lcm, u8 cmd)
{
	u8 ack[5];
	int ret;

	ack[0] = LCM_FRAME_ACK_HEADER;
	ack[1] = 0x01;		/* 1 data byte */
	ack[2] = cmd;
	ack[3] = 0x00;		/* status: OK */
	ack[4] = lcm_checksum(ack, 4);

	/*
	 * Use trylock to avoid blocking the rx thread on a writer that's
	 * waiting for its own ACK (which we, the rx thread, must read).
	 * If a writer holds the lock, skipping this ACK is harmless; the
	 * controller may resend the button event, which we'll then ACK.
	 */
	if (!mutex_trylock(&lcm->write_lock))
		return;
	ret = lcm_write_raw(lcm, ack, sizeof(ack));
	mutex_unlock(&lcm->write_lock);
	if (ret)
		pr_warn_ratelimited("failed to send ACK for cmd 0x%02x: %d\n",
				    cmd, ret);
}

/* Map LCD button ID to Linux input key code */
static unsigned int lcm_button_to_keycode(u8 button_id)
{
	switch (button_id) {
	case LCM_BTN_UP:	return KEY_UP;
	case LCM_BTN_DOWN:	return KEY_DOWN;
	case LCM_BTN_BACK:	return KEY_ESC;
	case LCM_BTN_ENTER:	return KEY_ENTER;
	default:		return KEY_UNKNOWN;
	}
}

/* Process a fully-received, checksum-verified F0 frame from the controller. */
static void lcm_handle_f0_frame(struct asustor_lcm *lcm, const u8 *buf,
				int frame_len)
{
	/* Send ACK back to controller for any received F0 frame */
	lcm_send_ack(lcm, buf[2]);

	/* Handle button press (CMD 0x80, 1 data byte = button ID) */
	if (buf[2] == LCM_CMD_BUTTON && buf[1] >= 1) {
		u8 button_id = buf[3];
		unsigned int keycode = lcm_button_to_keycode(button_id);

		if (keycode != KEY_UNKNOWN) {
			input_report_key(lcm->input_dev, keycode, 1);
			input_sync(lcm->input_dev);
			input_report_key(lcm->input_dev, keycode, 0);
			input_sync(lcm->input_dev);
		}
		pr_debug("button press: id=%d keycode=%d\n",
			 button_id, keycode);
	}
}

/*
 * Sole reader of the tty. Runs a byte-level state machine that parses
 * both F0 (controller-initiated) and F1 (ACK) frames. F1 frames fire
 * the writer's completion; F0 frames are dispatched via lcm_handle_f0_frame().
 *
 * Because this thread owns the read side exclusively, there is no need
 * to coordinate with writers via a mutex on reads — eliminating the
 * race that the previous shared-tty design suffered from.
 */
static int lcm_rx_thread(void *data)
{
	struct asustor_lcm *lcm = data;
	enum { ST_HEADER, ST_LEN, ST_PAYLOAD } state = ST_HEADER;
	u8 frame[LCM_FRAME_MAX_SIZE];
	int idx = 0, frame_len = 0;
	bool is_ack = false;

	while (!kthread_should_stop()) {
		u8 byte;
		loff_t pos = 0;
		int ret;

		ret = kernel_read(lcm->tty_filp, &byte, 1, &pos);
		if (ret <= 0) {
			/*
			 * VTIME timeout (no data for 100 ms): if we were
			 * mid-frame, the partial frame is garbage from
			 * line noise or a stale buffer — reset so we
			 * don't consume the next valid frame's bytes
			 * waiting for a phantom continuation.
			 */
			if (state != ST_HEADER) {
				state = ST_HEADER;
				idx = 0;
			}
			continue;
		}

		switch (state) {
		case ST_HEADER:
			if (byte == LCM_FRAME_HEADER ||
			    byte == LCM_FRAME_ACK_HEADER) {
				frame[0] = byte;
				is_ack = (byte == LCM_FRAME_ACK_HEADER);
				idx = 1;
				state = ST_LEN;
			}
			/* otherwise: drop noise byte and stay in ST_HEADER */
			break;

		case ST_LEN:
			frame[idx++] = byte;
			frame_len = byte + 4;
			if (frame_len > LCM_FRAME_MAX_SIZE) {
				lcm->rx_errs++;
				pr_warn_ratelimited("rx oversize len=%u (total errs: %u)\n",
						    byte, lcm->rx_errs);
				state = ST_HEADER;
				break;
			}
			state = ST_PAYLOAD;
			break;

		case ST_PAYLOAD:
			frame[idx++] = byte;
			if (idx >= frame_len) {
				u8 expected = lcm_checksum(frame, frame_len - 1);

				if (frame[frame_len - 1] != expected) {
					lcm->rx_errs++;
					pr_warn_ratelimited("rx checksum: got 0x%02x exp 0x%02x (total errs: %u)\n",
							    frame[frame_len - 1],
							    expected, lcm->rx_errs);
				} else if (is_ack) {
					lcm->ack_status = frame[3];
					complete(&lcm->ack_done);
				} else {
					lcm_handle_f0_frame(lcm, frame, frame_len);
				}
				state = ST_HEADER;
			}
			break;
		}
	}

	return 0;
}

/* ---- LCD command helpers ---- */

static int lcm_cmd_write_line(struct asustor_lcm *lcm, int line,
			      const char *text)
{
	u8 data[2 + LCM_WIDTH];	/* line + col + 16 chars */
	int len, i;

	if (line < 0 || line >= LCM_NUM_LINES)
		return -EINVAL;

	data[0] = line;
	data[1] = 0x00;		/* column = 0 */

	len = strnlen(text, LCM_WIDTH);
	memcpy(&data[2], text, len);

	/* Pad remaining characters with spaces */
	for (i = len; i < LCM_WIDTH; i++)
		data[2 + i] = ' ';

	return lcm_send_frame(lcm, LCM_CMD_TEXT, data, sizeof(data));
}

/* ---- sysfs attributes ---- */

static ssize_t lcd_line_show(int line, char *buf)
{
	return sysfs_emit(buf, "%s\n", lcm_data->line_cache[line]);
}

static ssize_t lcd_line_store(int line, const char *buf, size_t count)
{
	struct asustor_lcm *lcm = lcm_data;
	char text[LCM_WIDTH + 1];
	int len, ret;

	len = min_t(int, count, LCM_WIDTH);

	/* Strip trailing newline */
	if (len > 0 && buf[len - 1] == '\n')
		len--;

	memcpy(text, buf, len);
	text[len] = '\0';

	ret = lcm_cmd_write_line(lcm, line, text);
	if (ret == 0) {
		memset(lcm->line_cache[line], 0, sizeof(lcm->line_cache[line]));
		memcpy(lcm->line_cache[line], text, len);
	}

	return ret < 0 ? ret : count;
}

static ssize_t lcd_line0_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return lcd_line_show(0, buf);
}

static ssize_t lcd_line0_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	return lcd_line_store(0, buf, count);
}

static ssize_t lcd_line1_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return lcd_line_show(1, buf);
}

static ssize_t lcd_line1_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	return lcd_line_store(1, buf, count);
}

static ssize_t lcd_clear_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct asustor_lcm *lcm = lcm_data;
	int ret;

	ret = lcm_cmd_write_line(lcm, 0, "");
	if (ret == 0)
		ret = lcm_cmd_write_line(lcm, 1, "");
	if (ret == 0)
		memset(lcm->line_cache, 0, sizeof(lcm->line_cache));

	return ret < 0 ? ret : count;
}

static DEVICE_ATTR_RW(lcd_line0);
static DEVICE_ATTR_RW(lcd_line1);
static DEVICE_ATTR_WO(lcd_clear);

static struct attribute *asustor_lcm_attrs[] = {
	&dev_attr_lcd_line0.attr,
	&dev_attr_lcd_line1.attr,
	&dev_attr_lcd_clear.attr,
	NULL
};

static const struct attribute_group asustor_lcm_attr_group = {
	.attrs = asustor_lcm_attrs,
};

/* ---- Async cold-boot probe ---- */

/*
 * The LCD controller takes ~2 s after the enable GPIO is raised before it
 * responds to commands. Doing this synchronously in module init blocks
 * boot for ~2 s on cold start (warm reload returns on the first try).
 *
 * Run the probe on a workqueue instead and gate user-facing senders on
 * lcm->probe_done. Button events that arrive before the probe completes
 * are simply dropped on the floor — the rx thread is already running, but
 * has nothing meaningful to do until the controller is responsive.
 */
static void lcm_probe_work(struct work_struct *work)
{
	struct asustor_lcm *lcm = container_of(work, struct asustor_lcm,
					       probe_work);
	unsigned long deadline = jiffies + msecs_to_jiffies(5000);
	u8 on = 1, zero = 0;
	int ret;

	do {
		ret = __lcm_send_frame(lcm, LCM_CMD_POWER, &on, 1, true);
		if (ret == 0)
			break;
		if (ret != -ETIMEDOUT)
			break;
	} while (time_before(jiffies, deadline));

	if (ret) {
		dev_warn(&lcm->pdev->dev,
			 "LCD did not respond to power-on: %d\n", ret);
		goto out;
	}

	msleep(15);
	ret = __lcm_send_frame(lcm, LCM_CMD_CLEAR, &zero, 1, false);
	if (ret) {
		dev_warn(&lcm->pdev->dev, "LCD clear failed: %d\n", ret);
		goto out;
	}

	lcm->probe_ok = true;
	dev_info(&lcm->pdev->dev, "LCD controller ready\n");
out:
	complete_all(&lcm->probe_done);
}

/* ---- Init / Cleanup ---- */

int __init asustor_lcm_init(void)
{
	struct asustor_lcm *lcm;
	struct input_dev *input;
	struct file *f;
	struct platform_device *pdev;
	int ret;

	pr_info("initializing LCM on %s\n", LCM_SERIAL_PORT);

	lcm = kzalloc(sizeof(*lcm), GFP_KERNEL);
	if (!lcm)
		return -ENOMEM;

	mutex_init(&lcm->write_lock);
	init_completion(&lcm->ack_done);
	init_completion(&lcm->probe_done);
	INIT_WORK(&lcm->probe_work, lcm_probe_work);

	/* Open serial port */
	f = lcm_serial_open();
	if (IS_ERR(f)) {
		ret = PTR_ERR(f);
		goto err_free;
	}
	lcm->tty_filp = f;
	lcm_data = lcm;

	/* Create platform device for sysfs */
	pdev = platform_device_register_simple("asustor_lcm", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		pr_err("failed to register platform device: %d\n", ret);
		goto err_serial;
	}
	lcm->pdev = pdev;

	/*
	 * Enable the LCD controller before talking to it. Bound to this pdev
	 * via gpiod_lookup_table in asustor_as68xx.c with con_id "enable".
	 */
	lcm->enable_gpio = gpiod_get(&pdev->dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(lcm->enable_gpio)) {
		ret = PTR_ERR(lcm->enable_gpio);
		dev_err(&pdev->dev, "failed to acquire LCD enable GPIO: %d\n", ret);
		lcm->enable_gpio = NULL;
		goto err_pdev;
	}

	ret = sysfs_create_group(&pdev->dev.kobj, &asustor_lcm_attr_group);
	if (ret) {
		dev_err(&pdev->dev, "failed to create sysfs group: %d\n", ret);
		goto err_gpio;
	}

	/* Register input device for LCD navigation buttons */
	input = input_allocate_device();
	if (!input) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "failed to allocate input device\n");
		goto err_sysfs;
	}

	input->name = "ASUSTOR LCD Buttons";
	input->phys = "asustor_lcm/input0";
	input->id.bustype = BUS_RS232;
	input->dev.parent = &pdev->dev;

	input_set_capability(input, EV_KEY, KEY_UP);
	input_set_capability(input, EV_KEY, KEY_DOWN);
	input_set_capability(input, EV_KEY, KEY_ESC);
	input_set_capability(input, EV_KEY, KEY_ENTER);

	ret = input_register_device(input);
	if (ret) {
		dev_err(&pdev->dev, "failed to register input device: %d\n", ret);
		input_free_device(input);
		goto err_sysfs;
	}
	lcm->input_dev = input;

	/* Start receive thread for button events */
	lcm->rx_thread = kthread_run(lcm_rx_thread, lcm, "asustor_lcm_rx");
	if (IS_ERR(lcm->rx_thread)) {
		ret = PTR_ERR(lcm->rx_thread);
		dev_err(&pdev->dev, "failed to start rx thread: %d\n", ret);
		lcm->rx_thread = NULL;
		goto err_input;
	}

	/*
	 * Kick off the controller probe asynchronously so module init
	 * returns immediately. User-facing senders (sysfs) wait on
	 * lcm->probe_done before issuing frames.
	 */
	schedule_work(&lcm->probe_work);

	dev_info(&pdev->dev,
		 "LCM initialized (16x2 display + 4 buttons on %s at 9600 baud)\n",
		 LCM_SERIAL_PORT);

	return 0;

err_input:
	input_unregister_device(lcm->input_dev);
err_sysfs:
	sysfs_remove_group(&pdev->dev.kobj, &asustor_lcm_attr_group);
err_gpio:
	if (lcm->enable_gpio) {
		gpiod_set_value_cansleep(lcm->enable_gpio, 0);
		gpiod_put(lcm->enable_gpio);
	}
err_pdev:
	platform_device_unregister(pdev);
err_serial:
	lcm_serial_close(lcm->tty_filp);
err_free:
	mutex_destroy(&lcm->write_lock);
	lcm_data = NULL;
	kfree(lcm);
	return ret;
}

void __exit asustor_lcm_cleanup(void)
{
	struct asustor_lcm *lcm = lcm_data;

	if (!lcm)
		return;

	/*
	 * Make sure the async probe is done before tearing anything down.
	 * It uses the tty, the platform device and the rx thread.
	 */
	cancel_work_sync(&lcm->probe_work);

	if (lcm->rx_thread)
		kthread_stop(lcm->rx_thread);
	if (lcm->input_dev)
		input_unregister_device(lcm->input_dev);
	sysfs_remove_group(&lcm->pdev->dev.kobj, &asustor_lcm_attr_group);
	if (lcm->enable_gpio) {
		gpiod_set_value_cansleep(lcm->enable_gpio, 0);
		gpiod_put(lcm->enable_gpio);
	}
	platform_device_unregister(lcm->pdev);
	lcm_serial_close(lcm->tty_filp);
	mutex_destroy(&lcm->write_lock);
	lcm_data = NULL;
	kfree(lcm);

	pr_info("LCM cleaned up\n");
}
