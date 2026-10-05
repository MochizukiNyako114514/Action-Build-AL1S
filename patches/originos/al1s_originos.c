// SPDX-License-Identifier: GPL-2.0-only
/* AL1S: OriginOS compatibility ABI, implemented inside the OnePlus 13 msm_drm.
 * No SELinux hooks, scheduler changes, or fingerprint authentication emulation.
 * Display capabilities/state mirror the inspected 13T compatibility ABI;
 * SRE/ORE/ESD fields are compatibility state, not new panel implementations.
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include "../../msm/dsi/dsi_display.h"
#include "oplus_onscreenfingerprint.h"
#include "oplus_display_utils.h"

void al1s_originos_init(void);
void al1s_originos_exit(void);

static struct kobject *fp_kobj, *ufs_kobj, *cpu_kobj, *lcm_kobj[2];
static struct device *soc1_dev;
static struct class *fuel_class;
static unsigned int cpu_type;
static DEFINE_MUTEX(display_lock);

static ssize_t read_metric(const char *path, char *buf)
{
	struct file *file;
	loff_t pos = 0;
	ssize_t n;

	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);
	n = kernel_read(file, buf, PAGE_SIZE - 2, &pos);
	filp_close(file, NULL);
	if (n > 0) {
		if (buf[n - 1] != '\n')
			buf[n++] = '\n';
		buf[n] = '\0';
	}
	return n;
}

static ssize_t fp_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "ultrasonic_fake_nyako\n");
}
static ssize_t ufs_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return read_metric("/sys/devices/soc0/serial_number", buf);
}
static struct kobj_attribute fp_attr = __ATTR(fp_id, 0444, fp_show, NULL);
static struct kobj_attribute ufs_attr = __ATTR(ufsid, 0444, ufs_show, NULL);
static struct attribute *fp_attrs[] = { &fp_attr.attr, NULL };
static struct attribute *ufs_attrs[] = { &ufs_attr.attr, NULL };
static const struct attribute_group fp_group = { .attrs = fp_attrs };
static const struct attribute_group ufs_group = { .attrs = ufs_attrs };

static ssize_t cpu_read(const char *name, char *buf)
{
	unsigned int type = READ_ONCE(cpu_type);

	if (!strcmp(name, "type"))
		return sysfs_emit(buf, "%u\n", type);
	if (!strcmp(name, "cpu_set") || !strcmp(name, "cpu_type"))
		return sysfs_emit(buf, "8 Elite\n");
	if (!strcmp(name, "core_num"))
		return sysfs_emit(buf, "8\n");
	if (!strcmp(name, "cpu_freq"))
		return sysfs_emit(buf, "%u\n", type ? 4470000 : 4320000);
	if (!strcmp(name, "user_cpu_freq"))
		return sysfs_emit(buf, "%s\n", type ? "4.47" : "4.32");
	return -EINVAL;
}
static ssize_t cpu_write(const char *buf, size_t count)
{
	unsigned int value;
	int ret = kstrtouint(buf, 10, &value);

	if (ret)
		return ret;
	if (value > 1)
		return -EINVAL;
	/* Only an identity table selector. Never changes CPU clocks. */
	WRITE_ONCE(cpu_type, value);
	return count;
}
static ssize_t cpu_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return cpu_read(a->attr.name, buf);
}
static ssize_t cpu_store(struct kobject *k, struct kobj_attribute *a,
			 const char *buf, size_t count)
{
	return cpu_write(buf, count);
}
#define CPU_RO(n) static struct kobj_attribute cpu_##n = __ATTR(n, 0444, cpu_show, NULL)
CPU_RO(cpu_freq);
CPU_RO(core_num);
CPU_RO(cpu_set);
CPU_RO(user_cpu_freq);
static struct kobj_attribute cpu_type_attr = __ATTR(type, 0644, cpu_show, cpu_store);
static struct attribute *cpu_attrs[] = { &cpu_type_attr.attr, &cpu_cpu_freq.attr,
	&cpu_core_num.attr, &cpu_cpu_set.attr, &cpu_user_cpu_freq.attr, NULL };
