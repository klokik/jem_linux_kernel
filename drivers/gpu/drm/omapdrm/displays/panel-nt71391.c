
#undef DEBUG
#undef USE_OMAP_TIMINGS

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/backlight.h>
#include <linux/workqueue.h>

#include <video/omap-panel-data.h>
#include <video/mipi_display.h>

#include "../dss/omapdss.h"


#define DCS_READ_NUM_ERRORS	0x05
#define DCS_BRIGHTNESS		0x51
#define DCS_CTRL_DISPLAY	0x53

#define MCS_READ_ID1		0x8d
#define MCS_READ_ID2		0x8e
#define MCS_READ_ID3		0x8f

#define MCS_LOCK		0xf3
#define MCS_UNLOCK		0x00
#define MCS_WRITE_CLOCK	0xac
#define MCS_PERIPH_ON	0x32

#define MCS_PARAM_CLK153	0x2b
#define MCS_PARAM_LOCK		0xa0


struct panel_drv_data {
	struct omap_dss_device dssdev;

	struct videomode vm;

	struct device_node *backlight_node;
	struct backlight_device *backlight;
	struct delayed_work backlight_work;

	struct platform_device *pdev;

	struct mutex lock;

	struct gpio_desc *enable_gpio;
	struct gpio_desc *cabc_gpio;

	int channel0;
	int channel1;

	bool intro_printed;
};

#define NT71391_WIDTH		1920
#define NT71391_HEIGHT		1200
#define NT71391_PCLK		145066
#define NT71391_PIXELCLOCK	(NT71391_PCLK*1000)

/* DISPC timings */
#define NT71391_HFP		9
#define NT71391_HSW		5
#define NT71391_HBP		50
#define NT71391_VFP		9
#define NT71391_VSW		2
#define NT71391_VBP		9


static struct videomode nt71391_vm = {
	.hactive		= 1920,
	.vactive		= 1200,

	// .pixelclock		= 145066000,
	.pixelclock		= 145228800,

	.hfront_porch	= 9,
	.hsync_len		= 5,
	.hback_porch	= 50,

	.vfront_porch	= 9,
	.vsync_len		= 2,
	.vback_porch	= 9,
};

#ifdef USE_OMAP_TIMINGS
static struct omap_video_timings nt71391_timings = {
	.x_res		= 1920,
	.y_res		= 1200,
	.pixelclock	= 145066000,
	.hfp		= 9,
	.hsw		= 5,
	.hbp		= 50,
	.vfp		= 9,
	.vsw		= 2,
	.vbp		= 9,
};
#endif /* USE_OMAP_TIMINGS */

#define to_panel_data(p) container_of(p, struct panel_drv_data, dssdev)


static int pdsivm_power_on(struct panel_drv_data *ddata);
static void pdsivm_power_off(struct panel_drv_data *ddata);

static int pdsivm_dcs_write_0(struct panel_drv_data *ddata, u8 dcs_cmd)
{
	struct omap_dss_device *in = ddata->dssdev.src;

	if (dcs_cmd == MIPI_DSI_TURN_ON_PERIPHERAL)
		return in->ops->dsi.turn_on_periph(in, ddata->channel0);
	else
		return in->ops->dsi.dcs_write(in, ddata->channel0, &dcs_cmd, 1);
}

static int pdsivm_dcs_write_1(struct panel_drv_data *ddata, u8 dcs_cmd, u8 param)
{
	struct omap_dss_device *in = ddata->dssdev.src;
	u8 buf[2] = { dcs_cmd, param };

	return in->ops->dsi.dcs_write(in, ddata->channel0, buf, 2);
}

static int pdsivm_get_id(struct panel_drv_data *ddata, u8 *id1, u8 *id2, u8 *id3)
{
	struct omap_dss_device *in = ddata->dssdev.src;
	u8 buf[2];
	int r;

	dev_dbg(&ddata->pdev->dev, "Get ID\n");

	buf[0] = MCS_LOCK;
	buf[1] = MCS_PARAM_LOCK;
	r = in->ops->dsi.gen_write_nosync(in, ddata->channel0, buf, 2);
	if (r)
		return r;

	buf[0] = MCS_READ_ID1;
	r = in->ops->dsi.gen_read(in, ddata->channel0, buf, 1, id1, 1);
	if (r)
		return r;

	buf[0] = MCS_READ_ID2;
	r = in->ops->dsi.gen_read(in, ddata->channel0, buf, 1, id2, 1);
	if (r)
		return r;

	buf[0] = MCS_READ_ID3;
	r = in->ops->dsi.gen_read(in, ddata->channel0, buf, 1, id3, 1);
	if (r)
		return r;

	buf[0] = MCS_UNLOCK;
	r = in->ops->dsi.gen_write_nosync(in, ddata->channel0, buf, 1);
	if (r)
		return r;

	return 0;
}

