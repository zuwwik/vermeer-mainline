/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __Q6APM_VOICE_H__
#define __Q6APM_VOICE_H__

#include <sound/soc.h>

struct q6apm;
struct q6apm_graph;
struct audioreach_module_config;
struct audioreach_graph_info;

#define Q6APM_VOICE_DAI_NAME "Voice Call"
#define Q6APM_VOICE_DAI_ID 6

extern struct snd_soc_dai_driver q6apm_voice_dai;

int q6apm_voice_link_init(struct snd_soc_pcm_runtime *rtd, const char *firmware_prefix);
void q6apm_voice_link_exit(struct snd_soc_pcm_runtime *rtd);
int q6apm_voice_open(struct device *dev, long long sid);
void q6apm_voice_release(struct device *dev);
bool q6apm_voice_owns_audio(struct q6apm *apm);
bool q6apm_voice_media_allowed(struct q6apm *apm, u32 id);
int q6apm_voice_media_connection(struct q6apm *apm, struct audioreach_graph_info *info);
bool q6apm_voice_ready(struct device *dev);
int q6apm_voice_check_backend(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai);
struct q6apm_graph *q6apm_voice_graph_open(struct q6apm *apm,
					   struct device *dev, u32 id);
int q6apm_voice_graph_close(struct q6apm_graph *graph);
int q6apm_voice_graph_command(struct q6apm_graph *graph, u32 opcode);
int q6apm_voice_media_format(struct q6apm_graph *graph,
			     struct audioreach_module_config *cfg);
void q6apm_voice_destroy(struct q6apm *apm);

static inline bool q6apm_voice_substream(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);

	return !strcmp(snd_soc_rtd_to_cpu(rtd, 0)->name, Q6APM_VOICE_DAI_NAME);
}

#endif
