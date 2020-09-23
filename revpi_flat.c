/*
 * Copyright (C) 2020 KUNBUS GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2) as
 * published by the Free Software Foundation.
 */

#include "revpi_flat.h"

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/thermal.h>
#include <linux/iio/consumer.h>
#include <linux/gpio/consumer.h>
#include <uapi/linux/sched/types.h>

#include "revpi_common.h"
#include "project.h"
#include "piControlMain.h"
#include "process_image.h"

/* relais gpio num */
#define REVPI_FLAT_RELAIS_GPIO			28

#define REVPI_FLAT_DOUT_THREAD_PRIO		(MAX_USER_RT_PRIO / 2 + 8)
#define REVPI_FLAT_AIN_THREAD_PRIO		(MAX_USER_RT_PRIO / 2 + 6)
/* This value is a correction factor which takes the currency loss caused
   by resistors into account. See the flat schematics for details. */
#define REVPI_FLAT_AIN_CORRECTION		1986582478
#define REVPI_FLAT_AIN_POLL_INTERVAL		85

struct revpi_flat_config {
	unsigned int offset;
};

struct revpi_flat_image {
	struct {
		s16 ain;
#define REVPI_FLAT_AIN_TX_ERR  			7
		u8 ain_status;
		u8 cpu_temp;
		u8 cpu_freq;
	} __attribute__ ((__packed__)) drv;
	struct {
		u8 dout;
	} __attribute__ ((__packed__)) usr;
} __attribute__ ((__packed__));

struct revpi_flat {
	struct revpi_flat_image image;
	struct revpi_flat_config config;
	struct task_struct *dout_thread;
	struct task_struct *ain_thread;
	struct device *din_dev;
	struct gpio_desc *dout_fault;
	struct gpio_desc *digout;
	struct gpio_descs *dout;
	struct iio_channel ain;
};

static const struct kthread_prio revpi_flat_kthread_prios[] = {
	/* softirq daemons handling hrtimers */
	{ .comm = "ktimersoftd/0",
	  .prio = MAX_USER_RT_PRIO/2 + 10
	},
	{ .comm = "ktimersoftd/1",
	  .prio = MAX_USER_RT_PRIO/2 + 10
	},
	{ .comm = "ktimersoftd/2",
	  .prio = MAX_USER_RT_PRIO/2 + 10
	},
	{ .comm = "ktimersoftd/3",
	  .prio = MAX_USER_RT_PRIO/2 + 10
	},
	{ }
};

static int revpi_flat_poll_dout(void *data)
{
	struct revpi_flat *flat = (struct revpi_flat *) data;
	struct revpi_flat_image *image = &flat->image;
	struct revpi_flat_image *usr_image;
	int dout_val = -1;

	usr_image = (struct revpi_flat_image *) (piDev_g.ai8uPI +
						 flat->config.offset);
	while (!kthread_should_stop()) {
		my_rt_mutex_lock(&piDev_g.lockPI);
		usr_image->drv = image->drv;

		if (usr_image->usr.dout != image->usr.dout)
			dout_val = usr_image->usr.dout;

		image->usr = usr_image->usr;
		rt_mutex_unlock(&piDev_g.lockPI);

		if (dout_val != -1) {
			gpiod_set_value_cansleep(flat->digout, !!dout_val);
			dout_val = -1;
		}

		usleep_range(100, 150);
	}

	return 0;
}

static int revpi_flat_handle_ain(struct revpi_flat *flat)
{
	struct revpi_flat_image *image = &flat->image;
	unsigned long long ain_val;
	int raw_val;
	int ret;


	ret = iio_read_channel_raw(&flat->ain, &raw_val);
	assign_bit_in_byte(REVPI_FLAT_AIN_TX_ERR, &image->drv.ain_status,
			   ret < 0);
	if (ret < 0) {
		dev_err(piDev_g.dev, "failed to read from analog "
			"channel: %i\n", ret);
		return ret;
	}
	/* AIN value in mV = ((raw * 12.5V) >> 21 bit) + 6.25V */
	ain_val = shift_right((s64) raw_val * 12500, 21) + 6250;
	ain_val *= REVPI_FLAT_AIN_CORRECTION;
	ain_val = (int) div_s64(ain_val, 1000000000LL);

	my_rt_mutex_lock(&piDev_g.lockPI);
	image->drv.ain = ain_val;
	rt_mutex_unlock(&piDev_g.lockPI);

	return 0;
}

