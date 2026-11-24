/* SPDX-License-Identifier: GPL-2.0-only */
/* of_get_named_gpio for the Spectra camera drivers (removed from this kernel): the legacy GPIO number. */
#ifndef _Q2CAM_OF_GPIO_H
#define _Q2CAM_OF_GPIO_H

#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/string.h>

static inline int of_get_named_gpio(const struct device_node *np, const char *name, int index)
{
	char con[64];
	size_t n;
	struct gpio_desc *d;
	int gpio;

	strscpy(con, name, sizeof(con));
	n = strlen(con);
	if (n > 6 && !strcmp(con + n - 6, "-gpios"))
		con[n - 6] = 0;
	else if (n > 5 && !strcmp(con + n - 5, "-gpio"))
		con[n - 5] = 0;
	d = fwnode_gpiod_get_index(of_fwnode_handle(np), strcmp(con, "gpios") ? con : NULL, index,
				   GPIOD_ASIS, "q2cam");
	if (IS_ERR(d))
		return PTR_ERR(d);
	gpio = desc_to_gpio(d);
	gpiod_put(d);
	return gpio;
}

#define of_get_gpio(np, index) of_get_named_gpio(np, "gpios", index)

static inline int of_gpio_named_count(const struct device_node *np, const char *name)
{
	return of_count_phandle_with_args(np, name, "#gpio-cells");
}

#define of_gpio_count(np) of_gpio_named_count(np, "gpios")

#endif