void nt71391_set_clk_153(struct panel_drv_data *ddata)
{
	struct omap_dss_device  *in = ddata->dssdev.src;

	u8 buf[2];
	int r = 0;

	dev_dbg(&ddata->pdev->dev, "Set clock 153\n");

	buf[0] = 0xF3;
	buf[1] = 0xA0;

	r = in->ops->dsi.gen_write_nosync(in, ddata->channel0, buf, 2);
	if(r < 0) {
		dev_err(&ddata->pdev->dev, "Error in sending ulock cmd\n");
		goto err;
	}

	mdelay(2);

	buf[0] = 0xac;	// ADDR=AC
	buf[1] = 0x2b;	// CLK=153

	r = in->ops->dsi.gen_write_nosync(in, ddata->channel0, buf, 2);
	if(r < 0) {
		dev_err(&ddata->pdev->dev, "Error in setting clk cmd\n");
		goto err;
	}

	mdelay(2);

	buf[0] = 0;
	buf[1] = 0;

	r = in->ops->dsi.gen_write_nosync(in, ddata->channel0, buf, 2);
	if(r < 0) {
		dev_err(&ddata->pdev->dev, "Error in sending lock cmd\n");
		goto err;
	}

	return;
err:
	dev_dbg(&ddata->pdev->dev, "Failed to set system clock\n");
	return;
}

static int pdsivm_connect(struct omap_dss_device *in, struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct device *dev = &ddata->pdev->dev;
	int r;

	dev_dbg(dev, "connect\n");
	if (omapdss_device_is_connected(dssdev))
		return 0;

	r = in->ops->dsi.request_vc(in, &ddata->channel0);
	if (r) {
		dev_err(dev, "failed to get virtual channel0\n");
		goto err_req_vc0;
	}
	r = in->ops->dsi.set_vc_id(in, ddata->channel0, 0);
	if (r) {
		dev_err(dev, "failed to set VC_ID0\n");
		goto err_vc_id0;
	}

	r = in->ops->dsi.request_vc(in, &ddata->channel1);
	if (r) {
		dev_err(dev, "failed to get virtual channel1\n");
		goto err_req_vc1;
	}
	r = in->ops->dsi.set_vc_id(in, ddata->channel1, 0);
	if (r) {
		dev_err(dev, "failed to set VC_ID1\n");
		goto err_vc_id1;
	}

	return 0;

err_vc_id1:
	in->ops->dsi.release_vc(in, ddata->channel1);
err_req_vc1:
err_vc_id0:
	in->ops->dsi.release_vc(in, ddata->channel0);
err_req_vc0:
	return r;
}

static void pdsivm_disconnect(struct omap_dss_device *in, struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	if (!omapdss_device_is_connected(dssdev))
		return;

	in->ops->dsi.release_vc(in, ddata->channel0);
	in->ops->dsi.release_vc(in, ddata->channel1);
}

/* Backlight device might be initialized later than the panel
 * 	but if we defer panel probe, omapdrm will never probe it again.
 * 	Return: 1 on successful fetch, 0 otherwise
 * 	device node is put as soon as backlight device is fetched
 * 	*/
static int fetch_backlight_device(struct panel_drv_data *ddata)
{
	if (ddata->backlight)
		return 1;

	if (!ddata->backlight_node) {
		dev_err(&ddata->pdev->dev, "no backlight node");
		return 0;
	}

	ddata->backlight = of_find_backlight_by_node(ddata->backlight_node);
	if (ddata->backlight) {
		dev_dbg(&ddata->pdev->dev, "got backlight device");
		of_node_put(ddata->backlight_node);
		return 1;
	}

	dev_warn(&ddata->pdev->dev, "still no backlight device");
	return 0;
}