static int revpi_flat_poll_ain(void *data)
{
	struct revpi_flat *flat = (struct revpi_flat *) data;
	struct revpi_flat_image *image = &flat->image;
	int temperature;
	int ret;

	while (!kthread_should_stop()) {
		ret = revpi_flat_handle_ain(flat);
		if (ret)
			msleep(REVPI_FLAT_AIN_POLL_INTERVAL);

		my_rt_mutex_lock(&piDev_g.lockPI);
		/* read cpu temperature */
		if (piDev_g.thermal_zone != NULL) {
			ret = thermal_zone_get_temp(piDev_g.thermal_zone,
						    &temperature);
			if (ret) {
				dev_err(piDev_g.dev,"Failed to get cpu "
					"temperature");
			} else {
				image->drv.cpu_temp = temperature / 1000;
			}
		}
		/* read cpu frequency */
		image->drv.cpu_freq = bcm2835_cpufreq_get_clock() / 10;
		rt_mutex_unlock(&piDev_g.lockPI);
	}

	return 0;
}

static int revpi_flat_match_iio_name(struct device *dev, void *data)
{
	return !strcmp(data, dev_to_iio_dev(dev)->name);
}

int revpi_flat_init(void)
{
	struct revpi_flat *flat;
	struct sched_param param = { };
	struct device *dev;
	int ret;

	flat = devm_kzalloc(piDev_g.dev, sizeof(*flat), GFP_KERNEL);
	if (!flat)
		return -ENOMEM;

	piDev_g.machine = flat;

	flat->digout = gpio_to_desc(REVPI_FLAT_RELAIS_GPIO);
	if (!flat->digout) {
		dev_err(piDev_g.dev, "no gpio desc for digital output found\n");
		return -ENXIO;
	}

	ret = gpiod_direction_output(flat->digout, 0);
	if (ret) {
		dev_err(piDev_g.dev, "Failed to set direction for relais "
			"gpio %i\n", ret);
		return -ENXIO;
	}

	dev = bus_find_device(&iio_bus_type, NULL, "mcp3550-50",
			      revpi_flat_match_iio_name);
	if (!dev) {
		dev_err(piDev_g.dev, "cannot find analog input device\n");
		return -ENODEV;
	}

	flat->ain.indio_dev = dev_to_iio_dev(dev);
	flat->ain.channel = &(flat->ain.indio_dev->channels[0]);

	flat->dout_thread = kthread_create(&revpi_flat_poll_dout, flat,
					   "piControl dout");
	if (IS_ERR(flat->dout_thread)) {
		dev_err(piDev_g.dev, "cannot create dout thread\n");
		ret = PTR_ERR(flat->dout_thread);
		goto err_put_ain;
	}

	param.sched_priority = REVPI_FLAT_DOUT_THREAD_PRIO;
	ret = sched_setscheduler(flat->dout_thread, SCHED_FIFO, &param);
	if (ret) {
		dev_err(piDev_g.dev, "cannot upgrade dout thread priority\n");
		goto err_stop_dout_thread;
	}

	flat->ain_thread = kthread_create(&revpi_flat_poll_ain, flat,
					  "piControl ain");
	if (IS_ERR(flat->ain_thread)) {
		dev_err(piDev_g.dev, "cannot create ain thread\n");
		ret = PTR_ERR(flat->ain_thread);
		goto err_stop_dout_thread;
	}

	param.sched_priority = REVPI_FLAT_AIN_THREAD_PRIO;
	ret = sched_setscheduler(flat->ain_thread, SCHED_FIFO, &param);
	if (ret) {
		dev_err(piDev_g.dev, "cannot upgrade ain thread priority\n");
		goto err_stop_ain_thread;
	}

	ret = set_kthread_prios(revpi_flat_kthread_prios);
	if (ret)
		goto err_stop_ain_thread;

	wake_up_process(flat->dout_thread);
	wake_up_process(flat->ain_thread);

	return 0;

err_stop_ain_thread:
	kthread_stop(flat->ain_thread);
err_stop_dout_thread:
	kthread_stop(flat->dout_thread);
err_put_ain:
	iio_device_put(flat->ain.indio_dev);

	return ret;
}

void revpi_flat_fini(void)
{
	struct revpi_flat *flat = (struct revpi_flat *) piDev_g.machine;

	kthread_stop(flat->ain_thread);
	kthread_stop(flat->dout_thread);
	iio_device_put(flat->ain.indio_dev);
}