static const struct attribute_group cpu_group = { .attrs = cpu_attrs };

static ssize_t soc_show(struct device *d, struct device_attribute *a, char *buf)
{
	return cpu_read(a->attr.name, buf);
}
static ssize_t soc_store(struct device *d, struct device_attribute *a,
			 const char *buf, size_t count)
{
	return cpu_write(buf, count);
}
#define SOC_RO(n) static struct device_attribute soc_##n = __ATTR(n, 0444, soc_show, NULL)
SOC_RO(cpu_freq);
SOC_RO(core_num);
SOC_RO(cpu_type);
SOC_RO(user_cpu_freq);
static struct device_attribute soc_type = __ATTR(type, 0644, soc_show, soc_store);
static struct attribute *soc_attrs[] = { &soc_type.attr, &soc_cpu_freq.attr,
	&soc_core_num.attr, &soc_cpu_type.attr, &soc_user_cpu_freq.attr, NULL };
static const struct attribute_group soc_group = { .attrs = soc_attrs };

static ssize_t fuel_show(const struct class *c, const struct class_attribute *a,
			 char *buf)
{
	return read_metric(!strcmp(a->attr.name, "soh") ?
		"/sys/devices/virtual/oplus_chg/battery/battery_soh" :
		"/sys/devices/virtual/oplus_chg/battery/battery_cc", buf);
}
static struct class_attribute fuel_soh = __ATTR(soh, 0444, fuel_show, NULL);
static struct class_attribute fuel_cycle = __ATTR(cycle, 0444, fuel_show, NULL);

enum display_id {
	AOD_INFO, PARTIAL_AOD, FULL_AOD, AOD_1HZ, FP_1HZ,
	SRE, SRE_1, SRE1, SRE1_1, ELEC_SRE, LCM_SRE, ORE, ORE1,
	PANEL_INFO, PANEL_NAME, PANEL_ID, LCM_ID, ERROR_INFO,
	ESD_PS, ESD_ENABLE, SRE_MAX, LCM_SRE1, ELEC_SRE1, DISPLAY_COUNT
};
static const char * const display_names[] = {
	"aod_disp_info", "partial_aod_show", "full_aod_on", "aod_1hz_enable",
	"fp_1hz_enable", "sre_enable", "sre_enable1", "sre1_enable", "sre1_enable1",
	"elec_sre", "lcm_sre", "oled_ore", "oled_ore1", "panel_info", "panel_name",
	"panel_id", "lcm_id", "error_info", "vivo_esd_check_ps", "vivo_esd_check_enable",
	"lcm_sre_max_level", "lcm_sre1", "elec_sre1"
};
static unsigned int display_values[DISPLAY_COUNT];
static unsigned int support_1hz = 2, support_video = 1, support_full = 3;

static int display_index(const char *name)
{
	int i;
	for (i = 0; i < DISPLAY_COUNT; i++)
		if (!strcmp(name, display_names[i]))
			return i;
	return -EINVAL;
}

static ssize_t display_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	int id = display_index(a->attr.name);
	unsigned int value = 0;
	ssize_t ret;

	if (id < 0)
		return id;
	mutex_lock(&display_lock);
	switch (id) {
	case AOD_INFO:
		ret = sysfs_emit(buf, "support_1hz=%u,support_video_aod=%u,support_full_screen=%u\n",
				support_1hz, support_video, support_full);
		goto out;
	case PANEL_INFO:
		ret = sysfs_emit(buf, "support_full_screen=%u\n", support_full);
		goto out;
	case PANEL_NAME:
		ret = sysfs_emit(buf, "panel_name=unknown\n");
		goto out;
	case PANEL_ID: case LCM_ID:
		ret = sysfs_emit(buf, "0x0\n");
		goto out;
	case PARTIAL_AOD:
		value = oplus_ofp_get_aod_state() && !oplus_ofp_full_screen_aod_mode_is_enabled();
		break;
	case FULL_AOD:
		value = oplus_ofp_full_screen_aod_mode_is_enabled();
		break;
	case FP_1HZ:
		if (oplus_ofp_get_ultra_low_power_aod_mode(&value))
			value = display_values[id];
		break;
	default:
		value = display_values[id];
	}
	ret = sysfs_emit(buf, "%u\n", value);
