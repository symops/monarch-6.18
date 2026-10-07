// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTD129x SCPU (CPU) PLL clock
 *
 * Forward-ported from the vendor 4.9.330 tree's drivers/clk/realtek/
 * clk-pll.c + the "pll_scpu" clk_pll_div instance in clk-rtd1295-cc.c --
 * as a single-purpose driver rather than the vendor's generic multi-type
 * clk_pll_div/mux/gate framework for the SoC's whole CRT clock tree,
 * since this is the only clock currently needed (cpufreq).
 *
 * Discrete frequency/divider tables, not a formula: this PLL's N/F
 * fractional-synthesizer register doesn't compute cleanly from a target
 * Hz value, so both the PLL step ("SSC1" register layout) and the
 * post-divider step are exact copies of the vendor's lookup tables.
 * Rates that don't land on an exact table entry round down to the
 * nearest one, same as the vendor driver.
 *
 * No .enable/.disable/.is_enabled: the vendor's clk_pll_enable() et al.
 * are gated on a "pow_loc" field this clock's vendor struct never sets,
 * making clk_pll_is_enabled() unconditionally return -EINVAL for
 * pll_scpu specifically and the enable/disable hooks no-ops. This is
 * also the CPU's own clock (CLK_IGNORE_UNUSED in the vendor tree), so
 * treating it as always-on here is correct, not just a shortcut.
 *
 * Registers are offsets into the shared CRT syscon (&crt in the board
 * DTS) -- accessed through the parent syscon's regmap, not a private
 * MMIO window, since the divider register (0x030) and the PLL registers
 * (0x500+) sit far apart in the same block.
 */

#include <linux/bitops.h>
#include <linux/clk-provider.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define SCPU_DIV_REG		0x030
#define SCPU_DIV_SHIFT		7
#define SCPU_DIV_WIDTH		2
#define SCPU_DIV_MASK		GENMASK(SCPU_DIV_SHIFT + SCPU_DIV_WIDTH - 1, SCPU_DIV_SHIFT)

#define SCPU_SSC_CTRL_REG	0x500
#define SCPU_SSC_CTRL_MASK	0x7
#define SCPU_SSC_CTRL_PREPARE	0x4
#define SCPU_SSC_CTRL_OC_EN	0x5
#define SCPU_SSC_FREQ_REG	0x504
#define SCPU_SSC_FREQ_MASK	0x7ffff
#define SCPU_SSC_LOCK_REG	0x51c
#define SCPU_SSC_LOCK_BIT	BIT(20)
#define SCPU_SSC_LOCK_TIMEOUT_US 2000

#define SCPU_FREQ_NF(_n, _f)	(((_n) << 11) | (_f))

struct rtd129x_scpu_div {
	unsigned long rate;	/* vendor DIV_DV() floor: pick this entry
				 * for any target rate >= this threshold */
	unsigned int div;
	u32 val;
};

/* Exact copy of the vendor's scpu_div_tbl[] (clk-rtd1295-cc.c). */
static const struct rtd129x_scpu_div rtd129x_scpu_div_tbl[] = {
	{ 1000000000, 1, 0 },
	{  500000000, 2, 2 },
	{  250000000, 4, 3 },
};

struct rtd129x_scpu_freq {
	unsigned long rate;
	u32 val;
};

/*
 * Exact copy of the vendor's scpu_tbl[] (clk-rtd1295-cc.c), plus one
 * locally-added entry (marked below) disambiguating a tie in it.
 */
