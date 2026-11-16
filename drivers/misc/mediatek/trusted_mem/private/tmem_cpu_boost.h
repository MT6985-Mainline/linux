/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TMEM_CPU_BOOST_H
#define TMEM_CPU_BOOST_H
#include <linux/types.h>
enum tmem_cpu_group {
	CPU_SUPER_GROUP = 1,
	CPU_BIG_GROUP = 2,
	CPU_LITTLE_GROUP = 3,
};
int tmem_cpu_group_boost(enum tmem_cpu_group group, bool enable);
#endif
