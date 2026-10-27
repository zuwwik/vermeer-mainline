// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022, Linaro Limited

#include <dt-bindings/sound/qcom,q6dsp-lpass-ports.h>
#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/soundwire/sdw.h>
#include <sound/jack.h>
#include <linux/input-event-codes.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include "qdsp6/q6afe.h"
#include "qdsp6/q6apm-voice.h"
#include "common.h"
#include "sdw.h"

struct sc8280xp_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_card *card;
	struct snd_soc_jack jack;
	struct snd_soc_jack dp_jack[8];
	bool jack_setup;
	bool vermeer_tdm;
};

static int sc8280xp_snd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_jack *dp_jack  = NULL;
	struct snd_soc_dai *codec_dai;
	int dp_pcm_id = 0;
	int ret;
	int j;

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX:
	case PRIMARY_MI2S_TX:
	case TERTIARY_MI2S_RX...QUATERNARY_MI2S_TX:
	case QUINARY_MI2S_RX...QUINARY_MI2S_TX:
		snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_BP_FP);
		break;
	case SECONDARY_MI2S_RX:
		if (!data->vermeer_tdm) {
			snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_BP_FP);
			break;
		}

		/*
		 * Legacy MI2S diagnostic path only. The stock Vermeer speaker
		 * backend is SECONDARY_TDM_RX_0, with framing kept from the ACF.
		 */
		for_each_rtd_codec_dais(rtd, j, codec_dai) {
			ret = snd_soc_dai_set_tdm_slot(codec_dai, 0, 0, 0, 16);
			if (ret && ret != -EOPNOTSUPP)
				return ret;
		}
		break;
	case WSA_CODEC_DMA_RX_0:
	case WSA_CODEC_DMA_RX_1:
		/*
		 * Set limit of -3 dB on Digital Volume and 0 dB on PA Volume
		 * to reduce the risk of speaker damage until we have active
		 * speaker protection in place.
		 */
		snd_soc_limit_volume(card, "WSA_RX0 Digital Volume", 81);
		snd_soc_limit_volume(card, "WSA_RX1 Digital Volume", 81);
		snd_soc_limit_volume(card, "SpkrLeft PA Volume", 17);
		snd_soc_limit_volume(card, "SpkrRight PA Volume", 17);
		break;
	case DISPLAY_PORT_RX_0:
		/* DISPLAY_PORT dai ids are not contiguous */
		dp_pcm_id = 0;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	case DISPLAY_PORT_RX_1 ... DISPLAY_PORT_RX_7:
		dp_pcm_id = cpu_dai->id - DISPLAY_PORT_RX_1 + 1;
		dp_jack = &data->dp_jack[dp_pcm_id];
		break;
	default:
		break;
	}

	if (dp_jack)
		return qcom_snd_dp_jack_setup(rtd, dp_jack, dp_pcm_id);

	return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);
}

static int sc8280xp_snd_startup(struct snd_pcm_substream *substream)
{
	unsigned int fmt = SND_SOC_DAIFMT_BP_FP;
	unsigned int codec_dai_fmt = SND_SOC_DAIFMT_BC_FC;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	int j;

	switch (cpu_dai->id) {
	case SECONDARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		for_each_rtd_codec_dais(rtd, j, codec_dai) {
			snd_soc_component_set_sysclk(codec_dai->component,
							0, 0,
							1536000, SND_SOC_CLOCK_IN);
			snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		}
		break;
	default:
		break;
	}

	return qcom_snd_sdw_startup(substream);
}