static const struct rtd129x_scpu_freq rtd129x_scpu_freq_tbl[] = {
	{ 1000000000, SCPU_FREQ_NF(34,   75) },
	{ 1100000000, SCPU_FREQ_NF(37, 1517) },
	{ 1200000000, SCPU_FREQ_NF(41,  910) },
	{ 1300000000, SCPU_FREQ_NF(45,  303) },
	{ 1400000000, SCPU_FREQ_NF(48, 1745) },
	{ 1500000000, SCPU_FREQ_NF(52, 1137) },
	{ 1600000000, SCPU_FREQ_NF(56,  531) },
	{ 1800000000, SCPU_FREQ_NF(63, 1365) },
	{ 1200000000, SCPU_FREQ_NF(41, 1024) },
	{ 1300000000, SCPU_FREQ_NF(45, 1024) },
	{ 1503000000, SCPU_FREQ_NF(48, 1744) },
	/*
	 * NF(41, 1024) above is the boot loader's own PLL setting, labeled
	 * 1200000000 in the vendor table like its neighbor NF(41, 910), but
	 * actually ~1201.5 MHz. find_freq()'s closest-below-with-ties-
	 * broken-by-table-order search let that label collision silently
	 * prefer NF(41, 910) whenever a caller asked for the boot value's
	 * true rate (e.g. the 300.375/600.75 MHz OPPs, each needing exactly
	 * 1201500000 one or two post-dividers up) -- reprogramming the PLL
	 * away from the boot setting every time, off by ~1.5 MHz, instead of
	 * leaving it alone. Giving the true rate its own exact entry here
	 * makes find_freq() match it directly, before the tie ever matters.
	 */
	{ 1201500000, SCPU_FREQ_NF(41, 1024) },
};

struct rtd129x_scpu_clk {
	struct clk_hw hw;
	struct regmap *regmap;
};

static inline struct rtd129x_scpu_clk *to_rtd129x_scpu_clk(struct clk_hw *hw)
{
	return container_of(hw, struct rtd129x_scpu_clk, hw);
}

/* Vendor dtbl_find_by_rate(): first entry whose floor the rate clears. */
static const struct rtd129x_scpu_div *rtd129x_scpu_find_div(unsigned long rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rtd129x_scpu_div_tbl); i++)
		if (rate >= rtd129x_scpu_div_tbl[i].rate)
			return &rtd129x_scpu_div_tbl[i];
	return NULL;
}

static const struct rtd129x_scpu_div *rtd129x_scpu_find_div_by_val(u32 val)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rtd129x_scpu_div_tbl); i++)
		if (rtd129x_scpu_div_tbl[i].val == val)
			return &rtd129x_scpu_div_tbl[i];
	return NULL;
}

/* Vendor ftbl_find_by_rate(): exact match, else closest entry below. */
static const struct rtd129x_scpu_freq *rtd129x_scpu_find_freq(unsigned long rate)
{
	const struct rtd129x_scpu_freq *best = NULL;
	unsigned long best_rate = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(rtd129x_scpu_freq_tbl); i++) {
		const struct rtd129x_scpu_freq *f = &rtd129x_scpu_freq_tbl[i];

		if (f->rate == rate)
			return f;
		if (f->rate > rate)
			continue;
		if ((rate - best_rate) > (rate - f->rate)) {
			best_rate = f->rate;
			best = f;
		}
	}
	return best;
}

static const struct rtd129x_scpu_freq *rtd129x_scpu_find_freq_by_val(u32 val)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rtd129x_scpu_freq_tbl); i++)
		if (rtd129x_scpu_freq_tbl[i].val == val)
			return &rtd129x_scpu_freq_tbl[i];
	return NULL;
}

static long rtd129x_scpu_clk_round_rate(struct clk_hw *hw, unsigned long rate,
					 unsigned long *parent_rate)
{
	const struct rtd129x_scpu_div *dv;
	const struct rtd129x_scpu_freq *fv;

	dv = rtd129x_scpu_find_div(rate);
	if (!dv)
		return -EINVAL;

	fv = rtd129x_scpu_find_freq(rate * dv->div);
	if (!fv)
		return -EINVAL;

	return fv->rate / dv->div;
}

static unsigned long rtd129x_scpu_clk_recalc_rate(struct clk_hw *hw,
						    unsigned long parent_rate)
{
	struct rtd129x_scpu_clk *clk = to_rtd129x_scpu_clk(hw);
	const struct rtd129x_scpu_div *dv;
	const struct rtd129x_scpu_freq *fv;
	unsigned int val;

	regmap_read(clk->regmap, SCPU_DIV_REG, &val);
	dv = rtd129x_scpu_find_div_by_val((val & SCPU_DIV_MASK) >> SCPU_DIV_SHIFT);
	if (!dv)
		return 0;

	regmap_read(clk->regmap, SCPU_SSC_FREQ_REG, &val);
	fv = rtd129x_scpu_find_freq_by_val(val & SCPU_SSC_FREQ_MASK);
	if (!fv)
		return 0;

	return fv->rate / dv->div;
}

