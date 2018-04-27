/*
 * AD9361 Agile RF Transceiver
 *   Module for controlling external filter banks via GPIOs
 *
 * Copyright 2018 Analog Devices Inc.
 *
 * Licensed under the GPL-2.
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>

#include "ad9361.h"

#define MAX_CTRL_GPIOS		256	/* Should be enough for a while */
#define MAX_CTRL_SETTINGS	512	/* Should be enough for a while;
					 * maybe make it configurable via DT (if needed)
					 */

/* FIXME: remove this when printouts should be more silent */
#undef dev_dbg
#define dev_dbg	dev_info

enum CTL_HOOKS {
	CTL_INIT,
	CTL_UNINIT,
	__MAX_CTL_HOOKS,
};

enum CTL_GPIO_OPS {
	CTL_GPIO_NOP,
	CTL_GPIO_OUT_LOW,
	CTL_GPIO_OUT_HIGH,
	CTL_GPIO_IN,
};

struct ad9361_ctrl_objs {
	struct gpio_descs *gpios;
};

struct ad9361_band_setting {
	const char *name;
	u64 freq_min;
	u64 freq_max;
	u32 *gpio_values;
	/* reference to the global objects to be controlled;
	 * to number of args in some calls
	 */
	struct ad9361_ctrl_objs *objs;
};

struct ad9361_ext_band_ctl {
	struct ad9361_band_setting	*hooks[__MAX_CTL_HOOKS];
	struct ad9361_band_setting	*tx_curr_setting;
	struct ad9361_band_setting	*rx_curr_setting;
	struct ad9361_band_setting	*rx_settings;	/* RX Band settings */
	struct ad9361_band_setting	*tx_settings;	/* TX Band settings */
	struct ad9361_ctrl_objs		objs;		/* Objects to control */
};

static int ad9361_apply_settings(struct ad9361_rf_phy *phy,
				 struct ad9361_band_setting *new_sett,
				 struct ad9361_band_setting **curr_sett);

static inline bool lctl_gpio_value_valid(enum CTL_GPIO_OPS val)
{
	switch (val) {
	case CTL_GPIO_NOP:	/* FALLTRHOUGH */
	case CTL_GPIO_IN:	/* FALLTRHOUGH */
	case CTL_GPIO_OUT_LOW:	/* FALLTRHOUGH */
	case CTL_GPIO_OUT_HIGH:	/* FALLTRHOUGH */
		return true;
	default:
		return false;
	}
}

static int ad9361_populate_objs(struct device *dev,
				struct ad9361_ctrl_objs *objs)
{
	char pnamebuf[sizeof("adi,band-ctl-XXX-gpio")];
	struct device_node *np = dev->of_node;
	struct gpio_descs *descs;
	int cnt;

	cnt = 0;
	for (cnt = 0; cnt < MAX_CTRL_GPIOS; cnt++) {
		snprintf(pnamebuf, sizeof(pnamebuf),
			 "adi,band-ctl-%d-gpio", cnt);
		if (!of_find_property(np, pnamebuf, NULL))
			break;
	}
	if (cnt == 0)
		return 0;

	descs = devm_kzalloc(dev,
			     sizeof(*descs) + sizeof(descs->desc[0]) * cnt,
			     GFP_KERNEL);
	if (!descs)
		return -ENOMEM;
	descs->ndescs = cnt;
	objs->gpios = descs;

	for (cnt = 0; cnt < descs->ndescs; cnt++) {
		struct gpio_desc *desc;

		snprintf(pnamebuf, sizeof(pnamebuf), "adi,band-ctl-%d", cnt);
		desc = devm_gpiod_get(dev, pnamebuf, 0);
		if (IS_ERR(desc))
			return PTR_ERR(desc);
		descs->desc[cnt] = desc;
	}
	return cnt;
}

static int ad9361_parse_gpio_settings(struct device *dev,
				      struct device_node *np,
				      struct ad9361_ext_band_ctl *ctl,
				      struct ad9361_band_setting *sett)
{
	int ret, i, pidx;
	int gpio_cnt;
	char pbuf[256];

