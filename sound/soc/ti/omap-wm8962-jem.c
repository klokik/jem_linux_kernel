#define DEBUG 1

#include <linux/of_platform.h>
#include <linux/i2c.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>

#include <asm/mach-types.h>
#include <linux/platform_data/asoc-ti-mcbsp.h>

#include "omap-mcbsp.h"

#include "../codecs/wm8962.h"


#define MCLK_FS		512
#define MCLK_RATE	19200000


struct jem_card_data {
	struct clk *mclk;
	unsigned int mclk_rate;
	unsigned int sysclk_rate;
};

static int ti_wm8962_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct jem_card_data *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;

	dev_dbg(codec_dai->dev, "%s() - enter\n", __func__);

	priv->sysclk_rate = params_rate(params) * MCLK_FS;

	ret = snd_soc_dai_set_pll(codec_dai, WM8962_FLL, WM8962_FLL_MCLK, // WM8962_FLL_INT,
				  priv->mclk_rate, priv->sysclk_rate);
	if (ret < 0) {
		dev_err(codec_dai->dev, "Failed to start CODEC FLL: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(codec_dai, WM8962_SYSCLK_FLL,
				     priv->sysclk_rate, SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(codec_dai->dev, "Failed to set CODEC SYSCLK: %d\n", ret);
		return ret;
	}

	// ret = snd_soc_dai_set_sysclk(cpu_dai, OMAP_MCBSP_SYSCLK_CLK,
	// 			SYSCLK_RATE, SND_SOC_CLOCK_IN);
	// if (ret < 0) {
	// 	dev_err(cpu_dai->dev, "Failed to set CPU SYSCLK: %d\n", ret);
	// 	return ret;
	// }

	// ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_DSP_B |
	// 			SND_SOC_DAIFMT_CBM_CFM |
	// 			SND_SOC_DAIFMT_NB_NF);
	// if (ret < 0) {
	// 	dev_err(codec_dai->dev, "Failed to set CODEC DAI format: %d\n",	ret);
	// 	return ret;
	// }

	// ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_DSP_B |
	// 			SND_SOC_DAIFMT_CBM_CFM |
	// 			SND_SOC_DAIFMT_NB_NF);
	// if (ret < 0) {
	// 	dev_err(cpu_dai->dev, "Failed to set CPU DAI format: %d\n", ret);
	// 	return ret;
	// }

	return ret;
}

static struct snd_soc_ops jem_ops = {
	.hw_params = ti_wm8962_hw_params,
};

static int ti_wm8962_set_bias_level(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm,
	enum snd_soc_bias_level level)
{
	struct jem_card_data *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd = snd_soc_get_pcm_runtime(card, card->dai_link[0].name);
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;

	if (dapm->dev != codec_dai->dev) {
		// dev_dbg(dapm->dev, "dapm->dev!=codec_dai->dev\n");
		return 0;
	}

	dev_dbg(codec_dai->dev, "%s() - enter\n", __func__);

	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		dev_dbg(codec_dai->dev, "setting bias PREPARE\n");
		if (dapm->bias_level != SND_SOC_BIAS_STANDBY)
			break;

		dev_dbg(codec_dai->dev, "Starting FLL\n");
		ret = snd_soc_dai_set_pll(codec_dai, WM8962_FLL, WM8962_FLL_MCLK, // WM8962_FLL_INT,
					priv->mclk_rate, priv->sysclk_rate);
		if (ret < 0) {
			pr_err("Failed to start FLL: %d\n", ret);
			return ret;
		}

		dev_dbg(codec_dai->dev, "Setting SYSCLK\n");
		ret = snd_soc_dai_set_sysclk(codec_dai, WM8962_SYSCLK_FLL,
					priv->sysclk_rate, SND_SOC_CLOCK_IN);
		if (ret < 0) {
			pr_err("Failed to set SYSCLK: %d\n", ret);
			return ret;
		}
		break;

	default:
		break;
	}

	return 0;
}

static void dump_codec_regs(struct snd_soc_component *component) {
	unsigned reg;

#define jemDUMP_REG(REG) reg =\
	snd_soc_component_read32(component, WM8962_##REG);\
	pr_debug("reg" #REG ": 0x%04x\n", reg);

	jemDUMP_REG(CLOCKING1);
	jemDUMP_REG(CLOCKING2);
	jemDUMP_REG(FLL_CONTROL_1);
	jemDUMP_REG(PWR_MGMT_1);
	jemDUMP_REG(PWR_MGMT_2);
	jemDUMP_REG(DC_SERVO_6);
}

static int ti_wm8962_set_bias_level_post(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm,
	enum snd_soc_bias_level level)
{
	struct jem_card_data *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd = snd_soc_get_pcm_runtime(card,
						card->dai_link[0].name);
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_component *component = snd_soc_dapm_to_component(dapm);
	int ret;

	if (dapm->dev != codec_dai->dev)
		return 0;

	dev_dbg(codec_dai->dev, "%s() - enter\n", __func__);

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		dev_dbg(codec_dai->dev, "setting bias STANDBY\n");
		if (dapm->bias_level != SND_SOC_BIAS_PREPARE)
			break;

		dev_dbg(codec_dai->dev, "Stopping SYSCLK\n");
		ret = snd_soc_dai_set_sysclk(codec_dai, WM8962_SYSCLK_MCLK,
				priv->sysclk_rate, SND_SOC_CLOCK_IN);
		if (ret < 0) {
			pr_err("Failed to set SYSCLK: %d\n", ret);
			return ret;
		}

		dev_dbg(codec_dai->dev, "Stopping FLL\n");
		ret = snd_soc_dai_set_pll(codec_dai,
				WM8962_FLL, WM8962_FLL_MCLK, 0, 0);
		if (ret < 0) {
			pr_err("Failed to stop FLL: %d\n", ret);
			return ret;
		}
		break;

	case SND_SOC_BIAS_ON:
		dev_dbg(codec_dai->dev, "setting bias ON\n");
		// dump_codec_regs(component);
		break;

	default:
		break;
	}

	dapm->bias_level = level;

	return 0;
}

static const struct snd_soc_dapm_widget ti_wm8962_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_SPK("Main Speaker", NULL),
};

