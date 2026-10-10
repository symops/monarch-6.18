// SPDX-License-Identifier: GPL-2.0-only
/*
 * Power-off for WD My Cloud Home (Monarch, RTD1295) and WD My Cloud Home
 * Duo (Pelican, RTD1296), as WD's 4.9 kernel does it: the audio CPU
 * firmware, asked to "suspend to coolboot", puts the G2227 PMIC to sleep,
 * and the PMIC then turns off the rails whose sleep mode the g2227
 * regulator driver set to off at shutdown. Before that this driver turns
 * off what the SoC drives directly and disarms the watchdog:
 *
 *  - PWM channel(s) for the SYS LED (and fan, Duo only): the OCD
 *    register (offset 0x0 within the pwm@d0 block) with 0 written to a
 *    channel's 8-bit field is this hardware's own encoding for
 *    "disabled" (see pwm-rtd129x.c's get_state(): "ocd == 0 is how
 *    apply() encodes disabled") -- the exact same effect pwm_disable()
 *    produces on that channel, just reached directly instead of through
 *    a pwm_device this driver doesn't own. This register packs all 4
 *    channels' OCD fields into one 32-bit word, so only 4 bytes need
 *    mapping regardless of how many channels are listed. Left as a raw
 *    devm_ioremap() poke (not devm_platform_ioremap_resource()) because
 *    the pwm@d0 block itself is already exclusively owned by the real
 *    pwm-rtd129x.c driver (pwm-leds/pwm-fan); a second exclusive claim
 *    on the same page would collide with it the same way an earlier
 *    mistake did in the Reset button work.
 *
 *  - USB VBUS, on boards where the physical port-power GPIO line(s) have
 *    been empirically confirmed: the only way this was ever pinned down
 *    was by reading the vendor's own rtk_usb_manager.c driver together
 *    with a captured *stock-firmware* boot log from the exact physical
 *    unit being ported (not the generic reference-board DTS, which
 *    turned out to list a different, incomplete GPIO set than what
 *    retail firmware actually uses on either board), then confirming
 *    each candidate line on real hardware by holding it low for several
 *    seconds -- a brief 1-second pulse, tried first, swept every single
 *    bit of misc-gpio (both 32-bit banks) and every rtk_iso_gpio line
 *    with zero visible effect on VBUS; a 4-second hold on the exact same
 *    lines that had just "failed" is what actually cut power, and
 *    restored it again when driven back high.
 *
 *    Duo's two external ports are both on rtk_iso_gpio (lines 34/26,
 *    bottom/top bay), requested as normal gpiod consumers --
 *    rtk_iso_gpio is already a proper mainline gpiolib controller
 *    (drivers/gpio/gpio-rtd.c) with no competing exclusive claim on
 *    either line (only line 20, the Reset button, is otherwise spoken
 *    for). Monarch's stock-firmware log shows a completely different
 *    split: one port on a single misc-gpio bit (19), the other two
 *    (sharing one physical port) on rtk_iso_gpio line 1. All of them are
 *    wd,usb-vbus-gpios: this driver holds them high while the system runs
 *    and drives them low at power-off.
 *
 * Real disk spin-down is handled separately, through the SCSI layer's
 * own existing sd_shutdown() mechanism (the manage_shutdown sysfs
 * attribute, enabled via udev at boot) rather than reimplemented here;
 * ahci_rtd1295 then turns off the disk's supply.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

#define WDMC_PWM_OCD		0x0
#define WDMC_MAX_PWM_CHANNELS	4

#define WDMC_WDT_EN_MASK	0xff
#define WDMC_WDT_DISABLED	0xa5

#define WDMC_UART_MCR		0x10
#define WDMC_UART_LSR		0x14
#define WDMC_UART_MCR_LOOP	BIT(4)
#define WDMC_UART_LSR_TEMT	BIT(6)

/* struct rtk_ipc_shm (WD 4.9 rtk_ipc_shm.h) at 0xc4 into the RPC page, big-endian */
#define WDMC_IPC_AUDIO_RPC_FLAG	(0xc4 + 0x0c)
#define WDMC_IPC_SUSPEND_MASK	(0xc4 + 0x10)
#define WDMC_IPC_SUSPEND_FLAG	(0xc4 + 0x14)

struct wdmc_poweroff_data {
	void __iomem *pwm_base;
	struct gpio_descs *usb_vbus_gpios;
	void __iomem *wdt_ctl;
	void __iomem *uart;
	void __iomem *ipc;
	unsigned int pwm_channels[WDMC_MAX_PWM_CHANNELS];
	unsigned int n_pwm_channels;
};

static struct wdmc_poweroff_data *wdmc_poweroff;