	sett->objs = &ctl->objs;
	sett->name = np->name;

	if (!of_find_property(np, "adi,gpio-settings", NULL))
		return 0;

	gpio_cnt = ctl->objs.gpios->ndescs;
	sett->gpio_values = devm_kzalloc(dev,
					 sizeof(*(sett->gpio_values)) * gpio_cnt,
					 GFP_KERNEL);
	if (!sett->gpio_values)
		return -ENOMEM;

	ret = of_property_read_variable_u32_array(np, "adi,gpio-settings",
						  sett->gpio_values,
						  0, gpio_cnt);
	/* No GPIOs defined is a NOP */
	if (ret == 0) {
		devm_kfree(dev, sett->gpio_values);
		sett->gpio_values = NULL;
		return 0;
	}

	if (ret > 0 && (ret != gpio_cnt))
		ret = -EINVAL;

	if (ret < 0) {
		dev_err(dev,
			"Error while parsing '%s: adi,gpio-settings': %d\n",
			np->name, ret);
		return ret;
	}

	pidx = 0;
	for (i = 0; i < gpio_cnt; i++) {
		if (!lctl_gpio_value_valid(sett->gpio_values[i])) {
			dev_err(dev,
				"Invalid setting (%u) for '%s:adi,gpio-settings[%d]'\n",
				sett->gpio_values[i], np->name, i);
			return -EINVAL;
		}
		pidx += snprintf(&pbuf[pidx], sizeof(pbuf) - pidx, "%u,",
				 sett->gpio_values[i]);
	}

	dev_dbg(dev, " * gpio settings: %s\n", pbuf);

	return 0;
}

static int ad9361_parse_setting(struct device *dev,
				struct device_node *np,
				struct ad9361_ext_band_ctl *ctl,
				struct ad9361_band_setting *sett)
{
	int ret;

	ret = ad9361_parse_gpio_settings(dev, np, ctl, sett);
	if (ret < 0)
		return ret;

	return 0;
}

static int ad9361_parse_setting_with_freq_range(struct device *dev,
				   struct device_node *np,
				   struct ad9361_ext_band_ctl *ctl,
				   struct ad9361_band_setting *sett)
{
	int ret;

	ret = of_property_read_u64(np, "adi,lo-freq-min", &sett->freq_min);
	if (ret < 0) {
		dev_err(dev, "Error while parsing '%s:adi,lo-freq-min':%d\n",
			np->name, ret);
		return ret;
	}

	ret = of_property_read_u64(np, "adi,lo-freq-max", &sett->freq_max);
	if (ret < 0) {
		dev_err(dev, "Error while parsing '%s:adi,lo-freq-max':%d\n",
			np->name, ret);
		return ret;
	}

	dev_dbg(dev, " * frequency range %llu - %llu\n",
		 sett->freq_min, sett->freq_max);

	return ad9361_parse_setting(dev, np, ctl, sett);
}

static int ad9361_scan_ctl_settings(struct device *dev,
				    const char *type,
				    int max_settings,
				    struct ad9361_ext_band_ctl *ctl,
				    struct ad9361_band_setting *sett)
{
	struct device_node *np = dev->of_node;
	struct device_node *child;
	char pnamebuf[64];
	int cnt = 0;
	int ret;

	if (!np)
		return 0;

	for (cnt = 0; cnt < max_settings; cnt++) {
		snprintf(pnamebuf, sizeof(pnamebuf), "%s%d", type, cnt);
		child = of_get_child_by_name(np, pnamebuf);
		if (!child)
			break;
		/* if settings ptr is null, then we are just counting */
		if (!sett)
			continue;

		dev_dbg(dev, "Found '%s'\n", child->name);
		ret = ad9361_parse_setting_with_freq_range(dev, child,
						ctl, &sett[cnt]);
		if (ret < 0) {
			dev_err(dev, "Error while parsing '%s': %d\n",
				child->name, ret);
			return ret;
		}
	}