static void backlight_fetch_work(struct work_struct *data)
{
	struct panel_drv_data *ddata = container_of(data, struct panel_drv_data,
								backlight_work.work);

	mutex_lock(&ddata->lock);

	if (!fetch_backlight_device(ddata)) {
		schedule_delayed_work(&ddata->backlight_work, HZ/2);
		goto out;
	}

	// update backlight status
	switch (ddata->dssdev.state) {
	case OMAP_DSS_DISPLAY_ACTIVE:
		ddata->backlight->props.power = FB_BLANK_UNBLANK;
		break;

	case OMAP_DSS_DISPLAY_DISABLED:
		ddata->backlight->props.power = FB_BLANK_POWERDOWN;
		break;
	}

	backlight_update_status(ddata->backlight);

out:
	mutex_unlock(&ddata->lock);
}

static int pdsivm_enable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->dssdev.src;
	int r;

	dev_dbg(&ddata->pdev->dev, "enable\n");

	if (!omapdss_device_is_connected(dssdev))
		return -ENODEV;

	if (omapdss_device_is_enabled(dssdev))
		return 0;

	mutex_lock(&ddata->lock);

	in->ops->dsi.bus_lock(in);

	r = pdsivm_power_on(ddata);

	in->ops->dsi.bus_unlock(in);

	if (r)
		dev_err(&ddata->pdev->dev, "Enable failed\n");
	else
		dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

	if (ddata->backlight) {
		ddata->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ddata->backlight);
	}

	mutex_unlock(&ddata->lock);

	dev_dbg(&ddata->pdev->dev, "enable done\n");

	return r;
}

static void pdsivm_disable(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);
	struct omap_dss_device *in = ddata->dssdev.src;

	dev_dbg(&ddata->pdev->dev, "disable\n");

	if (!omapdss_device_is_enabled(dssdev))
		return;

	mutex_lock(&ddata->lock);
	in->ops->dsi.bus_lock(in);

	pdsivm_power_off(ddata);

	in->ops->dsi.bus_unlock(in);

	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;

	if (ddata->backlight) {
		ddata->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ddata->backlight);
	}

	mutex_unlock(&ddata->lock);

	dev_dbg(&ddata->pdev->dev, "disable done\n");
}

static void pdsivm_set_timings(struct omap_dss_device *dssdev,
		const struct videomode *vm)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	ddata->vm = *vm;
}

static void pdsivm_get_timings(struct omap_dss_device *dssdev,
		struct videomode *vm)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	*vm = ddata->vm;
}

static int pdsivm_check_timings(struct omap_dss_device *dssdev,
		struct videomode *vm)
{
	return 0;
}

static struct omap_dss_device_ops pdsivm_dss_ops = {
	.connect	= pdsivm_connect,
	.disconnect	= pdsivm_disconnect,

	.enable		= pdsivm_enable,
	.disable	= pdsivm_disable,

	.set_timings	= pdsivm_set_timings,
	.get_timings	= pdsivm_get_timings,
	.check_timings	= pdsivm_check_timings,
};

static void pdsivm_get_size(struct omap_dss_device *dssdev,
		unsigned int *width, unsigned int *height)
{
	*width = 193;
	*height = 121;
}

static struct omap_dss_driver pdsivm_dss_driver = {
	.get_size = pdsivm_get_size,
};

/* static void pdsivm_hw_reset(struct omap_dss_device *dssdev)
{
	struct panel_drv_data *ddata = to_panel_data(dssdev);

	gpiod_set_value_cansleep(ddata->enable_gpio, 0);
	msleep(100);
	gpiod_set_value_cansleep(ddata->enable_gpio, 1);
	msleep(20);
}
*/

static int pdsivm_probe_of(struct platform_device *pdev)
{
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);
	struct device_node *node = pdev->dev.of_node;

	struct gpio_desc *gpio;

	dev_info(&pdev->dev, "probe of\n");

	gpio = devm_gpiod_get(&pdev->dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(gpio)) {
		dev_err(&pdev->dev, "Failed to parse enable gpio\n");
		return PTR_ERR(gpio);
	}
	ddata->enable_gpio = gpio;

	gpio = devm_gpiod_get(&pdev->dev, "cabc", GPIOD_OUT_LOW);
	if (IS_ERR(gpio)) {
		dev_err(&pdev->dev, "Failed to parse cabc gpio\n");
		return PTR_ERR(gpio);
	}
	ddata->cabc_gpio = gpio;

	ddata->backlight_node = of_parse_phandle(node, "backlight", 0);
	if (!fetch_backlight_device(ddata)) {
		INIT_DELAYED_WORK(&ddata->backlight_work, backlight_fetch_work);
		schedule_delayed_work(&ddata->backlight_work, HZ/2);
	}

	return 0;
}