out:
	mutex_unlock(&display_lock);
	return ret;
}

/* Called with display_lock held; the display functions keep their own locks. */
static int display_set(int id, unsigned int value)
{
	struct dsi_display *display;
	unsigned int mode;
	int ret;

	if (id == PARTIAL_AOD || id == FULL_AOD) {
		if (value > 1)
			return -EINVAL;
		display = oplus_display_get_current_display();
		if (!display || !display->panel)
			return -ENODEV;
		mode = value ? (id == FULL_AOD ? 5 : 1) : 0;
		ret = oplus_ofp_set_longrui_aod_mode(&mode);
		if (ret)
			return ret;
		if (!value && oplus_ofp_get_aod_state()) {
			ret = oplus_ofp_aod_off_handle(display);
			if (ret)
				return ret;
			if (oplus_ofp_get_aod_state())
				return -EIO;
		}
		display_values[id] = value;
		if (value)
			display_values[id == FULL_AOD ? PARTIAL_AOD : FULL_AOD] = 0;
		return 0;
	}
	if (id == AOD_1HZ) {
		if (value > 1)
			return -EINVAL;
		if (value)
			return -EOPNOTSUPP; /* Same limitation as the inspected 13T adapter. */
	} else if (id == FP_1HZ) {
		if (value > 1)
			return -EINVAL;
		ret = oplus_ofp_set_ultra_low_power_aod_mode(&value);
		if (ret)
			return ret;
	} else if (id >= SRE && id <= SRE1_1) {
		if (value > 1)
			return -EINVAL;
	} else if (id == ESD_PS || id == ESD_ENABLE) {
		if (value > 1)
			return -EINVAL;
	} else if ((id >= ELEC_SRE && id <= ORE1) || id >= SRE_MAX) {
		if (value > 100)
			return -EINVAL;
	} else {
		return -EINVAL;
	}
	display_values[id] = value;
	return 0;
}

static ssize_t display_store(struct kobject *k, struct kobj_attribute *a,
			     const char *buf, size_t count)
{
	int id = display_index(a->attr.name), ret;
	unsigned int value;
	char *copy = NULL, *key, *arg;

	if (id < 0)
		return id;
	if (!count || count > 256)
		return -EINVAL;
	mutex_lock(&display_lock);
	if (id == AOD_INFO || id == PANEL_INFO) {
		copy = kstrndup(buf, count, GFP_KERNEL);
		if (!copy) { ret = -ENOMEM; goto out; }
		arg = copy;
		key = strsep(&arg, "=");
		if (!arg) { ret = -EINVAL; goto out; }
		key = strim(key);
		ret = kstrtouint(strim(arg), 0, &value);
		if (ret)
			goto out;
		if (!strcmp(key, "support_1hz") && value <= 2) {
			support_1hz = value; ret = 0;
		} else if (!strcmp(key, "support_video_aod") && value <= 1) {
			support_video = value; ret = 0;
		} else if (!strcmp(key, "support_full_screen") && value <= 3) {
			support_full = value; ret = 0;
		} else {
			id = display_index(key);
			ret = id < 0 ? id : display_set(id, value);
		}
	} else {
		ret = kstrtouint(buf, 0, &value);
		if (!ret)
			ret = display_set(id, value);
	}
out:
	kfree(copy);
	mutex_unlock(&display_lock);
	return ret ? ret : count;
}