static int rtd129x_scpu_clk_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long parent_rate)
{
	struct rtd129x_scpu_clk *clk = to_rtd129x_scpu_clk(hw);
	const struct rtd129x_scpu_div *ndv, *cdv;
	const struct rtd129x_scpu_freq *fv;
	unsigned int val;
	int ret;

	ndv = rtd129x_scpu_find_div(rate);
	if (!ndv)
		return -EINVAL;

	fv = rtd129x_scpu_find_freq(rate * ndv->div);
	if (!fv)
		return -EINVAL;

	regmap_read(clk->regmap, SCPU_DIV_REG, &val);
	cdv = rtd129x_scpu_find_div_by_val((val & SCPU_DIV_MASK) >> SCPU_DIV_SHIFT);
	if (!cdv)
		cdv = ndv;

	/*
	 * Anti-glitch ordering from the vendor driver: when raising the
	 * divider (slowing down), apply it before the PLL step; when
	 * lowering it (speeding up), apply it after -- so the transient
	 * frequency during the switch never exceeds the higher of the old
	 * and new target rates.
	 */
	if (ndv->div > cdv->div)
		regmap_update_bits(clk->regmap, SCPU_DIV_REG, SCPU_DIV_MASK,
				    ndv->val << SCPU_DIV_SHIFT);

	regmap_update_bits(clk->regmap, SCPU_SSC_CTRL_REG, SCPU_SSC_CTRL_MASK,
			    SCPU_SSC_CTRL_PREPARE);
	regmap_update_bits(clk->regmap, SCPU_SSC_FREQ_REG, SCPU_SSC_FREQ_MASK, fv->val);
	regmap_update_bits(clk->regmap, SCPU_SSC_CTRL_REG, SCPU_SSC_CTRL_MASK,
			    SCPU_SSC_CTRL_OC_EN);
	ret = regmap_read_poll_timeout(clk->regmap, SCPU_SSC_LOCK_REG, val,
					val & SCPU_SSC_LOCK_BIT, 0,
					SCPU_SSC_LOCK_TIMEOUT_US);
	if (ret)
		return ret;

	if (ndv->div < cdv->div)
		regmap_update_bits(clk->regmap, SCPU_DIV_REG, SCPU_DIV_MASK,
				    ndv->val << SCPU_DIV_SHIFT);

	return 0;
}

static const struct clk_ops rtd129x_scpu_clk_ops = {
	.round_rate  = rtd129x_scpu_clk_round_rate,
	.recalc_rate = rtd129x_scpu_clk_recalc_rate,
	.set_rate    = rtd129x_scpu_clk_set_rate,
};

static int rtd129x_scpu_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtd129x_scpu_clk *clk;
	struct clk_init_data init = { };
	const char *parent_name;
	int ret;

	clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return -ENOMEM;

	clk->regmap = syscon_node_to_regmap(dev->of_node->parent);
	if (IS_ERR(clk->regmap))
		return dev_err_probe(dev, PTR_ERR(clk->regmap),
				      "failed to get parent syscon regmap\n");

	parent_name = of_clk_get_parent_name(dev->of_node, 0);
	if (!parent_name)
		return -EINVAL;

	init.name = dev->of_node->name;
	init.ops = &rtd129x_scpu_clk_ops;
	init.parent_names = &parent_name;
	init.num_parents = 1;
	init.flags = CLK_IGNORE_UNUSED | CLK_GET_RATE_NOCACHE;
	clk->hw.init = &init;

	ret = devm_clk_hw_register(dev, &clk->hw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register clock\n");

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &clk->hw);
}

static const struct of_device_id rtd129x_scpu_clk_of_match[] = {
	{ .compatible = "realtek,rtd129x-scpu-clk" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rtd129x_scpu_clk_of_match);

static struct platform_driver rtd129x_scpu_clk_driver = {
	.driver = {
		.name = "rtd129x-scpu-clk",
		.of_match_table = rtd129x_scpu_clk_of_match,
	},
	.probe = rtd129x_scpu_clk_probe,
};
module_platform_driver(rtd129x_scpu_clk_driver);

MODULE_DESCRIPTION("Realtek RTD129x SCPU (CPU) PLL clock driver");
MODULE_LICENSE("GPL");