static int pdsivm_probe(struct platform_device *pdev)
{
	struct panel_drv_data *ddata;
	struct device *dev = &pdev->dev;
	struct omap_dss_device *dssdev;
	int r;

	dev_dbg(dev, "probe\n");

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (ddata == NULL)
		return -ENOMEM;

	platform_set_drvdata(pdev, ddata);
	ddata->pdev = pdev;

	if (!pdev->dev.of_node)
		return -ENODEV;

	r = pdsivm_probe_of(pdev);
	if (r)
		return r;

	ddata->vm = nt71391_vm;

	dssdev = &ddata->dssdev;
	dssdev->dev = dev;
	dssdev->ops = &pdsivm_dss_ops;
	dssdev->driver = &pdsivm_dss_driver;
	dssdev->type = OMAP_DISPLAY_TYPE_DSI;
	dssdev->owner = THIS_MODULE;
	dssdev->of_ports = BIT(0);

	omapdss_display_init(dssdev);
	omapdss_device_register(dssdev);

	mutex_init(&ddata->lock);

	dev_dbg(dev, "probe done\n");

	return 0;
}

static int __exit pdsivm_remove(struct platform_device *pdev)
{
	struct panel_drv_data *ddata = platform_get_drvdata(pdev);
	struct omap_dss_device *dssdev = &ddata->dssdev;

	dev_dbg(&pdev->dev, "remove\n");

	cancel_delayed_work_sync(&ddata->backlight_work);

	omapdss_device_unregister(dssdev);

	pdsivm_disable(dssdev);
	omapdss_device_disconnect(dssdev->src, dssdev);

	return 0;
}

