/*
 * Copyright (c) 2013 Google, Inc
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <dt-structs.h>
#include <dwmmc.h>
#include <errno.h>
#include <mapmem.h>
#include <pwrseq.h>
#include <syscon.h>
#include <asm/gpio.h>
#include <asm/arch/clock.h>
#include <asm/arch/periph.h>
#include <linux/err.h>

DECLARE_GLOBAL_DATA_PTR;

struct rockchip_mmc_plat {
#if CONFIG_IS_ENABLED(OF_PLATDATA)
	struct dtd_rockchip_rk3288_dw_mshc dtplat;
#endif
	struct mmc_config cfg;
	struct mmc mmc;
};

struct rockchip_dwmmc_priv {
	struct clk clk;
	struct dwmci_host host;
	int fifo_depth;
	bool fifo_mode;
	u32 minmax[2];
};

static uint rockchip_dwmmc_get_mmc_clk(struct dwmci_host *host, uint freq)
{
	struct udevice *dev = host->priv;
	struct rockchip_dwmmc_priv *priv = dev_get_priv(dev);
	int ret;

	ret = clk_set_rate(&priv->clk, freq);
	if (ret < 0) {
		debug("%s: err=%d\n", __func__, ret);
		return ret;
	}

	return freq;
}

static int rockchip_dwmmc_ofdata_to_platdata(struct udevice *dev)
{
#if !CONFIG_IS_ENABLED(OF_PLATDATA)
	struct rockchip_dwmmc_priv *priv = dev_get_priv(dev);
	struct dwmci_host *host = &priv->host;

	host->name = dev->name;
	host->ioaddr = (void *)dev_get_addr(dev);
	host->buswidth = fdtdec_get_int(gd->fdt_blob, dev->of_offset,
					"bus-width", 4);
	host->get_mmc_clk = rockchip_dwmmc_get_mmc_clk;
	host->priv = dev;

	/* use non-removeable as sdcard and emmc as judgement */
	if (fdtdec_get_bool(gd->fdt_blob, dev->of_offset, "non-removable"))
		host->dev_index = 0;
	else
		host->dev_index = 1;

	priv->fifo_depth = fdtdec_get_int(gd->fdt_blob, dev->of_offset,
				    "fifo-depth", 0);
	if (priv->fifo_depth < 0)
		return -EINVAL;
	priv->fifo_mode = fdtdec_get_bool(gd->fdt_blob, dev->of_offset,
					  "fifo-mode");
	if (fdtdec_get_int_array(gd->fdt_blob, dev->of_offset,
				 "clock-freq-min-max", priv->minmax, 2))
		return -EINVAL;
#endif
	return 0;
}

static int rockchip_dwmmc_probe(struct udevice *dev)
{
	struct rockchip_mmc_plat *plat = dev_get_platdata(dev);
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct rockchip_dwmmc_priv *priv = dev_get_priv(dev);
	struct dwmci_host *host = &priv->host;
	struct udevice *pwr_dev __maybe_unused;
	int ret;

#if CONFIG_IS_ENABLED(OF_PLATDATA)
	struct dtd_rockchip_rk3288_dw_mshc *dtplat = &plat->dtplat;

	host->name = dev->name;
	host->ioaddr = map_sysmem(dtplat->reg[0], dtplat->reg[1]);
	host->buswidth = dtplat->bus_width;
	host->get_mmc_clk = rockchip_dwmmc_get_mmc_clk;
	host->priv = dev;
	host->dev_index = 0;
	priv->fifo_depth = dtplat->fifo_depth;
	priv->fifo_mode = 0;
	memcpy(priv->minmax, dtplat->clock_freq_min_max, sizeof(priv->minmax));

	ret = clk_get_by_index_platdata(dev, 0, dtplat->clocks, &priv->clk);
	if (ret < 0)
		return ret;
#else
	ret = clk_get_by_index(dev, 0, &priv->clk);
	if (ret < 0)
		return ret;
#endif
	host->fifoth_val = MSIZE(0x2) |
		RX_WMARK(priv->fifo_depth / 2 - 1) |
		TX_WMARK(priv->fifo_depth / 2);

	host->fifo_mode = priv->fifo_mode;

#ifdef CONFIG_PWRSEQ
	/* Enable power if needed */
	ret = uclass_get_device_by_phandle(UCLASS_PWRSEQ, dev, "mmc-pwrseq",
					   &pwr_dev);
	if (!ret) {
		ret = pwrseq_set_power(pwr_dev, true);
		if (ret)
			return ret;
	}
#endif
	dwmci_setup_cfg(&plat->cfg, dev->name, host->buswidth, host->caps,
			priv->minmax[1], priv->minmax[0]);
	host->mmc = &plat->mmc;
	host->mmc->priv = &priv->host;
	host->mmc->dev = dev;
	upriv->mmc = host->mmc;

	return dwmci_probe(dev);
}

static int rockchip_dwmmc_bind(struct udevice *dev)
{
	struct rockchip_mmc_plat *plat = dev_get_platdata(dev);

	return dwmci_bind(dev, &plat->mmc, &plat->cfg);
}

static const struct udevice_id rockchip_dwmmc_ids[] = {
	{ .compatible = "rockchip,rk3288-dw-mshc" },
	{ }
};

U_BOOT_DRIVER(rockchip_dwmmc_drv) = {
	.name		= "rockchip_rk3288_dw_mshc",
	.id		= UCLASS_MMC,
	.of_match	= rockchip_dwmmc_ids,
	.ofdata_to_platdata = rockchip_dwmmc_ofdata_to_platdata,
	.ops		= &dm_dwmci_ops,
	.bind		= rockchip_dwmmc_bind,
	.probe		= rockchip_dwmmc_probe,
	.priv_auto_alloc_size = sizeof(struct rockchip_dwmmc_priv),
	.platdata_auto_alloc_size = sizeof(struct rockchip_mmc_plat),
};

#ifdef CONFIG_PWRSEQ
static int rockchip_dwmmc_pwrseq_set_power(struct udevice *dev, bool enable)
{
	struct gpio_desc reset;
	int ret;

	ret = gpio_request_by_name(dev, "reset-gpios", 0, &reset, GPIOD_IS_OUT);
	if (ret)
		return ret;
	dm_gpio_set_value(&reset, 1);
	udelay(1);
	dm_gpio_set_value(&reset, 0);
	udelay(200);

	return 0;
}

static const struct pwrseq_ops rockchip_dwmmc_pwrseq_ops = {
	.set_power	= rockchip_dwmmc_pwrseq_set_power,
};

static const struct udevice_id rockchip_dwmmc_pwrseq_ids[] = {
	{ .compatible = "mmc-pwrseq-emmc" },
	{ }
};

U_BOOT_DRIVER(rockchip_dwmmc_pwrseq_drv) = {
	.name		= "mmc_pwrseq_emmc",
	.id		= UCLASS_PWRSEQ,
	.of_match	= rockchip_dwmmc_pwrseq_ids,
	.ops		= &rockchip_dwmmc_pwrseq_ops,
};
#endif
