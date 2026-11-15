/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * drm/drmP.h was removed upstream in 5.5. The msm-5.4 display techpack
 * still includes it everywhere, so keep a local stand-in that pulls in
 * what the old header used to provide.
 */
#ifndef _TECHPACK_DRM_P_H_
#define _TECHPACK_DRM_P_H_

#include <linux/agp_backend.h>
#include <linux/cdev.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/ratelimit.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/dma-fence.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mman.h>
#include <linux/uaccess.h>

#include <drm/drm.h>
#include <drm/drm_agpsupport.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem.h>
#include <drm/drm_hashtab.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_irq.h>
#include <drm/drm_mm.h>
#include <drm/drm_print.h>
#include <drm/drm_sysfs.h>
#include <drm/drm_vblank.h>
#include <drm/drm_prime.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_atomic.h>
#include <drm/drm_mode_object.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_encoder.h>
#include <drm/drm_connector.h>
#include <drm/drm_modes.h>
#include <drm/drm_plane.h>
#include <drm/drm_property.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_dp_helper.h>
#include <drm/drm_of.h>
#include <drm/drm_client.h>

#endif /* _TECHPACK_DRM_P_H_ */
