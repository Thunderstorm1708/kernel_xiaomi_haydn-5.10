/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The android11-5.4 drm_panel notifier is gone on 5.10 (msm-5.10 moved to
 * soc/qcom panel_event_notifier). Nothing on haydn listens to it any more
 * (touch and charger use mi_disp_notifier), so keep the types and make the
 * call a no-op.
 */
#ifndef _DRM_PANEL_NOTIFIER_COMPAT_H_
#define _DRM_PANEL_NOTIFIER_COMPAT_H_

#include <drm/drm_panel.h>

enum {
	DRM_PANEL_BLANK_UNBLANK,
	DRM_PANEL_BLANK_POWERDOWN,
	DRM_PANEL_BLANK_LP,
	DRM_PANEL_BLANK_FPS_CHANGE,
};

#define DRM_PANEL_EVENT_BLANK		0x01
#define DRM_PANEL_EARLY_EVENT_BLANK	0x02

struct drm_panel_notifier {
	int refresh_rate;
	void *data;
	uint32_t id;
};

static inline int drm_panel_notifier_call_chain(struct drm_panel *panel,
		unsigned long val, void *v)
{
	return 0;
}

#endif
