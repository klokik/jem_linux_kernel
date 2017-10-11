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

#include "mcbsp.h"
#include "omap-mcbsp.h"

#include "../codecs/wm8962.h"

#define SAMPLE_RATE		44100
#define SYSCLK_RATE 	(512*SAMPLE_RATE)
#define PLL_MCLK_RATE		19200000

struct jem_wm8962_data {
	// struct snd_soc_dai_link dai;
	// struct snd_soc_card card;
	struct clk *mclk;
	unsigned int pll_mclk_rate; // 192...
};

static int jem_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct omap_mcbsp *mcbsp = snd_soc_dai_get_drvdata(cpu_dai);
	int ret;

	dev_dbg(codec_dai->dev, "%s() - enter\n", __func__);

#if 0 // FTM
	ret = snd_soc_dai_set_sysclk(codec_dai, WM8962_SYSCLK_MCLK,
				PLL_MCLK_RATE, SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(codec_dai->dev, "Failed to set CODEC SYSCLK: %d\n",
			ret);
		return ret;
	}
#endif

	ret = snd_soc_dai_set_pll(codec_dai, WM8962_FLL, WM8962_FLL_MCLK,
				PLL_MCLK_RATE, SYSCLK_RATE);
	if (ret < 0) {
		dev_err(codec_dai->dev, "Failed to start CODEC FLL: %d\n",
			ret);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(codec_dai, WM8962_SYSCLK_FLL,
				SYSCLK_RATE, SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(codec_dai->dev, "Failed to set CODEC SYSCLK: %d\n",
			ret);
		return ret;
	}
	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_DSP_B |
				SND_SOC_DAIFMT_CBM_CFM |
				SND_SOC_DAIFMT_NB_NF);
	if (ret < 0) {
		dev_err(codec_dai->dev, "Failed to set CODEC DAI format: %d\n",
			ret);
		return ret;
	}

	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_DSP_B |
				SND_SOC_DAIFMT_CBM_CFM |
				SND_SOC_DAIFMT_NB_NF);
	if (ret < 0) {
		dev_err(cpu_dai->dev, "Failed to set CPU DAI format: %d\n",
			ret);
		return ret;
	}

	omap_mcbsp_set_tx_threshold(mcbsp, params_channels(params));

	dev_dbg(codec_dai->dev, "%s() - exit\n", __func__);
	return ret;
}

static struct snd_soc_ops jem_ops = {
	.hw_params = jem_hw_params,
};

static int wm8962_set_bias_level(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm,
	enum snd_soc_bias_level level)
{
	// struct jem_wm8962_data *priv =	snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd = snd_soc_get_pcm_runtime(card, card->dai_link[0].name);
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;

	if (dapm->dev != codec_dai->dev) {
		dev_dbg(dapm->dev, "dapm->dev!=codec_dai->dev\n");
		return 0;
	}

	dev_dbg(codec_dai->dev, "%s() - enter\n", __func__);

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		dev_dbg(codec_dai->dev, "setting bias STANDBY\n");
		// if (dapm->bias_level == SND_SOC_BIAS_OFF) {
		// 	dev_dbg(codec_dai->dev, "Enable MCLK\n");
		// 	ret = clk_prepare_enable(priv->mclk_dev);
		// 	if (ret < 0) {
		// 		dev_err(codec_dai->dev, "Failed to enable MCLK: %d\n", ret);
		// 		return ret;
		// 	}
		// }
		break;

	case SND_SOC_BIAS_PREPARE:
		dev_dbg(codec_dai->dev, "setting bias PREPARE\n");
		if (dapm->bias_level == SND_SOC_BIAS_STANDBY) {
			dev_dbg(codec_dai->dev, "Starting FLL\n");
			ret = snd_soc_dai_set_pll(codec_dai, WM8962_FLL, WM8962_FLL_MCLK,
						PLL_MCLK_RATE, SYSCLK_RATE);
			if (ret < 0) {
				pr_err("Failed to start FLL: %d\n", ret);
				return ret;
			}

			dev_dbg(codec_dai->dev, "Setting SYSCLK\n");
			ret = snd_soc_dai_set_sysclk(codec_dai, WM8962_SYSCLK_FLL,
						SYSCLK_RATE, SND_SOC_CLOCK_IN);
			if (ret < 0) {
				pr_err("Failed to set SYSCLK: %d\n", ret);
				return ret;
			}
		}
		break;

	case SND_SOC_BIAS_ON:
		dev_dbg(codec_dai->dev, "setting bias ON\n");
		break;

	case SND_SOC_BIAS_OFF:
		dev_dbg(codec_dai->dev, "setting bias OFF\n");
		break;

	default:
		dev_dbg(codec_dai->dev, "setting bias OTHER: %d\n", level);
		break;
	}

	dev_dbg(codec_dai->dev, "%s() - exit\n", __func__);

	return 0;
}

static int wm8962_set_bias_level_post(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm,
	enum snd_soc_bias_level level)
{
	// struct jem_wm8962_data *priv =	snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd = snd_soc_get_pcm_runtime(card, card->dai_link[0].name);
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;

	if (dapm->dev != codec_dai->dev)
		return 0;

