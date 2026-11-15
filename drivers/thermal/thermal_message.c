// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi /sys/class/thermal/thermal_message node (used by mi_thermald and
 * init.target.rc), split out of the msm-5.4 haydn thermal_core.c.
 */
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thermal.h>

#include "thermal_core.h"

#define CPU_LIMITS_PARAM_NUM	2

struct device thermal_message_dev;
EXPORT_SYMBOL_GPL(thermal_message_dev);
static atomic_t switch_mode = ATOMIC_INIT(-1);
static atomic_t balance_mode = ATOMIC_INIT(0);
static atomic_t temp_state = ATOMIC_INIT(0);
static atomic_t charger_mode = ATOMIC_INIT(-1);
static atomic_t modem_limit = ATOMIC_INIT(0);
static atomic_t market_download_limit = ATOMIC_INIT(0);
static atomic_t flash_state = ATOMIC_INIT(0);
static atomic_t wifi_limit = ATOMIC_INIT(0);
static atomic_t poor_modem_limit = ATOMIC_INIT(0);
static char boost_buf[128];
const char *board_sensor;
static char board_sensor_temp[128];
static char board_sensor_second_temp[128];

/*
 * msm-5.4 Xiaomi cpu_cooling exported cpu_limits_set_level(); on 5.10 the
 * same cap is a per-policy max frequency QoS request.
 */
static DEFINE_PER_CPU(struct freq_qos_request *, mi_cpu_limit_req);

static void cpu_limits_set_level(unsigned int cpu, unsigned int max_freq)
{
	struct cpufreq_policy *policy;
	struct freq_qos_request *req;

	if (cpu >= nr_cpu_ids || !cpu_possible(cpu))
		return;

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return;

	req = per_cpu(mi_cpu_limit_req, policy->cpu);
	if (!req) {
		req = kzalloc(sizeof(*req), GFP_KERNEL);
		if (!req)
			goto out;
		if (freq_qos_add_request(&policy->constraints, req,
					 FREQ_QOS_MAX, max_freq) < 0) {
			kfree(req);
			goto out;
		}
		per_cpu(mi_cpu_limit_req, policy->cpu) = req;
	} else {
		freq_qos_update_request(req, max_freq);
	}
out:
	cpufreq_cpu_put(policy);
}

static int of_parse_thermal_message(void)
{
	struct device_node *np;

	np = of_find_node_by_name(NULL, "thermal-message");
	if (!np)
		return -EINVAL;

	if (of_property_read_string(np, "board-sensor", &board_sensor))
		return -EINVAL;

	pr_info("%s board sensor: %s\n", __func__, board_sensor);

	return 0;
}

static ssize_t
thermal_sconfig_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&switch_mode));
}

static ssize_t
thermal_sconfig_store(struct device *dev,
				      struct device_attribute *attr, const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&switch_mode, val);

	return len;
}

static DEVICE_ATTR(sconfig, 0664,
		   thermal_sconfig_show, thermal_sconfig_store);

static ssize_t
thermal_charger_temp_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&charger_mode));
}

static ssize_t
thermal_charger_temp_store(struct device *dev,
				      struct device_attribute *attr, const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&charger_mode, val);

	return len;
}

static DEVICE_ATTR(charger_temp, 0664,
		   thermal_charger_temp_show, thermal_charger_temp_store);

static ssize_t
thermal_balance_mode_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&balance_mode));
}

static ssize_t
thermal_balance_mode_store(struct device *dev,
				      struct device_attribute *attr, const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&balance_mode, val);

	return len;
}

static DEVICE_ATTR(balance_mode, 0664,
		   thermal_balance_mode_show, thermal_balance_mode_store);

static ssize_t
thermal_boost_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, boost_buf);
}

static ssize_t
thermal_boost_store(struct device *dev,
				      struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	ret = snprintf(boost_buf, sizeof(boost_buf), "%s", buf);
	return len;
}

static DEVICE_ATTR(boost, 0644,
		   thermal_boost_show, thermal_boost_store);

static ssize_t
thermal_temp_state_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&temp_state));
}

static ssize_t
thermal_temp_state_store(struct device *dev,
				      struct device_attribute *attr, const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&temp_state, val);

	return len;
}

static DEVICE_ATTR(temp_state, 0664,
		   thermal_temp_state_show, thermal_temp_state_store);

static ssize_t
cpu_limits_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t
cpu_limits_store(struct device *dev,
				      struct device_attribute *attr, const char *buf, size_t len)
{
	unsigned int cpu;
	unsigned int max;

	if (sscanf(buf, "cpu%u %u", &cpu, &max) != CPU_LIMITS_PARAM_NUM) {
		pr_err("input param error, can not prase param\n");
		return -EINVAL;
	}

	cpu_limits_set_level(cpu, max);

	return len;
}

static DEVICE_ATTR(cpu_limits, 0664,
		   cpu_limits_show, cpu_limits_store);

static ssize_t
thermal_board_sensor_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	if (!board_sensor)
		board_sensor = "invalid";

	return snprintf(buf, PAGE_SIZE, "%s", board_sensor);
}

static DEVICE_ATTR(board_sensor, 0664,
		thermal_board_sensor_show, NULL);

static ssize_t
thermal_board_sensor_temp_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, board_sensor_temp);
}

static ssize_t
thermal_board_sensor_temp_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	snprintf(board_sensor_temp, sizeof(board_sensor_temp), "%s", buf);

	return len;
}

static DEVICE_ATTR(board_sensor_temp, 0664,
		thermal_board_sensor_temp_show, thermal_board_sensor_temp_store);