static int sc8280xp_vermeer_tdm_hw_params(struct snd_pcm_substream *substream,
					  struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	unsigned int bclk;
	int ret;
	int j;

	if (!data->vermeer_tdm || cpu_dai->id != SECONDARY_TDM_RX_0)
		return 0;

	/*
	 * The Vermeer stock DTBO selects four 32-bit slots and an inverted
	 * SEC_TDM BCLK (qcom,tdm-max-slots = 4, qcom,tdm-clk-attribute = 4).
	 * The AW88261 profile configures each amplifier's slot internally;
	 * do not override that profile from ASoC.
	 */
	bclk = params_rate(params) * 4 * 32;
	/* Match the DSP endpoint's clock request to the inverted PRM clock vote. */
	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_BP_FP |
					SND_SOC_DAIFMT_IB_NF);
	if (ret)
		return ret;

	ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0, 0x3, 4, 32);
	if (ret)
		return ret;

	ret = snd_soc_dai_set_sysclk(cpu_dai, LPAIF_BIT_CLK, bclk,
				       SND_SOC_CLOCK_IN);
	if (ret)
		return ret;

	dev_info(rtd->card->dev,
		 "Vermeer speaker BCLK vote: %u Hz (4 x 32 at %u Hz)\n",
		 bclk, params_rate(params));

	for_each_rtd_codec_dais(rtd, j, codec_dai) {
		ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S |
					  SND_SOC_DAIFMT_NB_NF |
					  SND_SOC_DAIFMT_BC_FC);
		if (ret && ret != -ENOTSUPP)
			return ret;

		ret = snd_soc_dai_set_sysclk(codec_dai, 0, bclk,
					       SND_SOC_CLOCK_IN);
		if (ret && ret != -ENOTSUPP)
			return ret;
	}

	return 0;
}

static int sc8280xp_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	/* Android's TDM-LPAIF-RX-SECONDARY endpoint uses 32-bit stereo PCM. */
	if (data->vermeer_tdm && cpu_dai->id == SECONDARY_TDM_RX_0) {
		snd_mask_none(fmt);
		snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S32_LE);
	} else {
		snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);
	}
	channels->min = 2;
	channels->max = 2;
	switch (cpu_dai->id) {
	case TX_CODEC_DMA_TX_0:
	case TX_CODEC_DMA_TX_1:
	case TX_CODEC_DMA_TX_2:
	case TX_CODEC_DMA_TX_3:
		channels->min = 1;
		break;
	default:
		break;
	}


	return 0;
}

static int sc8280xp_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	return qcom_snd_sdw_prepare(substream, &data->stream_prepared[cpu_dai->id]);
}

static int sc8280xp_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sc8280xp_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static const struct snd_soc_ops sc8280xp_be_ops = {
	.startup = sc8280xp_snd_startup,
	.shutdown = qcom_snd_sdw_shutdown,
	.hw_params = sc8280xp_vermeer_tdm_hw_params,
	.hw_free = sc8280xp_snd_hw_free,
	.prepare = sc8280xp_snd_prepare,
};

static void sc8280xp_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->init = sc8280xp_snd_init;
			link->be_hw_params_fixup = sc8280xp_be_hw_params_fixup;
			link->ops = &sc8280xp_be_ops;
		}
	}
}

static int sc8280xp_vermeer_voice_init(struct snd_soc_pcm_runtime *rtd)
{
	static const struct snd_soc_dapm_route routes[] = {
		{ "Secondary TDM0 Playback", NULL, "Voice Call Playback" },
		{ "Voice Call Capture", NULL, "TX_CODEC_DMA_TX_3 Capture" },
	};
	int ret;

	ret = q6apm_voice_link_init(rtd, "qcom/sm8550/vermeer-voice");
	if (ret)
		return ret;
	ret = snd_soc_dapm_add_routes(snd_soc_card_to_dapm(rtd->card),
				      routes, ARRAY_SIZE(routes));
	if (ret)
		q6apm_voice_link_exit(rtd);
	return ret;
}

