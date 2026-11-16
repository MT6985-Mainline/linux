// SPDX-License-Identifier: GPL-2.0
/* MT6985 trusted-memory allocation frequency hints using Linux frequency QoS. */
#include <linux/cpufreq.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include "private/tmem_cpu_boost.h"

struct tmem_cpu_boost {
	unsigned int cpu;
	unsigned int frequency;
	unsigned int users;
	struct cpufreq_policy *policy;
	struct freq_qos_request request;
};
/* MiCode's MT6985 hints request 2 GHz little/super and 2.4 GHz big cores. */
static struct tmem_cpu_boost tmem_boost[] = {
	[CPU_SUPER_GROUP] = { .cpu = 7, .frequency = 2000000 },
	[CPU_BIG_GROUP] = { .cpu = 4, .frequency = 2400000 },
	[CPU_LITTLE_GROUP] = { .cpu = 0, .frequency = 2000000 },
};
static DEFINE_MUTEX(tmem_boost_lock);

int tmem_cpu_group_boost(enum tmem_cpu_group group, bool enable)
{
	struct tmem_cpu_boost *boost;
	int ret = 0;

	if (group < CPU_SUPER_GROUP || group > CPU_LITTLE_GROUP)
		return -EINVAL;
	boost = &tmem_boost[group];
	mutex_lock(&tmem_boost_lock);
	if (enable) {
		if (boost->users++) {
			ret = boost->policy ? 0 : -ENODEV;
			goto unlock;
		}
		boost->policy = cpufreq_cpu_get(boost->cpu);
		if (!boost->policy) {
			ret = -ENODEV;
			goto unlock;
		}
		ret = freq_qos_add_request(&boost->policy->constraints,
					  &boost->request, FREQ_QOS_MIN,
					  min(boost->frequency,
					      boost->policy->cpuinfo.max_freq));
		if (ret < 0) {
			cpufreq_cpu_put(boost->policy);
			boost->policy = NULL;
		} else {
			ret = 0;
		}
	} else if (!boost->users) {
		ret = -EINVAL;
	} else if (!--boost->users && boost->policy) {
		ret = freq_qos_remove_request(&boost->request);
		cpufreq_cpu_put(boost->policy);
		boost->policy = NULL;
	}
unlock:
	mutex_unlock(&tmem_boost_lock);
	return ret;
}