static const struct snd_kcontrol_new bowser_controls[] = {
	SOC_DAPM_PIN_SWITCH("DMICDAT"),
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Main Speaker"),
};

static const struct snd_soc_dapm_route audio_map[] = {
	{ "Headphone", NULL, "HPOUTL" },
	{ "Headphone", NULL, "HPOUTR" },

	{ "Main Speaker", NULL, "SPKOUTL" },
	{ "Main Speaker", NULL, "SPKOUTR" },
};

static struct snd_soc_dai_link jem_dai_links[] = {
	{
		.name = "JemAudio",
		.stream_name = "Playback",
		.codec_dai_name = "wm8962",
		.dai_fmt = SND_SOC_DAIFMT_DSP_B | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBM_CFM,
		.ops = &jem_ops,
	},
};

/* Audio machine driver */
static struct snd_soc_card snd_soc_jem = {
	.name = "JemAudio",
	.owner = THIS_MODULE,
	.dai_link = jem_dai_links,
	.num_links = ARRAY_SIZE(jem_dai_links),

	.set_bias_level	= ti_wm8962_set_bias_level,
	.set_bias_level_post = ti_wm8962_set_bias_level_post,

	.dapm_widgets = ti_wm8962_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(ti_wm8962_dapm_widgets),
	.dapm_routes = audio_map,
	.num_dapm_routes = ARRAY_SIZE(audio_map),
	// .controls = bowser_controls,
	// .num_controls = ARRAY_SIZE(bowser_controls),

	.fully_routed = true,
};

static int ti_wm8962_probe(struct platform_device *pdev)
{
	struct jem_card_data *priv;
	struct snd_soc_card *card = &snd_soc_jem;
	struct device_node *ssi_np, *codec_np;
	struct platform_device *ssi_pdev;
	struct i2c_client *codec_dev;
	int ret = 0;

	dev_dbg(&pdev->dev, "Jem Audio Card / OMAP4x SoC probe\n");

	ssi_np = of_parse_phandle(pdev->dev.of_node, "ssi-controller", 0);
	codec_np = of_parse_phandle(pdev->dev.of_node, "audio-codec", 0);
	if (!ssi_np || !codec_np) {
		dev_err(&pdev->dev, "phandle missing or invalid\n");
		ret = -EINVAL;
		goto fail;
	}

	ssi_pdev = of_find_device_by_node(ssi_np);
	if (!ssi_pdev) {
		dev_err(&pdev->dev, "failed to find SSI platform device\n");
		ret = -EINVAL;
		goto fail;
	}
	codec_dev = of_find_i2c_device_by_node(codec_np);
	if (!codec_dev || !codec_dev->dev.driver) {
		dev_err(&pdev->dev, "failed to find codec platform device\n");
		ret = -EPROBE_DEFER; //-EINVAL; // FIXME: defer conditionally
		goto fail;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->mclk = devm_clk_get(&codec_dev->dev, NULL);
	if (IS_ERR(priv->mclk)) {
		ret = PTR_ERR(priv->mclk);
		dev_err(&pdev->dev, "failed to get codec clk: %d\n", ret);
		goto fail;
	}

	// priv->mclk_rate = clk_get_rate(priv->mclk);
	priv->mclk_rate = MCLK_RATE;
	ret = clk_set_rate(priv->mclk, priv->mclk_rate);
	if (ret) {
		dev_err(&pdev->dev, "failed to set codec clk rate: %d\n", ret);
		goto fail;
	}
	dev_dbg(&pdev->dev, "MCLK new rate: %d\n", priv->mclk_rate);


	/* Init snd_soc_card */
	card->dev = &pdev->dev;
	jem_dai_links[0].codec_of_node		= codec_np;
	jem_dai_links[0].platform_of_node	= ssi_np;
	jem_dai_links[0].cpu_dai_name		= dev_name(&ssi_pdev->dev);

	snd_soc_card_set_drvdata(card, priv);

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
		goto fail;
	}
	dev_dbg(&codec_dev->dev, "Card registered\n");

	of_node_put(ssi_np);
	of_node_put(codec_np);

	return 0;

fail:
	of_node_put(ssi_np);
	of_node_put(codec_np);

	return ret;
}

static int ti_wm8962_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct jem_card_data *priv = snd_soc_card_get_drvdata(card);

	dev_dbg(&pdev->dev, "Card removed\n");

	return 0;
}

static const struct of_device_id ti_wm8962_dt_ids[] = {
	{ .compatible = "ti,jem-audio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ti_wm8962_dt_ids);

static struct platform_driver ti_wm8962_driver = {
	.driver = {
		.name = "ti-wm8962",
		.pm = &snd_soc_pm_ops,
		.of_match_table = ti_wm8962_dt_ids,
	},
	.probe = ti_wm8962_probe,
	.remove = ti_wm8962_remove,
};
module_platform_driver(ti_wm8962_driver);

MODULE_AUTHOR("Mykola Dolhyi <0xb000@gmail.com>");
MODULE_DESCRIPTION("ALSA SoC OMAP4 / KF HD Jem");
MODULE_LICENSE("GPL v2");
