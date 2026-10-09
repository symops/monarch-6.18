// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTD129x SPI-NOR flash controller (SFC) driver
 *
 * Forward-ported from the vendor 4.9.330 tree's
 * drivers/mtd/spi-nor/rtk-sfc.c to the modern spi_nor_controller_ops API
 * (mainline 6.x moved prepare()/unprepare() to take a single struct
 * spi_nor * argument, dropping the per-op enum spi_nor_ops parameter --
 * the vendor code used that parameter only to toggle the SFC's
 * auto-write mode, which is now toggled directly inside the write()
 * callback instead, matching the read-mode restore the vendor write()
 * already did at its end).
 *
 * This is the only SPI-NOR chip on the board: single chip-select, no
 * per-child DT node (matches the vendor DTS: a single "sfc@9801a800"
 * node with no children, registered as one hardcoded flash below).
 *
 * The vendor's inter-processor "lockapi" (coordinating SFC access with
 * the ACPU/RPC firmware) is dropped -- this port never loads that
 * firmware, so there is nothing else touching the flash controller.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/spi-nor.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define SFC_OPCODE		0x00
#define SFC_CTL			0x04
#define SFC_SCK			0x08
#define SFC_CE			0x0c
#define SFC_WP			0x10
#define SFC_POS_LATCH		0x14
#define SFC_WAIT_WR		0x18
#define SFC_EN_WR		0x1c
#define SFC_FAST_RD		0x20
#define SFC_SCK_TAP		0x24
#define SFC_OPCODE2		0x28

/* RTD129x DMA ("MD") block used for bulk SFC transfers. */
#define MD_BASE_ADDR		0x9800b000
#define MD_FDMA_DDR_SADDR	0x88
#define MD_FDMA_FL_SADDR	0x8c
#define MD_FDMA_CTRL2		0x90
#define MD_FDMA_CTRL1		0x94

#define RTKSFC_DMA_MAX_LEN	0x100
#define RTKSFC_WAIT_TIMEOUT	1000000

#define NOR_BASE_PHYS		0x88100000

/* RTD129x-specific CRT setup bit the vendor pokes before touching the SFC. */
#define RTD129X_SFC_CRT_REG	0x9801a914

struct rtksfc_priv {
	struct rtksfc_host *host;
};

struct rtksfc_host {
	struct device *dev;
	struct mutex lock;
	void __iomem *regbase;
	void __iomem *iobase;
	void __iomem *mdbase;
	void *buffer;
	dma_addr_t dma_buffer;
	struct spi_nor nor;
};

static int rtk_spi_nor_read_status(struct rtksfc_host *host)
{
	int i = 100;

	while (i--) {
		writel(0x05, host->regbase + SFC_OPCODE);
		udelay(50);
		writel(0x10, host->regbase + SFC_CTL);

		if (readb(host->iobase) & 0x1)
			msleep(100);
		else
			return 0;
	}

	return -ETIMEDOUT;
}

static void rtk_spi_nor_read_mode(struct rtksfc_host *host)
{
	writel(0x03, host->regbase + SFC_OPCODE);
	udelay(50);
	writel(0x18, host->regbase + SFC_CTL);
	readl(host->iobase);
}

static void rtk_spi_nor_write_mode(struct rtksfc_host *host)
{
	writel(0x02, host->regbase + SFC_OPCODE);
	udelay(50);
	writel(0x18, host->regbase + SFC_CTL);
}

static void rtk_spi_nor_enable_auto_write(struct rtksfc_host *host)
{
	writel(0x105, host->regbase + SFC_WAIT_WR);
	writel(0x106, host->regbase + SFC_EN_WR);
	writel(0x001A1307, host->regbase + SFC_CE);
}

static void rtk_spi_nor_disable_auto_write(struct rtksfc_host *host)
{
	writel(0x005, host->regbase + SFC_WAIT_WR);
	writel(0x006, host->regbase + SFC_EN_WR);
}

static void rtk_spi_nor_init(struct rtksfc_host *host)
{
	void __iomem *reg;

	reg = ioremap(RTD129X_SFC_CRT_REG, 0x4);
	if (reg) {
		writel(readl(reg) | 0x00000001, reg);
		iounmap(reg);
	}

	writel(0x00000013, host->regbase + SFC_SCK);
	writel(0x001a1307, host->regbase + SFC_CE);
	writel(0x00000000, host->regbase + SFC_POS_LATCH);
	writel(0x00000005, host->regbase + SFC_WAIT_WR);
	writel(0x00000006, host->regbase + SFC_EN_WR);
}

static int rtk_spi_nor_prep(struct spi_nor *nor)
{
	struct rtksfc_priv *priv = nor->priv;
	struct rtksfc_host *host = priv->host;

	mutex_lock(&host->lock);
	rtk_spi_nor_disable_auto_write(host);
	return 0;
}

static void rtk_spi_nor_unprep(struct spi_nor *nor)
{
	struct rtksfc_priv *priv = nor->priv;
	struct rtksfc_host *host = priv->host;

	mutex_unlock(&host->lock);
}