static int sc8280xp_vermeer_voice_link(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *links, *link;
	struct snd_soc_dai_link_component *components;
	struct device_node *apm, *dais;

	apm = card->dai_link[0].platforms[0].of_node;
	dais = of_get_compatible_child(apm, "qcom,q6apm-dais");
	if (!dais)
		return -ENODEV;
	links = devm_kcalloc(card->dev, card->num_links + 1, sizeof(*links), GFP_KERNEL);
	components = devm_kcalloc(card->dev, 2, sizeof(*components), GFP_KERNEL);
	if (!links || !components) {
		of_node_put(dais);
		return -ENOMEM;
	}
	memcpy(links, card->dai_link, card->num_links * sizeof(*links));
	link = &links[card->num_links];
	link->name = Q6APM_VOICE_DAI_NAME;
	link->stream_name = Q6APM_VOICE_DAI_NAME;
	link->id = Q6APM_VOICE_DAI_ID;
	link->cpus = &components[0];
	link->cpus[0].of_node = apm;
	link->cpus[0].dai_name = Q6APM_VOICE_DAI_NAME;
	link->num_cpus = 1;
	link->platforms = &components[1];
	link->platforms[0].of_node = dais;
	link->num_platforms = 1;
	link->codecs = &snd_soc_dummy_dlc;
	link->num_codecs = 1;
	link->dynamic = 1;
	link->nonatomic = 1;
	link->ignore_suspend = 1;
	link->ignore_pmdown_time = 1;
	link->init = sc8280xp_vermeer_voice_init;
	link->exit = q6apm_voice_link_exit;
	card->dai_link = links;
	card->num_links++;
	/* Device-tree nodes outlive this card. The link only borrows this node. */
	of_node_put(dais);
	return 0;
}

static int sc8280xp_vermeer_link_codecs(struct snd_soc_card *card)
{
	struct snd_soc_dai_link_component *codec;
	struct snd_soc_dai_link *link;
	struct platform_device *supplier;
	struct device_link *dependency;
	int i, j;

	/* ASoC's stateless links order suspend, but do not reprobe this
	 * machine driver when LPASS components disappear during ADSP SSR.
	 * The parsed DAI names and jack state must belong to the new card.
	 * LPASS platform devices survive while their drivers unbind/rebind.
	 */
	for_each_card_prelinks(card, i, link) {
		for_each_link_codecs(link, j, codec) {
			if (!codec->of_node)
				continue;
			supplier = of_find_device_by_node(codec->of_node);
			if (!supplier)
				continue;
			dependency = device_link_add(card->dev, &supplier->dev,
						     DL_FLAG_AUTOPROBE_CONSUMER);
			put_device(&supplier->dev);
			if (!dependency)
				return -EINVAL;
		}
	}
	return 0;
}

static int sc8280xp_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sc8280xp_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;
	card->owner = THIS_MODULE;
	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->vermeer_tdm = of_device_is_compatible(dev->of_node,
						      "xiaomi,vermeer-sndcard");
	card->dev = dev;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	card->driver_name = of_device_get_match_data(dev);
	sc8280xp_add_be_ops(card);
	if (data->vermeer_tdm) {
		ret = sc8280xp_vermeer_link_codecs(card);
		if (ret)
			return ret;
		ret = sc8280xp_vermeer_voice_link(card);
		if (ret)
			return ret;
	}
	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id snd_sc8280xp_dt_match[] = {
	{.compatible = "xiaomi,vermeer-sndcard", "sm8550"},
	{.compatible = "qcom,eliza-sndcard", "eliza"},
	{.compatible = "qcom,kaanapali-sndcard", "kaanapali"},
	{.compatible = "qcom,qcm6490-idp-sndcard", "qcm6490"},
	{.compatible = "qcom,qcs615-sndcard", "qcs615"},
	{.compatible = "qcom,qcs6490-rb3gen2-sndcard", "qcs6490"},
	{.compatible = "qcom,qcs8275-sndcard", "qcs8300"},
	{.compatible = "qcom,qcs9075-sndcard", "sa8775p"},
	{.compatible = "qcom,qcs9100-sndcard", "sa8775p"},
	{.compatible = "qcom,sc8280xp-sndcard", "sc8280xp"},
	{.compatible = "qcom,sm8450-sndcard", "sm8450"},
	{.compatible = "qcom,sm8550-sndcard", "sm8550"},
	{.compatible = "qcom,sm8650-sndcard", "sm8650"},
	{.compatible = "qcom,sm8750-sndcard", "sm8750"},
	{}
};

MODULE_DEVICE_TABLE(of, snd_sc8280xp_dt_match);

static struct platform_driver snd_sc8280xp_driver = {
	.probe  = sc8280xp_platform_probe,
	.driver = {
		.name = "snd-sc8280xp",
		.of_match_table = snd_sc8280xp_dt_match,
	},
};
module_platform_driver(snd_sc8280xp_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("SC8280XP ASoC Machine Driver");
MODULE_LICENSE("GPL");