static ssize_t
thermal_board_sensor_second_temp_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, board_sensor_second_temp);
}

static ssize_t
thermal_board_sensor_second_temp_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	snprintf(board_sensor_second_temp, sizeof(board_sensor_second_temp), "%s", buf);

	return len;
}

static DEVICE_ATTR(board_sensor_second_temp, 0664,
		thermal_board_sensor_second_temp_show, thermal_board_sensor_second_temp_store);

static ssize_t
thermal_modem_limit_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&modem_limit));
}
static ssize_t
thermal_modem_limit_store(struct device *dev,
				      struct device_attribute *attr, const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&modem_limit, val);
	return len;
}

static DEVICE_ATTR(modem_limit, 0664,
	   thermal_modem_limit_show, thermal_modem_limit_store);


static ssize_t
thermal_market_download_limit_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&market_download_limit));
}
static ssize_t
thermal_market_download_limit_store(struct device *dev,
				      struct device_attribute *attr, const char *buf, size_t len)
{
	int val = -1;

	val = simple_strtol(buf, NULL, 10);

	atomic_set(&market_download_limit, val);
	return len;
}

static DEVICE_ATTR(market_download_limit, 0664,
	   thermal_market_download_limit_show, thermal_market_download_limit_store);

static ssize_t
thermal_flash_state_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&flash_state));
}
static ssize_t
thermal_flash_state_store(struct device *dev,
				      struct device_attribute *attr, const char *buf, size_t len)
{
	int val = -1;
	val = simple_strtol(buf, NULL, 10);
	atomic_set(&flash_state, val);
	return len;
}

static DEVICE_ATTR(flash_state, 0664,
           thermal_flash_state_show, thermal_flash_state_store);

static ssize_t
thermal_wifi_limit_show(struct device *dev,
                                      struct device_attribute *attr, char *buf)
{
        return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&wifi_limit));
}
static ssize_t
thermal_wifi_limit_store(struct device *dev,
                                      struct device_attribute *attr, const char *buf, size_t len)
{
        int val = -1;
        val = simple_strtol(buf, NULL, 10);
        atomic_set(&wifi_limit, val);
        return len;
}

static DEVICE_ATTR(wifi_limit, 0664,
	   thermal_wifi_limit_show, thermal_wifi_limit_store);

static ssize_t
thermal_poor_modem_limit_show(struct device *dev,
                                      struct device_attribute *attr, char *buf)
{
        return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&poor_modem_limit));
}
static ssize_t
thermal_poor_modem_limit_store(struct device *dev,
                                      struct device_attribute *attr, const char *buf, size_t len)
{
        int val = -1;
        val = simple_strtol(buf, NULL, 10);
        atomic_set(&poor_modem_limit, val);
        return len;
}

static DEVICE_ATTR(poor_modem_limit, 0664,
           thermal_poor_modem_limit_show, thermal_poor_modem_limit_store);

int __init thermal_message_node_create(void)
{
	int ret = 0;

	if (of_parse_thermal_message())
		pr_warn("Thermal: thermal-message board-sensor not found in DT\n");

	thermal_message_dev.class = &thermal_class;

	dev_set_name(&thermal_message_dev, "thermal_message");
	ret = device_register(&thermal_message_dev);
	if (!ret) {
		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_sconfig.attr);
		if (ret < 0)
			pr_warn("Thermal: create sconfig node failed\n");

		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_balance_mode.attr);
		if (ret < 0)
			pr_warn("Thermal: create balance mode node failed\n");

		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_boost.attr);
		if (ret < 0)
			pr_warn("Thermal: create boost node failed\n");

		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_temp_state.attr);
		if (ret < 0)
			pr_warn("Thermal: create temp state node failed\n");

		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_cpu_limits.attr);
		if (ret < 0)
			pr_warn("Thermal: create cpu limits node failed\n");

		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_board_sensor.attr);
		if (ret < 0)
			pr_warn("Thermal: create board sensor node failed\n");

		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_board_sensor_temp.attr);
		if (ret < 0)
			pr_warn("Thermal: create board sensor temp node failed\n");

		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_board_sensor_second_temp.attr);
		if (ret < 0)
			pr_warn("Thermal: create board sensor second temp node failed\n");

		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_charger_temp.attr);
		if (ret < 0)
			pr_warn("Thermal: create charger temp node failed\n");
		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_modem_limit.attr);
		if (ret < 0)
			pr_warn("Thermal: create modem limit node failed\n");
		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_market_download_limit.attr);
		if (ret < 0)
			pr_warn("Thermal: create market download limit node failed\n");
		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_flash_state.attr);
		if (ret < 0)
                        pr_warn("Thermal: create flash state node failed\n");
		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_wifi_limit.attr);
		if (ret < 0)
                        pr_warn("Thermal: create wifi limit node failed\n");
		ret = sysfs_create_file(&thermal_message_dev.kobj, &dev_attr_poor_modem_limit.attr);
                if (ret < 0)
                        pr_warn("Thermal: create poor modem limit node failed\n");
	}
	return ret;
}

void thermal_message_node_destroy(void)
{
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_poor_modem_limit.attr);
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_wifi_limit.attr);
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_flash_state.attr);
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_market_download_limit.attr);
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_charger_temp.attr);
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_modem_limit.attr);
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_board_sensor_temp.attr);
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_board_sensor_second_temp.attr);
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_board_sensor.attr);
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_cpu_limits.attr);
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_temp_state.attr);
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_boost.attr);
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_sconfig.attr);
	sysfs_remove_file(&thermal_message_dev.kobj, &dev_attr_balance_mode.attr);
	device_unregister(&thermal_message_dev);
}