static int rtk_spi_nor_read_reg(struct spi_nor *nor, u8 opcode, u8 *buf,
				 size_t len)
{
	struct rtksfc_priv *priv = nor->priv;
	struct rtksfc_host *host = priv->host;
	u32 val;

	writel(opcode, host->regbase + SFC_OPCODE);
	udelay(50);

	switch (opcode) {
	case SPINOR_OP_RDID:
	case SPINOR_OP_RDSR:
		/*
		 * The whole (up to 4-byte) response is latched by a single
		 * SFC_CTL=0x10 pulse and comes back packed into one 32-bit
		 * little-endian read of iobase -- not one byte per read/
		 * pulse. Confirmed by hand on real hardware via devmem:
		 * `devmem iobase 8` after one pulse reads 0xef (just the low
		 * byte) every time, re-pulsing SFC_CTL changes nothing, but
		 * `devmem iobase 32` reads 0x001440ef -- ef 40 14 in byte
		 * order, the real JEDEC ID for this board's flash (matches
		 * the FSBL's own "nor flash id [0x00ef4014]"). RDID asks for
		 * SPI_NOR_MAX_ID_LEN (6) bytes but the hardware only ever
		 * has 4 to give; any bytes beyond that are left as-is by the
		 * caller-zeroed id buffer, same as the vendor driver's
		 * unconditional `len`-byte copy did (RDSR only ever wants 1).
		 */
		writel(0x00000010, host->regbase + SFC_CTL);
		/*
		 * Discard the first read, as the vendor driver does: right
		 * after boot it can return a stale byte ahead of the real
		 * response (seen on a GigaDevice GD25Q80, answering
		 * "c0 c8 40 14" instead of "c8 40 14" on the very first RDID
		 * after the boot loader).
		 */
		readl(host->iobase);
		val = readl(host->iobase);
		memcpy(buf, &val, min_t(size_t, len, sizeof(val)));
		break;
	default:
		dev_warn(nor->dev, "read_reg: unknown opcode 0x%02x\n", opcode);
		return -EINVAL;
	}

	return 0;
}

static int rtk_spi_nor_write_reg(struct spi_nor *nor, u8 opcode,
				  const u8 *buf, size_t len)
{
	struct rtksfc_priv *priv = nor->priv;
	struct rtksfc_host *host = priv->host;

	writel(opcode, host->regbase + SFC_OPCODE);
	udelay(50);

	switch (opcode) {
	case SPINOR_OP_WRSR:
		writel(0x10, host->regbase + SFC_CTL);
		writeb(buf[0], host->iobase);
		break;
	case SPINOR_OP_WREN:
	case SPINOR_OP_WRDI:
		writel(0x0, host->regbase + SFC_CTL);
		readb(host->iobase);
		break;
	case SPINOR_OP_EN4B:
		writel(0x0, host->regbase + SFC_CTL);
		readl(host->iobase);
		writel(0x1, host->regbase + SFC_OPCODE2);
		break;
	case SPINOR_OP_EX4B:
		writel(0x0, host->regbase + SFC_CTL);
		readl(host->iobase);
		writel(0x0, host->regbase + SFC_OPCODE2);
		break;
	case SPINOR_OP_CHIP_ERASE:
		dev_info(nor->dev, "erasing whole flash\n");
		writel(0x0, host->regbase + SFC_CTL);
		readb(host->iobase);
		return rtk_spi_nor_read_status(host);
	default:
		dev_warn(nor->dev, "write_reg: unknown opcode 0x%02x\n", opcode);
		return -EINVAL;
	}

	return 0;
}

static int rtk_spi_nor_dma_transfer(struct rtksfc_host *host, loff_t offset,
				     size_t len, bool write)
{
	u32 val;

	writel(0x0a, host->mdbase + MD_FDMA_CTRL1);

	writel((u32)host->dma_buffer, host->mdbase + MD_FDMA_DDR_SADDR);
	writel((u32)(NOR_BASE_PHYS + offset), host->mdbase + MD_FDMA_FL_SADDR);

	val = write ? (0x26000000 | len) : (0x2C000000 | len);
	writel(val, host->mdbase + MD_FDMA_CTRL2);

	mb();

	writel(0x03, host->mdbase + MD_FDMA_CTRL1);
	udelay(100);

	if (readl_poll_timeout(host->mdbase + MD_FDMA_CTRL1, val,
				!(val & 0x1), 100, RTKSFC_WAIT_TIMEOUT))
		return -ETIMEDOUT;

	return rtk_spi_nor_read_status(host);
}

static ssize_t rtk_spi_nor_read(struct spi_nor *nor, loff_t from, size_t len,
				 u8 *read_buf)
{
	struct rtksfc_priv *priv = nor->priv;
	struct rtksfc_host *host = priv->host;
	size_t done = 0;

	while (done < len) {
		size_t chunk = min_t(size_t, len - done, RTKSFC_DMA_MAX_LEN);
		int ret;

		rtk_spi_nor_read_mode(host);
		ret = rtk_spi_nor_dma_transfer(host, from + done, chunk, false);
		if (ret) {
			dev_err(nor->dev, "DMA read timeout\n");
			return ret;
		}
		memcpy(read_buf + done, host->buffer, chunk);
		done += chunk;
	}

	return len;
}

