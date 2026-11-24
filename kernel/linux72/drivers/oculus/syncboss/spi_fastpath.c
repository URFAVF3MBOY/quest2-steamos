// SPDX-License-Identifier: GPL-2.0
#include "spi_fastpath.h"

/* Not used on this kernel: transfers go through the SPI core (see syncboss_spi.c). */
int spi_fastpath_transfer(struct spi_device *spi, struct spi_message *msg)
{
	return -EOPNOTSUPP;
}
