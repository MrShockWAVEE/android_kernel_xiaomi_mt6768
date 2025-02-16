// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/*
 *
 * (C) COPYRIGHT 2022-2024 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 */

#include <linux/of_platform.h>
#include <coresight-priv.h>
#include "sources/coresight_mali_sources.h"

/* Linux Coresight framework does not support multiple sources enabled
 * at the same time.
 *
 * To avoid Kernel instability, all Mali Coresight sources use the
 * same trace ID value as the mandatory ETM one.
 */
#define CS_MALI_TRACE_ID 0x00000010

#define CS_SCS_BASE_ADDR 0xE000E000
#define SCS_DEMCR 0xDFC
#define CS_ITM_BASE_ADDR 0xE0000000
#define ITM_TCR 0xE80
#define ITM_TCR_BUSY_BIT (0x1 << 22)
#define CS_DWT_BASE_ADDR 0xE0001000
#define DWT_CTRL 0x000
#define DWT_CYCCNT 0x004

#if KERNEL_VERSION(5, 3, 0) <= LINUX_VERSION_CODE
static char *type_name = "mali-source-itm";
#endif

#define NELEMS(s) (sizeof(s) / sizeof((s)[0]))

enum cs_itm_dwt_dynamic_regs { CS_DWT_CTRL, CS_ITM_TCR, CS_ITM_DWT_NR_DYN_REGS };

struct cs_itm_state {
	int enabled;
	u32 regs[CS_ITM_DWT_NR_DYN_REGS];
};

static struct cs_itm_state itm_state = { 0 };

static struct kbase_debug_coresight_csf_address_range dwt_itm_scs_range[] = {
	{ CS_SCS_BASE_ADDR, CS_SCS_BASE_ADDR + CORESIGHT_DEVTYPE }
};

static struct kbase_debug_coresight_csf_address_range dwt_itm_range[] = {
	{ CS_ITM_BASE_ADDR, CS_ITM_BASE_ADDR + CORESIGHT_DEVTYPE },
	{ CS_DWT_BASE_ADDR, CS_DWT_BASE_ADDR + CORESIGHT_DEVTYPE }
};

/* For ITM source, pre enable and post disable sequences are
 * defined to manipulate DEMCR.TRECNA. Clearing of this register has
 * to be done as the last step of coresight diasbling procedure and
 * only if there are no more configurations to disable. Otherwise,
 * if cleared earlier, TMC state machine gets stuck during Flush
 * procedure as clearing DEMCR.TRCENA stops ITM/ETM clocks. As a result,
 * the AFREADY signal from ITM is not received, Flush procedure never
 * ends and TMC will stay in STOPPING state.
 */
static struct kbase_debug_coresight_csf_op dwt_itm_pre_enable_ops[] = {
	// enable ITM/DWT functionality via DEMCR register
	WRITE_IMM_OP(CS_SCS_BASE_ADDR + SCS_DEMCR, 0x01000000),
};

static struct kbase_debug_coresight_csf_op dwt_itm_post_disable_ops[] = {
	// disable ITM/DWT functionality via DEMCR register
	WRITE_IMM_OP(CS_SCS_BASE_ADDR + SCS_DEMCR, 0x00000000),
};

static struct kbase_debug_coresight_csf_op dwt_itm_enable_ops[] = {
	// Unlock DWT configuration
	WRITE_IMM_OP(CS_DWT_BASE_ADDR + CORESIGHT_LAR, CS_MALI_UNLOCK_COMPONENT),
	// prep DWT counter to immediately send sync packet ((1 << 24) - 1)
	WRITE_IMM_OP(CS_DWT_BASE_ADDR + DWT_CYCCNT, 0x00ffffff),
	// Write initial value of post count counter
	WRITE_IMM_OP(CS_DWT_BASE_ADDR + DWT_CTRL, 0x00000020),
	// Set DWT configuration:
	WRITE_PTR_OP(CS_DWT_BASE_ADDR + DWT_CTRL, &itm_state.regs[CS_DWT_CTRL]),
	// Lock DWT Configuration
	WRITE_IMM_OP(CS_DWT_BASE_ADDR + CORESIGHT_LAR, 0x00000000),
	// Unlock DWT configuration
	WRITE_IMM_OP(CS_ITM_BASE_ADDR + CORESIGHT_LAR, CS_MALI_UNLOCK_COMPONENT),
	// Set ITM configuration:
	WRITE_PTR_OP(CS_ITM_BASE_ADDR + ITM_TCR, &itm_state.regs[CS_ITM_TCR]),
	// Lock DWT configuration
	WRITE_IMM_OP(CS_ITM_BASE_ADDR + CORESIGHT_LAR, 0x00000000),
	// Set enabled bit on at the end of sequence
	BIT_OR_OP(&itm_state.enabled, 0x1),
};