	dev_dbg(codec_dai->dev, "%s() - enter\n", __func__);

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		dev_dbg(codec_dai->dev, "setting bias STANDBY\n");
		// if (dapm->bias_level == SND_SOC_BIAS_PREPARE) {
		// }
		break;

	case SND_SOC_BIAS_PREPARE:
		dev_dbg(codec_dai->dev, "setting bias PREPARE\n");
		break;

	case SND_SOC_BIAS_ON:
		dev_dbg(codec_dai->dev, "setting bias ON\n");
		break;

	case SND_SOC_BIAS_OFF:
		dev_dbg(codec_dai->dev, "setting bias OFF\n");

		dev_dbg(codec_dai->dev, "Stopping SYSCLK\n");
		ret = snd_soc_dai_set_sysclk(codec_dai, WM8962_SYSCLK_MCLK,
					SYSCLK_RATE, SND_SOC_CLOCK_IN);
		if (ret < 0) {
			pr_err("Failed to set SYSCLK: %d\n", ret);
			return ret;
		}

		dev_dbg(codec_dai->dev, "Stopping FLL\n");
		ret = snd_soc_dai_set_pll(codec_dai, WM8962_FLL, WM8962_FLL_MCLK,
					0, 0);
		if (ret < 0) {
			pr_err("Failed to stop FLL: %d\n", ret);
			return ret;
		}
		break;

	default:
		dev_dbg(codec_dai->dev, "setting bias OTHER: %d\n", level);
		break;
	}

	dapm->bias_level = level;
	dev_dbg(codec_dai->dev, "%s() - exit\n", __func__);

	return 0;
}

/* jem machine dapm widgets */
static const struct snd_soc_dapm_widget jem_wm8962_dapm_widgets[] = {
	// SND_SOC_DAPM_HP("HP", NULL),
	SND_SOC_DAPM_SPK("Main Speaker", NULL),
};

static const struct snd_soc_dapm_route audio_map[] = {
	// { "HP", NULL, "HPOUTL" },
	// { "HP", NULL, "HPOUTR" },

	{ "Main Speaker", NULL, "SPKOUTL" },
	{ "Main Speaker", NULL, "SPKOUTR" },
};

/* Digital audio interface glue - connects codec <--> CPU */
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

	.set_bias_level	= wm8962_set_bias_level,
	.set_bias_level_post = wm8962_set_bias_level_post,

	// TODO:
	// .controls
	// .num_controls
	.dapm_widgets = jem_wm8962_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(jem_wm8962_dapm_widgets),
	.dapm_routes = audio_map,
	.num_dapm_routes = ARRAY_SIZE(audio_map),
	.fully_routed = true,
};

static int ti_wm8962_probe(struct platform_device *pdev)
{
	struct jem_wm8962_data *priv;
	struct snd_soc_card *card = &snd_soc_jem;
	// struct snd_soc_dai_link *dai_link;
	// struct simple_dai_props *dai_props;
	struct device_node *ssi_np, *codec_np;
	struct platform_device *ssi_pdev;
	struct i2c_client *codec_dev;
	int ret = 0;

	dev_dbg(&pdev->dev, "Jem Audio Card / OMAP4x SoC init\n");

	if (!pdev->dev.of_node || !of_device_is_available(pdev->dev.of_node)) {
		dev_dbg(&pdev->dev, "Legacy probe not supported\n");
		return -EINVAL;
	}

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
		ret = -EINVAL;
		goto fail;
	}

	/* Allocate the private data and the DAI link array */
	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->mclk = devm_clk_get(&codec_dev->dev, NULL);
	if (IS_ERR(priv->mclk)) {
		ret = PTR_ERR(priv->mclk);
		dev_err(&pdev->dev, "failed to get codec clk: %d\n", ret);
		goto fail;
	}

	priv->pll_mclk_rate = PLL_MCLK_RATE;//clk_get_rate(priv->mclk);
	// ret = clk_set_rate(priv->mclk, priv->pll_mclk_rate);
	ret = clk_prepare_enable(priv->mclk);
	if (ret) {
		dev_err(&pdev->dev, "failed to set or enable codec clk: %d\n", ret);
		goto fail;
	}
	dev_dbg(&pdev->dev, "MCLK enabled: %lu\n", clk_get_rate(priv->mclk));


	/* Init snd_soc_card */
	card->dev = &pdev->dev;
	jem_dai_links[0].codec_of_node		= codec_np;
	jem_dai_links[0].platform_of_node	= ssi_np;
	jem_dai_links[0].cpu_dai_name		= dev_name(&ssi_pdev->dev);


	snd_soc_card_set_drvdata(card, priv);

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
		goto clk_fail;
	}
	dev_dbg(&codec_dev->dev, "Card registered\n");

	of_node_put(ssi_np);
	of_node_put(codec_np);

	return 0;

clk_fail:
	clk_disable_unprepare(priv->mclk);
fail:
	of_node_put(ssi_np);
	of_node_put(codec_np);

	return ret;
}

static int ti_wm8962_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct jem_wm8962_data *priv = snd_soc_card_get_drvdata(card);

	if (!IS_ERR(priv->mclk)) {
		clk_disable_unprepare(priv->mclk);
		dev_dbg(&pdev->dev, "MCLK disabled\n");
	}

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