static struct kobj_attribute display_attrs[DISPLAY_COUNT];
static struct attribute *display_attr_ptrs[DISPLAY_COUNT + 1];
static const struct attribute_group display_group = { .attrs = display_attr_ptrs };

static int make_root_group(const char *name, struct kobject **out,
			   const struct attribute_group *group)
{
	int ret;
	/* Explicit NULL parent: unlike early_initcall, kernel_kobj now exists. */
	*out = kobject_create_and_add(name, NULL);
	if (!*out)
		return -ENOMEM;
	ret = sysfs_create_group(*out, group);
	if (ret) {
		kobject_put(*out);
		*out = NULL;
	}
	return ret;
}

void al1s_originos_exit(void)
{
	int i;
	for (i = 0; i < 2; i++) {
		if (lcm_kobj[i]) {
			sysfs_remove_group(lcm_kobj[i], &display_group);
			kobject_put(lcm_kobj[i]); lcm_kobj[i] = NULL;
		}
	}
	if (fuel_class) {
		class_remove_file(fuel_class, &fuel_soh);
		class_remove_file(fuel_class, &fuel_cycle);
		class_destroy(fuel_class); fuel_class = NULL;
	}
	if (soc1_dev) {
		sysfs_remove_group(&soc1_dev->kobj, &soc_group);
		root_device_unregister(soc1_dev); soc1_dev = NULL;
	}
#define DROP_ROOT(k, g) do { if (k) { sysfs_remove_group(k, &g); kobject_put(k); k = NULL; } } while (0)
	DROP_ROOT(cpu_kobj, cpu_group);
	DROP_ROOT(ufs_kobj, ufs_group);
	DROP_ROOT(fp_kobj, fp_group);
}

void al1s_originos_init(void)
{
	int ret, i;

	ret = make_root_group("fp_id", &fp_kobj, &fp_group);
	if (ret) goto fail;
	ret = make_root_group("ufs", &ufs_kobj, &ufs_group);
	if (ret) goto fail;
	ret = make_root_group("cpu_info", &cpu_kobj, &cpu_group);
	if (ret) goto fail;
	soc1_dev = root_device_register("soc1");
	if (IS_ERR(soc1_dev)) { ret = PTR_ERR(soc1_dev); soc1_dev = NULL; goto fail; }
	ret = sysfs_create_group(&soc1_dev->kobj, &soc_group);
	if (ret) goto fail;
	fuel_class = class_create("fuelsummary");
	if (IS_ERR(fuel_class)) { ret = PTR_ERR(fuel_class); fuel_class = NULL; goto fail; }
	ret = class_create_file(fuel_class, &fuel_soh);
	if (ret) goto fail;
	ret = class_create_file(fuel_class, &fuel_cycle);
	if (ret) goto fail;
	for (i = 0; i < DISPLAY_COUNT; i++) {
		bool ro = i >= PANEL_NAME && i <= ERROR_INFO;
		sysfs_attr_init(&display_attrs[i].attr);
		display_attrs[i].attr.name = display_names[i];
		display_attrs[i].attr.mode = ro ? 0444 : 0644;
		display_attrs[i].show = display_show;
		display_attrs[i].store = ro ? NULL : display_store;
		display_attr_ptrs[i] = &display_attrs[i].attr;
	}
	ret = make_root_group("lcm", &lcm_kobj[0], &display_group);
	if (ret) goto fail;
	ret = make_root_group("lcm1", &lcm_kobj[1], &display_group);
	if (ret) goto fail;
	pr_info("AL1S OriginOS: compatibility ABI ready in msm_drm (vendor_dlkm)\n");
	return;
fail:
	/* Compatibility setup must not prevent the native display driver loading. */
	pr_err("AL1S OriginOS: compatibility ABI setup failed: %d\n", ret);
	al1s_originos_exit();
}

/* Common GKI and the vendor mixed tree use different VFS namespaces. */
MODULE_IMPORT_NS(ANDROID_GKI_VFS_EXPORT_ONLY);
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