static struct kbase_debug_coresight_csf_op dwt_itm_disable_ops[] = {
	// Unlock ITM configuration
	WRITE_IMM_OP(CS_ITM_BASE_ADDR + CORESIGHT_LAR, CS_MALI_UNLOCK_COMPONENT),
	// Check ITM is disabled
	POLL_OP(CS_ITM_BASE_ADDR + ITM_TCR, ITM_TCR_BUSY_BIT, 0x0),
	// Lock
	WRITE_IMM_OP(CS_ITM_BASE_ADDR + CORESIGHT_LAR, 0x00000000),
	// Set enabled bit off at the end of sequence
	BIT_AND_OP(&itm_state.enabled, 0x0),
};

static void set_default_regs(void)
{
	// DWT configuration:
	// [0] = 1, enable cycle counter
	// [4:1] = 4, set PC sample rate pf 256 cycles
	// [8:5] = 1, set initial post count value
	// [9] = 1, select position of post count tap on the cycle counter
	// [10:11] = 1, enable sync packets
	// [12] = 1, enable periodic PC sample packets
	itm_state.regs[CS_DWT_CTRL] = 0x00001629;
	// ITM configuration:
	// [0] = 1, Enable ITM
	// [1] = 1, Enable Time stamp generation
	// [2] = 1, Enable sync packet transmission
	// [3] = 1, Enable HW event forwarding
	// [11:10] = 1, Generate TS request approx every 128 cycles
	// [22:16] = 1, Trace bus ID
	itm_state.regs[CS_ITM_TCR] = 0x0001040F;
}

static int verify_store_reg(struct device *dev, const char *buf, size_t count, int reg)
{
	struct coresight_mali_source_drvdata *drvdata = dev_get_drvdata(dev->parent);
	u32 val;
	int err;

	if (buf == NULL)
		return -EINVAL;

	if (itm_state.enabled == 1) {
		dev_err(drvdata->base.dev,
			"Config needs to be disabled before modifying registers\n");
		return -EINVAL;
	}

	err = kstrtou32(buf, 0, &val);
	if (err) {
		dev_err(drvdata->base.dev, "Invalid input value\n");
		return -EINVAL;
	}

	itm_state.regs[reg] = val;
	return count;
}

static ssize_t is_enabled_show(struct device *dev, struct device_attribute *attr, char *const buf)
{
	return sprintf(buf, "%d\n", itm_state.enabled);
}
static DEVICE_ATTR_RO(is_enabled);