static int pdsivm_power_on(struct panel_drv_data *ddata)
{
	struct omap_dss_device *in = ddata->dssdev.src;
	u8 id1, id2, id3;
	int r;

/*	struct omap_dss_dsi_videomode_timings vm_timings = {
		.hsa				= 0,
		.hfp				= 27,
		.hbp				= 6,
		.vsa				= 1,
		.vfp				= 10,
		.vbp				= 9,
		.hact				= 1920,
		.vact				= 1200,
		.blanking_mode		= 1,
		.hsa_blanking_mode	= 1,
		.hbp_blanking_mode	= 1,
		.hfp_blanking_mode	= 1,
		.trans_mode = OMAP_DSS_DSI_BURST_MODE,
		.ddr_clk_always_on	= 0,
		.window_sync		= 4,
	};
*/

	struct omap_dss_dsi_config dsi_config = {
		.mode = OMAP_DSS_DSI_VIDEO_MODE,
		.pixel_format = OMAP_DSS_DSI_FMT_RGB666_PACKED,
		// .pixel_format = OMAP_DSS_DSI_FMT_RGB888,
		.vm = &ddata->vm,
		.hs_clk_min = 125000000,
		.hs_clk_max = 450000000,
		.lp_clk_min = 7000000,
		.lp_clk_max = 10000000,
		.ddr_clk_always_on = false,
		.trans_mode = OMAP_DSS_DSI_BURST_MODE,
	};

	dev_dbg(&ddata->pdev->dev, "power on\n");

	// power supply off
	gpiod_set_value_cansleep(ddata->cabc_gpio, 0);
	gpiod_set_value_cansleep(ddata->enable_gpio, 0);
	msleep(100);

	r = in->ops->dsi.set_config(in, &dsi_config);
	if (r) {
		dev_err(&ddata->pdev->dev, "Failed to configure DSI\n");
		goto err0;
	}

	r = in->ops->enable(in);
	if (r) {
		dev_err(&ddata->pdev->dev, "Failed to enable DSI\n");
		goto err0;
	}
	dev_dbg(&ddata->pdev->dev, "DSI enebled\n");

	// power supply on
	gpiod_set_value_cansleep(ddata->enable_gpio, 1);
	gpiod_set_value_cansleep(ddata->cabc_gpio, 1);
	msleep(120);

	dev_dbg(&ddata->pdev->dev, "Soft reset\n");
	r = pdsivm_dcs_write_1(ddata, 0x01, 0x00);
	if (r)	goto err;
	// msleep(50);

	dev_dbg(&ddata->pdev->dev, "Swing double mode\n");
	r = pdsivm_dcs_write_1(ddata, 0xae, 0x0d);
	if (r)	goto err;

	// dev_dbg(&ddata->pdev->dev, "Test mode 1\n");
	// r = pdsivm_dcs_write_1(ddata, 0xee, 0xea);
	// if (r)	goto err;

	// dev_dbg(&ddata->pdev->dev, "Test mode 2\n");
	// r = pdsivm_dcs_write_1(ddata, 0xef, 0x5f);
	// if (r)	goto err;

	// dev_dbg(&ddata->pdev->dev, "Bias\n");
	// r = pdsivm_dcs_write_1(ddata, 0xf2, 0x28);
	// if (r)	goto err;

	dev_dbg(&ddata->pdev->dev, "CABC\n");
	r = pdsivm_dcs_write_1(ddata, 0xb0, 0x7e);
	if (r)	goto err;

	// dev_dbg(&ddata->pdev->dev, "Enter BIST mode\n");
	// r = pdsivm_dcs_write_1(ddata, 0xB1, 0xEF);
	// if (r)
	// 	goto err;

	dev_dbg(&ddata->pdev->dev, "Clock 153\n");
	nt71391_set_clk_153(ddata);

	r = pdsivm_get_id(ddata, &id1, &id2, &id3);
	if (r)	goto err;

	dev_dbg(&ddata->pdev->dev, "Turn on periph\n");
	r = pdsivm_dcs_write_0(ddata, MIPI_DSI_TURN_ON_PERIPHERAL);
	if (r)	goto err;

	in->ops->dsi.enable_hs(in, ddata->channel0, true);
	in->ops->dsi.enable_hs(in, ddata->channel1, true);


/*	r = pdsivm_dcs_write_1(ddata, MIPI_DCS_SET_PIXEL_FORMAT,
		MIPI_DCS_PIXEL_FMT_18BIT);
	if (r)
		goto err;

	r = pdsivm_dcs_write_0(ddata, MIPI_DCS_SET_DISPLAY_ON);
	if (r)
		goto err;
*/

	r = in->ops->dsi.enable_video_output(in, ddata->channel0);
	if (r)	goto err;

	if (!ddata->intro_printed) {
		dev_dbg(&ddata->pdev->dev, "NT71391 Panel revision %02x.%02x.%02x\n",
			id1, id2, id3);
		ddata->intro_printed = true;
	}

	dev_dbg(&ddata->pdev->dev, "power on done\n");

	return 0;
err:
	dev_err(&ddata->pdev->dev, "error while enabling panel, issuing HW reset\n");

	in->ops->dsi.disable(in, false, false);
	gpiod_set_value_cansleep(ddata->cabc_gpio, 0);
	gpiod_set_value_cansleep(ddata->enable_gpio, 0);
	mdelay(20);

err0:
	return r;
}

static void pdsivm_power_off(struct panel_drv_data *ddata)
{
	struct omap_dss_device *in = ddata->dssdev.src;

	dev_dbg(&ddata->pdev->dev, "power off\n");

	in->ops->dsi.disable_video_output(in, ddata->channel0);
	in->ops->dsi.disable_video_output(in, ddata->channel1);

	in->ops->dsi.disable(in, false, false);
	mdelay(10);

	gpiod_set_value_cansleep(ddata->cabc_gpio, 0);
	gpiod_set_value_cansleep(ddata->enable_gpio, 0);

	mdelay(20);
}

static const struct of_device_id pdsivm_of_match[] = {
	{ .compatible = "omapdss,hydis,nt71391", },
	{},
};

MODULE_DEVICE_TABLE(of, pdsivm_of_match);

static struct platform_driver pdsivm_driver = {
	.probe = pdsivm_probe,
	.remove = __exit_p(pdsivm_remove),
	.driver = {
		.name = "hydis,nt71391",
		.of_match_table = pdsivm_of_match,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(pdsivm_driver);

MODULE_AUTHOR("Mykola Dolhyi <0xb000@gmail.com>");
MODULE_DESCRIPTION("DSI NT71391 Panel Driver");
MODULE_LICENSE("GPL");