static ssize_t rtk_spi_nor_write(struct spi_nor *nor, loff_t to, size_t len,
				  const u8 *write_buf)
{
	struct rtksfc_priv *priv = nor->priv;
	struct rtksfc_host *host = priv->host;
	size_t done = 0;

	rtk_spi_nor_enable_auto_write(host);

	while (done < len) {
		size_t chunk = min_t(size_t, len - done, RTKSFC_DMA_MAX_LEN);
		int ret;

		memcpy(host->buffer, write_buf + done, chunk);
		rtk_spi_nor_write_mode(host);
		ret = rtk_spi_nor_dma_transfer(host, to + done, chunk, true);
		if (ret) {
			dev_err(nor->dev, "DMA write timeout\n");
			rtk_spi_nor_read_mode(host);
			return ret;
		}
		done += chunk;
	}

	rtk_spi_nor_read_mode(host);

	return len;
}

static int rtk_spi_nor_erase(struct spi_nor *nor, loff_t offs)
{
	struct rtksfc_priv *priv = nor->priv;
	struct rtksfc_host *host = priv->host;

	writel(nor->erase_opcode, host->regbase + SFC_OPCODE);
	udelay(50);
	writel(0x08, host->regbase + SFC_CTL);
	readb(host->iobase + offs);

	return rtk_spi_nor_read_status(host);
}

static const struct spi_nor_controller_ops rtk_sfc_controller_ops = {
	.prepare	= rtk_spi_nor_prep,
	.unprepare	= rtk_spi_nor_unprep,
	.read_reg	= rtk_spi_nor_read_reg,
	.write_reg	= rtk_spi_nor_write_reg,
	.read		= rtk_spi_nor_read,
	.write		= rtk_spi_nor_write,
	.erase		= rtk_spi_nor_erase,
};

static int rtk_spi_nor_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_nor_hwcaps hwcaps = { .mask = SNOR_HWCAPS_READ | SNOR_HWCAPS_PP };
	struct rtksfc_host *host;
	struct rtksfc_priv *priv;
	struct spi_nor *nor;
	struct mtd_info *mtd;
	struct resource *res;
	int ret;

	host = devm_kzalloc(dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	platform_set_drvdata(pdev, host);
	host->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	host->regbase = devm_ioremap_resource(dev, res);
	if (IS_ERR(host->regbase))
		return PTR_ERR(host->regbase);

	host->iobase = devm_ioremap(dev, NOR_BASE_PHYS, 0x2000000);
	if (!host->iobase)
		return -ENOMEM;

	host->mdbase = devm_ioremap(dev, MD_BASE_ADDR, 0x100);
	if (!host->mdbase)
		return -ENOMEM;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "unable to set dma mask\n");
		return ret;
	}

	host->buffer = dmam_alloc_coherent(dev, RTKSFC_DMA_MAX_LEN,
					    &host->dma_buffer, GFP_KERNEL);
	if (!host->buffer)
		return -ENOMEM;

	mutex_init(&host->lock);
	rtk_spi_nor_init(host);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_mutex;
	}
	priv->host = host;

	nor = &host->nor;
	nor->dev = dev;
	spi_nor_set_flash_node(nor, dev->of_node);
	nor->priv = priv;
	nor->controller_ops = &rtk_sfc_controller_ops;

	ret = spi_nor_scan(nor, NULL, &hwcaps);
	if (ret) {
		dev_err(dev, "failed to scan spi-nor flash: %d\n", ret);
		goto err_mutex;
	}

	mtd = &nor->mtd;
	mtd->name = dev_name(dev);
	ret = mtd_device_register(mtd, NULL, 0);
	if (ret) {
		dev_err(dev, "failed to register mtd device: %d\n", ret);
		goto err_mutex;
	}

	return 0;

err_mutex:
	mutex_destroy(&host->lock);
	return ret;
}

static void rtk_spi_nor_remove(struct platform_device *pdev)
{
	struct rtksfc_host *host = platform_get_drvdata(pdev);

	mtd_device_unregister(&host->nor.mtd);
	mutex_destroy(&host->lock);
}

static const struct of_device_id rtk_spi_nor_dt_ids[] = {
	{ .compatible = "realtek,rtk-sfc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rtk_spi_nor_dt_ids);

static struct platform_driver rtk_spi_nor_driver = {
	.driver = {
		.name		= "rtk-sfc",
		.of_match_table	= rtk_spi_nor_dt_ids,
	},
	.probe	= rtk_spi_nor_probe,
	.remove	= rtk_spi_nor_remove,
};
module_platform_driver(rtk_spi_nor_driver);

MODULE_DESCRIPTION("Realtek RTD129x SPI-NOR Flash Controller Driver");
MODULE_LICENSE("GPL");