#define CS_ITM_DWT_REG_ATTR_RW(_a, _b)                                               \
	static ssize_t _a##_show(struct device *dev, struct device_attribute *attr,  \
				 char *const buf)                                    \
	{                                                                            \
		return sprintf(buf, "%#x\n", itm_state.regs[CS_##_b]);               \
	}                                                                            \
	static ssize_t _a##_store(struct device *dev, struct device_attribute *attr, \
				  const char *buf, size_t count)                     \
	{                                                                            \
		return verify_store_reg(dev, buf, count, CS_##_b);                   \
	}                                                                            \
	static DEVICE_ATTR_RW(_a)

CS_ITM_DWT_REG_ATTR_RW(dwt_ctrl, DWT_CTRL);
CS_ITM_DWT_REG_ATTR_RW(itm_tcr, ITM_TCR);

static struct attribute *coresight_mali_source_attrs[] = {
	&dev_attr_is_enabled.attr,
	&dev_attr_dwt_ctrl.attr,
	&dev_attr_itm_tcr.attr,
	NULL,
};

static const struct attribute_group coresight_mali_source_group = {
	.attrs = coresight_mali_source_attrs,
	.name = "mgmt"
};

static const struct attribute_group *coresight_mali_source_groups[] = {
	&coresight_mali_source_group,
	NULL,
};

const struct attribute_group **coresight_mali_source_groups_get(void)
{
	return coresight_mali_source_groups;
}

int coresight_mali_sources_init_drvdata(struct coresight_mali_source_drvdata *drvdata)
{
	if (drvdata == NULL)
		return -EINVAL;

#if KERNEL_VERSION(5, 3, 0) <= LINUX_VERSION_CODE
	drvdata->type_name = type_name;
#endif

	drvdata->base.kbase_pre_post_all_client = kbase_debug_coresight_csf_register(
		drvdata->base.gpu_dev, dwt_itm_scs_range, NELEMS(dwt_itm_scs_range));
	if (drvdata->base.kbase_pre_post_all_client == NULL) {
		dev_err(drvdata->base.dev, "Registration with access to SCS failed unexpectedly\n");
		return -EINVAL;
	}

	drvdata->base.kbase_client = kbase_debug_coresight_csf_register(
		drvdata->base.gpu_dev, dwt_itm_range, NELEMS(dwt_itm_range));
	if (drvdata->base.kbase_client == NULL) {
		dev_err(drvdata->base.dev, "Registration with full range failed unexpectedly\n");
		goto kbase_pre_post_all_client_unregister;
	}

	drvdata->trcid = CS_MALI_TRACE_ID;

	drvdata->base.pre_enable_seq.ops = dwt_itm_pre_enable_ops;
	drvdata->base.pre_enable_seq.nr_ops = NELEMS(dwt_itm_pre_enable_ops);

	drvdata->base.post_disable_seq.ops = dwt_itm_post_disable_ops;
	drvdata->base.post_disable_seq.nr_ops = NELEMS(dwt_itm_post_disable_ops);

	drvdata->base.enable_seq.ops = dwt_itm_enable_ops;
	drvdata->base.enable_seq.nr_ops = NELEMS(dwt_itm_enable_ops);

	drvdata->base.disable_seq.ops = dwt_itm_disable_ops;
	drvdata->base.disable_seq.nr_ops = NELEMS(dwt_itm_disable_ops);

	set_default_regs();

	drvdata->base.pre_post_all_config = kbase_debug_coresight_csf_config_create(
		drvdata->base.kbase_pre_post_all_client, &drvdata->base.pre_enable_seq,
		&drvdata->base.post_disable_seq, true);
	if (!drvdata->base.pre_post_all_config) {
		dev_err(drvdata->base.dev, "pre_post_all_config create failed unexpectedly\n");
		goto kbase_client_unregister;
	}

	drvdata->base.config = kbase_debug_coresight_csf_config_create(drvdata->base.kbase_client,
								       &drvdata->base.enable_seq,
								       &drvdata->base.disable_seq,
								       false);
	if (!drvdata->base.config) {
		dev_err(drvdata->base.dev, "config create failed unexpectedly\n");
		goto kbase_pre_post_all_config_unregister;
	}

	return 0;

kbase_pre_post_all_config_unregister:
	kbase_debug_coresight_csf_config_free(drvdata->base.pre_post_all_config);
kbase_client_unregister:
	kbase_debug_coresight_csf_unregister(drvdata->base.kbase_client);
kbase_pre_post_all_client_unregister:
	kbase_debug_coresight_csf_unregister(drvdata->base.kbase_pre_post_all_client);

	return -EINVAL;
}

void coresight_mali_sources_deinit_drvdata(struct coresight_mali_source_drvdata *drvdata)
{
	if (drvdata->base.config != NULL)
		kbase_debug_coresight_csf_config_free(drvdata->base.config);

	if (drvdata->base.pre_post_all_config != NULL)
		kbase_debug_coresight_csf_config_free(drvdata->base.pre_post_all_config);

	if (drvdata->base.kbase_client != NULL)
		kbase_debug_coresight_csf_unregister(drvdata->base.kbase_client);

	if (drvdata->base.kbase_pre_post_all_client != NULL)
		kbase_debug_coresight_csf_unregister(drvdata->base.kbase_pre_post_all_client);
}

static const struct of_device_id mali_source_ids[] = { { .compatible =
								 "arm,coresight-mali-source-itm" },
						       {} };

static struct platform_driver mali_sources_platform_driver = {
	.probe      = coresight_mali_sources_probe,
	.remove     = coresight_mali_sources_remove,
	.driver = {
		.name = "coresight-mali-source-itm",
		.owner = THIS_MODULE,
		.of_match_table = mali_source_ids,
		.suppress_bind_attrs    = true,
	},
};

static int __init mali_sources_init(void)
{
	return platform_driver_register(&mali_sources_platform_driver);
}

static void __exit mali_sources_exit(void)
{
	platform_driver_unregister(&mali_sources_platform_driver);
}

module_init(mali_sources_init);
module_exit(mali_sources_exit);

MODULE_AUTHOR("ARM Ltd.");
MODULE_DESCRIPTION("Arm Coresight Mali source ITM");
MODULE_LICENSE("GPL");
