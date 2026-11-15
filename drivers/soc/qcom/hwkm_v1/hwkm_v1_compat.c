// SPDX-License-Identifier: GPL-2.0-only
/*
 * msm-5.10 <linux/hwkm.h> entry points on top of the HWKM v1 (lahaina)
 * driver. Kept in its own file because hwkm_v1.h and <linux/hwkm.h>
 * describe two different hardware generations with the same names.
 */
#include <linux/module.h>
#include <linux/hwkm.h>

int qti_hwkm_v1_init(void __iomem *hwkm_slave_mmio_base);

int qti_hwkm_init(const struct ice_mmio_data *mmio_data)
{
	return qti_hwkm_v1_init(mmio_data->ice_hwkm_mmio);
}
EXPORT_SYMBOL(qti_hwkm_init);

/* v1: the TP key is set up by qti_hwkm_v1_init() from the key programming path */
int qti_hwkm_ice_init_sequence(const struct ice_mmio_data *mmio_data)
{
	return 0;
}
EXPORT_SYMBOL(qti_hwkm_ice_init_sequence);

bool qti_hwkm_init_required(const struct ice_mmio_data *mmio_data)
{
	return true;
}
EXPORT_SYMBOL(qti_hwkm_init_required);

bool qti_hwkm_is_ice_tpkey_set(const struct ice_mmio_data *mmio_data)
{
	return false;
}
EXPORT_SYMBOL(qti_hwkm_is_ice_tpkey_set);