	return cnt;
}

static int ad9361_populate_settings(struct device *dev,
				    struct ad9361_ext_band_ctl *ctl,
				    const char *type,
				    struct ad9361_band_setting **sett)
{
	int ret, cnt;

	/* Count RX/TX settings first */
	cnt = ad9361_scan_ctl_settings(dev, type, MAX_CTRL_SETTINGS,
				       NULL, NULL);
	if (cnt <= 0)
		return cnt;

	*sett = devm_kzalloc(dev,
			     sizeof(struct ad9361_band_setting) * (cnt + 1),
			     GFP_KERNEL);
	if (!*sett)
		return -ENOMEM;

	ret = ad9361_scan_ctl_settings(dev, type, cnt, ctl, *sett);
	if (ret < 0)
		return ret;

	if (ret != cnt)
		return -EINVAL;

	return 0;
}

static int ad9361_populate_hooks(struct device *dev,
				 struct ad9361_ext_band_ctl *ctl)
{
	static const char *map[__MAX_CTL_HOOKS] = {
		[CTL_INIT]	= "adi_ext_band_ctl_init",
		[CTL_UNINIT]	= "adi_ext_band_ctl_uninit",
	};
	struct device_node *np = dev->of_node;
	struct device_node *child;
	int i, ret;

	for (i = 0; i < __MAX_CTL_HOOKS; i++) {
		child = of_get_child_by_name(np, map[i]);
		if (!child)
			continue;

		ctl->hooks[i] = devm_kzalloc(dev, sizeof(*ctl->hooks[i]),
					     GFP_KERNEL);
		if (!ctl->hooks[i])
			return -ENOMEM;

		ret = ad9361_parse_setting(dev, child, ctl, ctl->hooks[i]);
		if (ret < 0)
			return ret;
	}
	return 0;
}

int ad9361_register_ext_band_control(struct ad9361_rf_phy *phy)
{
	struct device *dev = &phy->spi->dev;
	struct ad9361_ext_band_ctl *ctl;
	int ret;

	ctl = devm_kzalloc(dev, sizeof(*phy->ext_band_ctl), GFP_KERNEL);
	if (!ctl)
		return -ENOMEM;

	ret = ad9361_populate_objs(dev, &ctl->objs);
	if (ret <= 0) {
		if (ret == 0)
			dev_info(dev, "No GPIOs defined for ext band ctrl\n");
		return ret;
	}

	ret = ad9361_populate_hooks(dev, ctl);
	if (ret < 0)
		return ret;

	if (ctl->hooks[CTL_INIT]) {
		ret = ad9361_apply_settings(phy, ctl->hooks[CTL_INIT], NULL);
		if (ret == 0) {
			ctl->rx_curr_setting = ctl->hooks[CTL_INIT];
			ctl->tx_curr_setting = ctl->hooks[CTL_INIT];
		}
	}
	if (ret < 0)
		return ret;

	ret = ad9361_populate_settings(dev, ctl, "adi_rx_band_setting_",
				       &ctl->rx_settings);
	if (ret < 0)
		return ret;

	ret = ad9361_populate_settings(dev, ctl, "adi_tx_band_setting_",
				       &ctl->tx_settings);
	if (ret < 0)
		return ret;

	phy->ext_band_ctl = ctl;

	return 0;
}

void ad9361_unregister_ext_band_control(struct ad9361_rf_phy *phy)
{
	struct ad9361_ext_band_ctl *ctl = phy->ext_band_ctl;

	if (!ctl || !ctl->hooks[CTL_UNINIT])
		return;

	ad9361_apply_settings(phy, ctl->hooks[CTL_UNINIT], NULL);
}

static struct ad9361_band_setting *ad9361_find_first_setting(
		struct ad9361_band_setting *settings, u64 freq)
{
	struct ad9361_band_setting *sett;
	int i;

	if (!settings)
		return NULL;

	for (i = 0; (sett = &settings[i]); i++) {
		if (sett->freq_min <= freq && freq < sett->freq_max)
			return sett;
	}

