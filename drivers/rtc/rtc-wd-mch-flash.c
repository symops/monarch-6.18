// SPDX-License-Identifier: GPL-2.0-only
/*
 * WD My Cloud Home "software RTC" -- persists the clock in SPI-NOR flash
 * instead of a battery-backed hardware RTC (this board has none).
 *
 * Forward-ported from the vendor 4.9.330 tree's drivers/rtc/rtc-rtk.c
 * (rtk_rtc_gettime_virtual()/rtk_rtc_settime_virtual()). The vendor driver
 * also drove real "realtek,rtd1295-rtc" hardware registers for setup and
 * alarm support, but read_time/set_time -- the only thing CONFIG_RTC_
 * HCTOSYS/SYSTOHC actually rely on -- went exclusively through this flash
 * path. That hardware-register half is dropped here: mainline already has
 * a "realtek,rtd1295-rtc" driver (drivers/rtc/rtc-rtd119x.c) for the real
 * counter, and this driver deliberately uses a different DT compatible so
 * it doesn't compete with it for the binding -- it fully replaces it as
 * the board's rtc0 by being the one referenced from the board DTS's rtc
 * node, since the real hardware counter free-runs from an arbitrary reset
 * value with no battery backup and is useless for keeping actual time
 * across a power cycle.
 *
 * Storage format (unchanged from the vendor driver, to keep the flash
 * layout compatible with the vendor firmware if this device is ever
 * dual-booted back to it): 8 bytes at offset 0 of mtd0 -- the whole raw
 * SPI-NOR chip, unpartitioned. u32 seconds, then u32 (seconds ^ 0xffffffff)
 * as a checksum. mtd0 offset 0 was confirmed on real hardware (via a live
 * hexdump) to already hold a valid, plausible-recent timestamp pair before
 * this driver was ever used -- it is the vendor's own dedicated scratch
 * area, not the start of the signed FSBL image the BootROM validates.
 */

#include <linux/delay.h>
#include <linux/mtd/mtd.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>

/* Vendor comment: "read 2 bytes from spi flash, if read > 2 bytes, system
 * will kernel panic" -- "2 bytes" there means 2 u32s, i.e. this exact
 * 8-byte struct; kept as-is rather than re-verified on real hardware.
 */
struct wd_rtc_blob {
	u32 seconds;
	u32 seconds_inv;
};

#define WD_RTC_MTD_INDEX	0
#define WD_RTC_MTD_OFFSET	0
#define WD_RTC_WAIT_MTD_RETRIES	1000
#define WD_RTC_WAIT_MTD_DELAY_MS 10
#define WD_RTC_DEFAULT_YEAR	2021

/*
 * set_time() erases the whole sector on every call, and CONFIG_RTC_SYSTOHC
 * drives it every 11 minutes while the clock stays NTP-synchronised -- about
 * 131 erases/day, enough to wear out this one-sector-of-flash RTC storage
 * (100k erase cycles rated) in roughly two years. The flash only ever holds
 * the time of the last write, not a running clock (a power cut already
 * loses the time the board was off), so writing far less often costs
 * nothing real: throttle to once a day. (The vendor driver had the same
 * throttle, via FLASH_WRITE_LIMIT -- just set to 0 there.)
 */
#define WD_RTC_WRITE_INTERVAL_SEC (24ULL * 60 * 60)

static time64_t wd_rtc_last_write;

static int wd_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct wd_rtc_blob blob = { 0 };
	struct mtd_info *mtd;
	size_t retlen;
	int ret;
	time64_t time;

	mtd = get_mtd_device(NULL, WD_RTC_MTD_INDEX);
	if (IS_ERR(mtd)) {
		dev_err(dev, "cannot get mtd device: %ld\n", PTR_ERR(mtd));
		return PTR_ERR(mtd);
	}

	ret = mtd_read(mtd, WD_RTC_MTD_OFFSET, sizeof(blob), &retlen,
		       (u8 *)&blob);
	put_mtd_device(mtd);
	if (ret && !mtd_is_bitflip(ret))
		return ret;
	if (retlen != sizeof(blob))
		return -EIO;

	if ((blob.seconds ^ blob.seconds_inv) == UINT_MAX) {
		time = blob.seconds;
	} else {
		dev_warn(dev, "invalid time in flash, using default\n");
		time = mktime64(WD_RTC_DEFAULT_YEAR, 1, 1, 0, 0, 0);
	}

	rtc_time64_to_tm(time, tm);
	return 0;
}

static int wd_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct wd_rtc_blob blob;
	struct mtd_info *mtd;
	struct erase_info ei = { 0 };
	size_t retlen;
	time64_t now;
	int ret;

	now = rtc_tm_to_time64(tm);
	if (wd_rtc_last_write &&
	    now - wd_rtc_last_write < WD_RTC_WRITE_INTERVAL_SEC)
		return 0;

	mtd = get_mtd_device(NULL, WD_RTC_MTD_INDEX);
	if (IS_ERR(mtd)) {
		dev_err(dev, "cannot get mtd device: %ld\n", PTR_ERR(mtd));
		return PTR_ERR(mtd);
	}

	blob.seconds = (u32)now;
	blob.seconds_inv = blob.seconds ^ UINT_MAX;

	ei.addr = WD_RTC_MTD_OFFSET;
	ei.len = mtd->erasesize;

	ret = mtd_erase(mtd, &ei);
	if (ret) {
		dev_err(dev, "flash erase failed: %d\n", ret);
		goto out;
	}

	ret = mtd_write(mtd, WD_RTC_MTD_OFFSET, sizeof(blob), &retlen,
			(u8 *)&blob);
	if (ret || retlen != sizeof(blob)) {
		dev_err(dev, "flash write failed: %d\n", ret);
		if (!ret)
			ret = -EIO;
	} else {
		wd_rtc_last_write = now;
	}

out:
	put_mtd_device(mtd);
	return ret;
}

static const struct rtc_class_ops wd_rtc_ops = {
	.read_time = wd_rtc_read_time,
	.set_time  = wd_rtc_set_time,
};

static int wd_rtc_wait_mtd_ready(struct device *dev)
{
	struct mtd_info *mtd;
	int retry = WD_RTC_WAIT_MTD_RETRIES;

	while (retry--) {
		mtd = get_mtd_device(NULL, WD_RTC_MTD_INDEX);
		if (!IS_ERR(mtd)) {
			put_mtd_device(mtd);
			return 0;
		}
		msleep(WD_RTC_WAIT_MTD_DELAY_MS);
	}

	dev_err(dev, "timed out waiting for mtd%d\n", WD_RTC_MTD_INDEX);
	return -ENODEV;
}

static int wd_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtc_device *rtc;
	int ret;

	ret = wd_rtc_wait_mtd_ready(dev);
	if (ret)
		return ret;

	rtc = devm_rtc_device_register(dev, "wd-mch-rtc", &wd_rtc_ops,
					THIS_MODULE);
	if (IS_ERR(rtc)) {
		dev_err(dev, "cannot attach rtc: %ld\n", PTR_ERR(rtc));
		return PTR_ERR(rtc);
	}

	return 0;
}

static const struct of_device_id wd_rtc_of_match[] = {
	{ .compatible = "wd,mycloud-home-rtc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, wd_rtc_of_match);

static struct platform_driver wd_rtc_driver = {
	.probe = wd_rtc_probe,
	.driver = {
		.name = "wd-mch-rtc",
		.of_match_table = wd_rtc_of_match,
	},
};
module_platform_driver(wd_rtc_driver);

MODULE_DESCRIPTION("WD My Cloud Home flash-backed software RTC");
MODULE_LICENSE("GPL");
