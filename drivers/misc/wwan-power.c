/*
 * wwan_on_off
 * driver for controlling power states of some WWAN modules
 * like the GTM601 or the PHS8 which are independently powered
 * from the APU so that they can continue to run during suspend
 * and potentially during power-off.
 *
 * Such modules usually have some ON_KEY or IGNITE input
 * that can be triggered to turn the modem power on or off
 * by giving a sufficiently long (200ms) impulse.
 *
 * Some modules have a power-is-on feedback that can be fed
 * into another GPIO so that the driver knows the real state.
 *
 * If this is not available we can monitor some USB PHY
 * port which becomes active if the modem is powered on.
 *
 * The driver is based on the w2sg0004 driver developed
 * by Neil Brown.
 */

#define DEBUG

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/rfkill.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/usb/phy.h>
#include <linux/workqueue.h>


struct wwan_on_off {
	struct regulator  *vcc_regulator;
	struct rfkill *rf_kill;
	struct gpio_desc  *on_off_gpio; /* may be invalid */
	struct gpio_desc  *feedback_gpio; /* may be invalid */
	struct gpio_desc  *sim_gpio;
	struct gpio_desc  *reset_gpio;
	struct gpio_desc  *usb_en_gpio;
	// struct gpio_desc  *host_wake_gpio;
	struct usb_phy  *usb_phy; /* USB PHY to monitor for modem activity */
	bool    is_power_on;  /* current state */
	bool    can_turnoff;  /* can also turn off by impulse */
};

static bool wwan_on_off_is_powered_on(struct wwan_on_off *wwan)
{
	/* check with physical interfaces if possible */
	if (!IS_ERR_OR_NULL(wwan->feedback_gpio)) {
		pr_debug("%s: gpio value = %d\n", __func__,
			gpiod_get_value_cansleep(wwan->feedback_gpio));

		return gpiod_get_value_cansleep(wwan->feedback_gpio);
	}

	if (!IS_ERR_OR_NULL(wwan->usb_phy)) {
		printk("%s: USB phy event %d\n", __func__, wwan->usb_phy->last_event);
		/* check with PHY if available */
	}

	if (IS_ERR_OR_NULL(wwan->on_off_gpio)) {
		pr_debug("%s: on-off invalid\n", __func__);
		pr_debug("%s: return 'true'\n", __func__);

		return true;  /* we can't even control power, assume it is on */
	}

	pr_debug("%s: we assume %d\n", __func__, wwan->is_power_on);
	pr_debug("%s: return '%s'\n", __func__, wwan->is_power_on?"true":"false");

	return wwan->is_power_on; /* assume that we know the correct state */
}

static void wwan_on_off_set_power(struct wwan_on_off *wwan, bool on)
{
	int state;
	int ret;

	pr_debug("%s:on = %d\n", __func__, on);

	if (IS_ERR_OR_NULL(wwan->on_off_gpio))
		return; /* we can't control power */

	state = wwan_on_off_is_powered_on(wwan);

	pr_debug("%s: state %d\n", __func__, state);
	if (!IS_ERR_OR_NULL(wwan->vcc_regulator))
		pr_debug("%s: regulator %d\n", __func__, regulator_is_enabled(wwan->vcc_regulator));

	if(state != on) {
		if (on && !IS_ERR_OR_NULL(wwan->vcc_regulator) && !regulator_is_enabled(wwan->vcc_regulator)) {
			ret = regulator_enable(wwan->vcc_regulator);
			gpiod_set_value_cansleep(wwan->reset_gpio, 0);	/* deassert pmic reset */
			msleep(200); // min 20ms

			gpiod_set_value_cansleep(wwan->usb_en_gpio, 1);
		}

		if (!on && !wwan->can_turnoff) {
			pr_info("%s: can't turn off by impulse\n", __func__);
			if (!IS_ERR_OR_NULL(wwan->vcc_regulator) && regulator_is_enabled(wwan->vcc_regulator))
				regulator_disable(wwan->vcc_regulator);
			return;
		}

		pr_debug("%s: send impulse\n", __func__);
		gpiod_set_value_cansleep(wwan->on_off_gpio, 1);
		msleep(600);
		gpiod_set_value_cansleep(wwan->on_off_gpio, 0);

		msleep(on ? 6000 : 1000); // TBD

		if (!on) {
			gpiod_set_value_cansleep(wwan->usb_en_gpio, 0);
			msleep(10);
			gpiod_set_value_cansleep(wwan->reset_gpio, 1);
			msleep(2500);
			gpiod_set_value_cansleep(wwan->reset_gpio, 0);

			regulator_disable(wwan->vcc_regulator);
		}

		wwan->is_power_on = on;
		if (wwan_on_off_is_powered_on(wwan) != on) {
			pr_err("%s: failed to change modem state\n", __func__); /* warning only! using USB feedback might not be immediate */
			if (!IS_ERR_OR_NULL(wwan->vcc_regulator) && regulator_is_enabled(wwan->vcc_regulator))
				regulator_disable(wwan->vcc_regulator);
		}
	}

	pr_debug("%s: done\n", __func__);
}