static void wdmc_poweroff_handler(void)
{
	unsigned int i;
	u32 val;

	if (!wdmc_poweroff)
		return;

	for (i = 0; i < wdmc_poweroff->n_pwm_channels; i++) {
		unsigned int shift = wdmc_poweroff->pwm_channels[i] * 8;

		val = readl(wdmc_poweroff->pwm_base + WDMC_PWM_OCD);
		val &= ~(0xffU << shift);
		writel(val, wdmc_poweroff->pwm_base + WDMC_PWM_OCD);
	}

	if (wdmc_poweroff->usb_vbus_gpios) {
		/*
		 * Confirmed on real hardware that this needs to actually
		 * be held low, not just pulsed -- the machine parks (IRQs
		 * off, CPUs stopped) shortly after this returns, which is
		 * exactly what leaves it held indefinitely from here on.
		 */
		for (i = 0; i < wdmc_poweroff->usb_vbus_gpios->ndescs; i++)
			gpiod_direction_output(wdmc_poweroff->usb_vbus_gpios->desc[i], 0);
	}

	if (wdmc_poweroff->wdt_ctl) {
		val = readl(wdmc_poweroff->wdt_ctl) & ~WDMC_WDT_EN_MASK;
		writel(val | WDMC_WDT_DISABLED, wdmc_poweroff->wdt_ctl);
	}

	if (wdmc_poweroff->ipc) {
		void __iomem *ipc = wdmc_poweroff->ipc;
		void __iomem *uart = wdmc_poweroff->uart;

		/* The firmware prints on UART0 before taking the request: loop it back to drain */
		if (uart) {
			for (i = 0; i < 10000; i++) {
				if (readl(uart + WDMC_UART_LSR) & WDMC_UART_LSR_TEMT)
					break;
				udelay(10);
			}
			val = readl(uart + WDMC_UART_MCR);
			writel(val | WDMC_UART_MCR_LOOP, uart + WDMC_UART_MCR);
		}

		/* Suspend version 2, author SCPU, coolboot; the firmware clears the request */
		iowrite32be(0x00020000, ipc + WDMC_IPC_SUSPEND_MASK);
		iowrite32be(0x40000002, ipc + WDMC_IPC_SUSPEND_FLAG);
		iowrite32be(0xdeadffff, ipc + WDMC_IPC_AUDIO_RPC_FLAG);
		for (i = 0; i < 1000 && ioread32be(ipc + WDMC_IPC_AUDIO_RPC_FLAG); i++)
			mdelay(1);
		pr_emerg("wdmc-poweroff: the audio CPU %s the power-off request\n",
			 i < 1000 ? "took" : "did not take");
	}

	for (;;)
		wfi();
}

static int wdmc_poweroff_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wdmc_poweroff_data *data;
	struct device_node *rmem;
	struct resource *res;
	int ret, i;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	data->pwm_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!data->pwm_base)
		return -ENOMEM;

	/* reg[1] watchdog control, reg[2] UART0: both optional */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res && !(data->wdt_ctl = devm_ioremap(dev, res->start, resource_size(res))))
		return -ENOMEM;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (res && !(data->uart = devm_ioremap(dev, res->start, resource_size(res))))
		return -ENOMEM;

	/* The RPC page must be no-map: the audio CPU reads it uncached */
	rmem = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (rmem) {
		struct resource ipc;

		ret = of_address_to_resource(rmem, 0, &ipc);
		of_node_put(rmem);
		if (ret)
			return dev_err_probe(dev, ret, "bad memory-region\n");
		data->ipc = devm_ioremap(dev, ipc.start, resource_size(&ipc));
		if (!data->ipc)
			return dev_err_probe(dev, -ENOMEM, "cannot map memory-region\n");
	}

	/* U-Boot powers USB only when it boots from USB, so raise VBUS here */
	data->usb_vbus_gpios = devm_gpiod_get_array_optional(dev, "wd,usb-vbus",
							       GPIOD_OUT_HIGH);
	if (IS_ERR(data->usb_vbus_gpios))
		return dev_err_probe(dev, PTR_ERR(data->usb_vbus_gpios),
				      "failed to get usb-vbus gpios\n");

	ret = of_property_count_u32_elems(dev->of_node, "wd,pwm-off-channels");
	if (ret < 0 || ret > WDMC_MAX_PWM_CHANNELS)
		return dev_err_probe(dev, -EINVAL,
				      "bad or missing wd,pwm-off-channels\n");
	data->n_pwm_channels = ret;

	for (i = 0; i < ret; i++) {
		u32 ch;

		of_property_read_u32_index(dev->of_node, "wd,pwm-off-channels", i, &ch);
		if (ch >= WDMC_MAX_PWM_CHANNELS)
			return dev_err_probe(dev, -EINVAL,
					      "channel %u out of range\n", ch);
		data->pwm_channels[i] = ch;
	}

	if (pm_power_off)
		return dev_err_probe(dev, -EBUSY,
				      "pm_power_off already claimed\n");

	wdmc_poweroff = data;
	pm_power_off = wdmc_poweroff_handler;
	platform_set_drvdata(pdev, data);

	dev_info(dev, "registered as pm_power_off (%u pwm channel(s), %u usb vbus gpio(s)%s)\n",
		 data->n_pwm_channels,
		 data->usb_vbus_gpios ? data->usb_vbus_gpios->ndescs : 0,
		 data->ipc ? ", audio CPU coolboot" : "");

	return 0;
}

static void wdmc_poweroff_remove(struct platform_device *pdev)
{
	if (pm_power_off == wdmc_poweroff_handler) {
		pm_power_off = NULL;
		wdmc_poweroff = NULL;
	}
}

static const struct of_device_id wdmc_poweroff_of_match[] = {
	{ .compatible = "wd,mycloud-home-poweroff" },
	{ }
};
MODULE_DEVICE_TABLE(of, wdmc_poweroff_of_match);

static struct platform_driver wdmc_poweroff_driver = {
	.probe = wdmc_poweroff_probe,
	.remove = wdmc_poweroff_remove,
	.driver = {
		.name = "wdmc-poweroff",
		.of_match_table = wdmc_poweroff_of_match,
	},
};
module_platform_driver(wdmc_poweroff_driver);

MODULE_DESCRIPTION("Cosmetic power-off (LED/fan/USB VBUS) for WD My Cloud Home / Duo");
MODULE_LICENSE("GPL");
