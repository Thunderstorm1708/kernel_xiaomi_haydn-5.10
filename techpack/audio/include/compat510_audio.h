/* SPDX-License-Identifier: GPL-2.0-only */
/* 5.10 compat for the msm-5.4 lahaina APR audio techpack */
#ifndef _COMPAT510_AUDIO_H
#define _COMPAT510_AUDIO_H
#ifndef PCM_RUNTIME_CHECK
/* was in sound/core/pcm_local.h, not visible to drivers on 5.10 */
#define PCM_RUNTIME_CHECK(sub) snd_BUG_ON(!(sub) || !(sub)->runtime)
#endif

/* 5.10 snd_soc_get_pcm_runtime() takes a dai_link; msm-5.4 looked up by name */
#define snd_soc_get_pcm_runtime_by_name(__card, __nm) ({			\
	struct snd_soc_pcm_runtime *__rtd, *__found = NULL;		\
	for_each_card_rtds(__card, __rtd)				\
		if (__rtd->dai_link->name &&				\
		    !strcmp(__rtd->dai_link->name, (__nm))) {		\
			__found = __rtd;				\
			break;						\
		}							\
	__found;							\
})
#endif