static int wwan_on_off_rfkill_set_block(void *data, bool blocked)
{
	struct wwan_on_off *wwan = data;
	int ret = 0;

	pr_debug("%s: blocked: %d\n", __func__, blocked);
	if (IS_ERR_OR_NULL(wwan->on_off_gpio))
		return -EIO;  /* can't block if we have no control */

	wwan_on_off_set_power(wwan, !blocked);
	return ret;
}

static struct rfkill_ops wwan_on_off_rfkill_ops = {
	// get status to read feedback gpio as HARD block (?)
	.set_block = wwan_on_off_rfkill_set_block,
};

static int wwan_on_off_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wwan_on_off *wwan;
	struct rfkill *rf_kill;
	int err;

	pr_debug("%s: wwan_on_off_probe()\n", __func__);

	if (!pdev->dev.of_node)
		return -EINVAL;

	wwan = devm_kzalloc(dev, sizeof(*wwan), GFP_KERNEL);
	if (wwan == NULL)
		return -ENOMEM;

	platform_set_drvdata(pdev, wwan);

	wwan->on_off_gpio = devm_gpiod_get(&pdev->dev, "on-off", GPIOD_OUT_LOW);
	if (IS_ERR_OR_NULL(wwan->on_off_gpio)) {
		/* defer until we have the gpio */
		if (PTR_ERR(wwan->on_off_gpio) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
	}

	wwan->feedback_gpio = devm_gpiod_get(&pdev->dev, "feedback", GPIOD_IN);
	wwan->sim_gpio = devm_gpiod_get(&pdev->dev, "sim-present", GPIOD_IN);
	wwan->reset_gpio = devm_gpiod_get(&pdev->dev, "reset", GPIOD_OUT_HIGH);
	wwan->usb_en_gpio = devm_gpiod_get(&pdev->dev, "usb-en", GPIOD_OUT_HIGH);

	// wwan->host_wake_gpio = devm_gpiod_get(&pdev->dev, "host-wake", GPIOD_IN);

	wwan->vcc_regulator = devm_regulator_get_optional(&pdev->dev, "modem");
	if (IS_ERR_OR_NULL(wwan->vcc_regulator)) {
		/* defer until we can get the regulator */
		if (PTR_ERR(wwan->vcc_regulator) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		wwan->vcc_regulator = NULL; /* ignore other errors */
	}

	wwan->usb_phy = devm_usb_get_phy_by_phandle(dev, "usb-port", 0);
	pr_info("%s: onoff = %p indicator = %p usb_phy = %lu\n",
		__func__, wwan->on_off_gpio, wwan->feedback_gpio,
		PTR_ERR(wwan->usb_phy));
	// get optional reference to USB PHY (through "usb-port")

	pr_debug("%s: wwan_on_off_probe() wwan=%p\n", __func__, wwan);

	// FIXME: read from of_device_id table
	wwan->can_turnoff = of_property_read_bool(dev->of_node, "can-turnoff");
	wwan->is_power_on = false;  /* assume initial power is off */

	rf_kill = rfkill_alloc("WWAN", &pdev->dev,
				RFKILL_TYPE_WWAN,
				&wwan_on_off_rfkill_ops, wwan);
	if (rf_kill == NULL) {
		return -ENOMEM;
	}

	rfkill_init_sw_state(rf_kill, !wwan_on_off_is_powered_on(wwan));

	err = rfkill_register(rf_kill);
	if (err) {
		dev_err(&pdev->dev, "Cannot register rfkill device\n");
		goto err;
	}

	wwan->rf_kill = rf_kill;

	pr_debug("%s: successfully probed\n", __func__);

	return 0;

err:
	rfkill_destroy(rf_kill);
	pr_debug("%s: probe failed %d\n", __func__, err);

	return err;
}

static int wwan_on_off_remove(struct platform_device *pdev)
{
	struct wwan_on_off *wwan = platform_get_drvdata(pdev);
	return 0;
}

/* we only suspend the driver (i.e. set the gpio in a state
 * that it does not harm)
 * the reason is that the modem must continue to be powered
 * on to receive SMS and incoming calls that wake up the CPU
 * through a wakeup GPIO
 */

static int wwan_on_off_suspend(struct device *dev)
{
	struct wwan_on_off *wwan = dev_get_drvdata(dev);
	pr_debug("%s: WWAN suspend\n", __func__);

	/* set gpio to harmless mode */
	return 0;
}

static int wwan_on_off_resume(struct device *dev)
{
	struct wwan_on_off *wwan = dev_get_drvdata(dev);
	pr_debug("%s: WWAN resume\n", __func__);

	/* restore gpio */
	return 0;
}

/* on system power off we must turn off the
 * modem (which has a separate connection to
 * the battery).
 */

static int wwan_on_off_poweroff(struct device *dev)
{
	struct wwan_on_off *wwan = dev_get_drvdata(dev);

	pr_debug("%s: WWAN poweroff\n", __func__);

	wwan_on_off_set_power(wwan, 0); /* turn off modem */
	pr_info("%s: WWAN powered off\n", __func__);

	return 0;
}

static const struct of_device_id wwan_of_match[] = {
	{ .compatible = "option,gtm601-power" },
	{ .compatible = "gemalto,phs8-power" },
	{ .compatible = "gemalto,pls8-power" },
	{ .compatible = "folksy,3rn13-power" },
	{},
};
MODULE_DEVICE_TABLE(of, wwan_of_match);

const struct dev_pm_ops wwan_on_off_pm_ops = {
	.suspend = wwan_on_off_suspend,
	.resume = wwan_on_off_resume,
	.freeze = wwan_on_off_suspend,
	.thaw = wwan_on_off_resume,
	.poweroff = wwan_on_off_poweroff,
	.restore = wwan_on_off_resume,
	};

static struct platform_driver wwan_on_off_driver = {
	.driver.name  = "wwan-on-off",
	.driver.owner = THIS_MODULE,
	.driver.pm  = &wwan_on_off_pm_ops,
	.driver.of_match_table = of_match_ptr(wwan_of_match),
	.probe    = wwan_on_off_probe,
	.remove   = wwan_on_off_remove,
};

static int __init wwan_on_off_init(void)
{
	pr_debug("%s: wwan_on_off_init\n", __func__);
	return platform_driver_register(&wwan_on_off_driver);
}
module_init(wwan_on_off_init);

static void __exit wwan_on_off_exit(void)
{
	platform_driver_unregister(&wwan_on_off_driver);
}
module_exit(wwan_on_off_exit);


MODULE_ALIAS("wwan_on_off");

MODULE_AUTHOR("Nikolaus Schaller <hns@goldelico.com>");
MODULE_DESCRIPTION("3G Modem rfkill and virtual GPIO driver");
MODULE_LICENSE("GPL v2");