	return NULL;
}

static int ad9361_apply_gpio_settings(struct device *dev,
				      struct ad9361_band_setting *new_sett,
				      struct ad9361_band_setting *curr_sett)
{
	struct gpio_descs *gpios;
	int i, ret;

	if (!new_sett->gpio_values) /* NOP */
		return 0;
	gpios = new_sett->objs->gpios;

	/* FIXME: try to use gpiod_set_array_value_complex() or similar asap
	 * to set GPIOs all at once. But with the current one it does not seem
	 * to be straightforward to switch between in/out-low/out-high.
	 */
	for (i = 0; i < gpios->ndescs; i++) {
		/* If values are the same as previous setting, skip */
		if (curr_sett && curr_sett->gpio_values &&
		    new_sett->gpio_values[i] == curr_sett->gpio_values[i])
			continue;
		switch (new_sett->gpio_values[i]) {
		case CTL_GPIO_IN:
			ret = gpiod_direction_input(gpios->desc[i]);
			break;
		case CTL_GPIO_OUT_LOW:
			ret = gpiod_direction_output_raw(gpios->desc[i], 0);
			break;
		case CTL_GPIO_OUT_HIGH:
			ret = gpiod_direction_output_raw(gpios->desc[i], 1);
			break;
		default:
			continue;
		}
		if (ret < 0) {
			dev_err(dev, "%s: err when setting GPIO(%d) val %d\n",
				__func__, i, ret);
			return ret;
		}
		dev_dbg(dev, "%s: GPIO(%d) set to %d\n", __func__, i,
			new_sett->gpio_values[i]);
	}

	return 0;
}

static int ad9361_apply_settings(struct ad9361_rf_phy *phy,
				 struct ad9361_band_setting *new_sett,
				 struct ad9361_band_setting **curr_sett)
{
	struct ad9361_band_setting *lcurr_sett;
	struct device *dev = &phy->spi->dev;
	int ret;

	if (curr_sett) {
		lcurr_sett = *curr_sett;
	} else {
		lcurr_sett = NULL;
		curr_sett = &lcurr_sett;
	}

	dev_dbg(dev, "%s: Applying setting '%s'\n", __func__,
		new_sett->name);

	ret = ad9361_apply_gpio_settings(dev, new_sett, lcurr_sett);
	if (ret < 0)
		return ret;

	*curr_sett = new_sett;

	dev_dbg(dev, "%s: Applied setting '%s'\n", __func__,
		new_sett->name);

	return 0;
}

int ad9361_adjust_rx_ext_band_settings(struct ad9361_rf_phy *phy, u64 freq)
{
	struct ad9361_band_setting *sett;
	struct ad9361_ext_band_ctl *ctl;
	int ret;

	if (!phy)
		return -EINVAL;

	if (!phy->ext_band_ctl)
		return 0;
	ctl = phy->ext_band_ctl;

	sett = ad9361_find_first_setting(ctl->rx_settings, freq);
	if (!sett)
		return 0;

	/* Silently exit, if the same setting */
	if (ctl->rx_curr_setting == sett)
		return 0;

	ret = ad9361_apply_settings(phy, sett, &ctl->rx_curr_setting);
	if (ret < 0)
		return ret;

	return 0;
}

int ad9361_adjust_tx_ext_band_settings(struct ad9361_rf_phy *phy, u64 freq)
{
	struct ad9361_band_setting *sett;
	struct ad9361_ext_band_ctl *ctl;
	int ret;

	if (!phy)
		return -EINVAL;

	if (!phy->ext_band_ctl)
		return 0;
	ctl = phy->ext_band_ctl;

	sett = ad9361_find_first_setting(ctl->tx_settings, freq);
	if (!sett)
		return 0;

	/* Silently exit, if the same setting */
	if (ctl->tx_curr_setting == sett)
		return 0;

	ret = ad9361_apply_settings(phy, sett, &ctl->tx_curr_setting);
	if (ret < 0)
		return ret;

	return 0;
}
