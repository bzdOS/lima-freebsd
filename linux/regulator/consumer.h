/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * linux/regulator/consumer.h — voltage regulator stub for Lima / FreeBSD 15.1
 *
 * No regulator framework in linuxkpi.  All no-ops; on the Allwinner A64
 * (PinePhone, Banana Pi M64) the GPU VDD is a fixed-factor regulator managed by
 * the PMIC driver at boot.
 */

#ifndef _LIMA_LINUX_REGULATOR_CONSUMER_H_
#define _LIMA_LINUX_REGULATOR_CONSUMER_H_

#include <linux/compiler.h>
#include <linux/stddef.h>
#include <linux/errno.h>
#include <linux/err.h>

struct regulator;
struct device;

static inline struct regulator *
devm_regulator_get_optional(struct device *dev, const char *id)
{
	return ERR_PTR(-ENODEV);
}

static inline struct regulator *
devm_regulator_get(struct device *dev, const char *id)
{
	return ERR_PTR(-ENODEV);
}

static inline int regulator_enable(struct regulator *reg)
{
	return 0;
}

static inline void regulator_disable(struct regulator *reg)
{
}

static inline int regulator_get_voltage(struct regulator *reg)
{
	return 0;
}

#endif /* _LIMA_LINUX_REGULATOR_CONSUMER_H_ */
