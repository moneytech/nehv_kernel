/*
 * Copyright 2011-2014 Freescale Semiconductor, Inc.
 * Copyright 2011 Linaro Ltd.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/genalloc.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/syscon/imx6q-iomuxc-gpr.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/suspend.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/syscon/imx6q-iomuxc-gpr.h>
#include <asm/cacheflush.h>
#include <asm/fncpy.h>
#include <asm/proc-fns.h>
#include <asm/suspend.h>
#include <asm/tlb.h>

#include "common.h"
#include "hardware.h"

#define CCR				0x0
#define BM_CCR_WB_COUNT			(0x7 << 16)
#define BM_CCR_RBC_BYPASS_COUNT		(0x3f << 21)
#define BM_CCR_RBC_EN			(0x1 << 27)

#define CLPCR				0x54
#define BP_CLPCR_LPM			0
#define BM_CLPCR_LPM			(0x3 << 0)
#define BM_CLPCR_BYPASS_PMIC_READY	(0x1 << 2)
#define BM_CLPCR_ARM_CLK_DIS_ON_LPM	(0x1 << 5)
#define BM_CLPCR_SBYOS			(0x1 << 6)
#define BM_CLPCR_DIS_REF_OSC		(0x1 << 7)
#define BM_CLPCR_VSTBY			(0x1 << 8)
#define BP_CLPCR_STBY_COUNT		9
#define BM_CLPCR_STBY_COUNT		(0x3 << 9)
#define BM_CLPCR_COSC_PWRDOWN		(0x1 << 11)
#define BM_CLPCR_WB_PER_AT_LPM		(0x1 << 16)
#define BM_CLPCR_WB_CORE_AT_LPM		(0x1 << 17)
#define BM_CLPCR_BYP_MMDC_CH0_LPM_HS	(0x1 << 19)
#define BM_CLPCR_BYP_MMDC_CH1_LPM_HS	(0x1 << 21)
#define BM_CLPCR_MASK_CORE0_WFI		(0x1 << 22)
#define BM_CLPCR_MASK_CORE1_WFI		(0x1 << 23)
#define BM_CLPCR_MASK_CORE2_WFI		(0x1 << 24)
#define BM_CLPCR_MASK_CORE3_WFI		(0x1 << 25)
#define BM_CLPCR_MASK_SCU_IDLE		(0x1 << 26)
#define BM_CLPCR_MASK_L2CC_IDLE		(0x1 << 27)

#define CGPR				0x64
#define BM_CGPR_INT_MEM_CLK_LPM		(0x1 << 17)

#define MX6Q_SUSPEND_OCRAM_SIZE		0x1000
#define MX6_MAX_MMDC_IO_NUM		33

static void __iomem *ccm_base;
static void __iomem *suspend_ocram_base;
static void (*imx6_suspend_in_ocram_fn)(void __iomem *ocram_vbase);

/*
 * suspend ocram space layout:
 * ======================== high address ======================
 *                              .
 *                              .
 *                              .
 *                              ^
 *                              ^
 *                              ^
 *                      imx6_suspend code
 *              PM_INFO structure(imx6_cpu_pm_info)
 * ======================== low address =======================
 */

struct imx6_pm_base {
	phys_addr_t pbase;
	void __iomem *vbase;
};

struct imx6_pm_socdata {
	u32 cpu_type;
	const char *mmdc_compat;
	const char *src_compat;
	const char *iomuxc_compat;
	const char *gpc_compat;
	const u32 mmdc_io_num;
	const u32 *mmdc_io_offset;
};

static const u32 imx6q_mmdc_io_offset[] __initconst = {
	0x5ac, 0x5b4, 0x528, 0x520, /* DQM0 ~ DQM3 */
	0x514, 0x510, 0x5bc, 0x5c4, /* DQM4 ~ DQM7 */
	0x56c, 0x578, 0x588, 0x594, /* CAS, RAS, SDCLK_0, SDCLK_1 */
	0x5a8, 0x5b0, 0x524, 0x51c, /* SDQS0 ~ SDQS3 */
	0x518, 0x50c, 0x5b8, 0x5c0, /* SDQS4 ~ SDQS7 */
	0x784, 0x788, 0x794, 0x79c, /* GPR_B0DS ~ GPR_B3DS */
	0x7a0, 0x7a4, 0x7a8, 0x748, /* GPR_B4DS ~ GPR_B7DS */
	0x59c, 0x5a0, 0x750, 0x774, /* SODT0, SODT1, MODE_CTL, MODE */
	0x74c,			    /* GPR_ADDS */
};

static const struct imx6_pm_socdata imx6q_pm_data __initconst = {
	.cpu_type = MXC_CPU_IMX6Q,
	.mmdc_compat = "fsl,imx6q-mmdc",
	.src_compat = "fsl,imx6q-src",
	.iomuxc_compat = "fsl,imx6q-iomuxc",
	.gpc_compat = "fsl,imx6q-gpc",
	.mmdc_io_num = ARRAY_SIZE(imx6q_mmdc_io_offset),
	.mmdc_io_offset = imx6q_mmdc_io_offset,
};

/*
 * This structure is for passing necessary data for low level ocram
 * suspend code(arch/arm/mach-imx/suspend-imx6.S), if this struct
 * definition is changed, the offset definition in
 * arch/arm/mach-imx/suspend-imx6.S must be also changed accordingly,
 * otherwise, the suspend to ocram function will be broken!
 */
struct imx6_cpu_pm_info {
	phys_addr_t pbase; /* The physical address of pm_info. */
	phys_addr_t resume_addr; /* The physical resume address for asm code */
	u32 cpu_type;
	u32 pm_info_size; /* Size of pm_info. */
	struct imx6_pm_base mmdc_base;
	struct imx6_pm_base src_base;
	struct imx6_pm_base iomuxc_base;
	struct imx6_pm_base ccm_base;
	struct imx6_pm_base gpc_base;
	struct imx6_pm_base l2_base;
	u32 mmdc_io_num; /* Number of MMDC IOs which need saved/restored. */
	u32 mmdc_io_val[MX6_MAX_MMDC_IO_NUM][2]; /* To save offset and value */
} __aligned(8);

void imx6q_set_cache_lpm_in_wait(bool enable)
{
	if ((cpu_is_imx6q() && imx_get_soc_revision() >
		IMX_CHIP_REVISION_1_1) ||
		(cpu_is_imx6dl() && imx_get_soc_revision() >
		IMX_CHIP_REVISION_1_0)) {
		u32 val;

		val = readl_relaxed(ccm_base + CGPR);
		if (enable)
			val |= BM_CGPR_INT_MEM_CLK_LPM;
		else
			val &= ~BM_CGPR_INT_MEM_CLK_LPM;
		writel_relaxed(val, ccm_base + CGPR);
	}
}

static void imx6q_enable_rbc(bool enable)
{
	u32 val;

	/*
	 * need to mask all interrupts in GPC before
	 * operating RBC configurations
	 */
	imx_gpc_mask_all();

	/* configure RBC enable bit */
	val = readl_relaxed(ccm_base + CCR);
	val &= ~BM_CCR_RBC_EN;
	val |= enable ? BM_CCR_RBC_EN : 0;
	writel_relaxed(val, ccm_base + CCR);

	/* configure RBC count */
	val = readl_relaxed(ccm_base + CCR);
	val &= ~BM_CCR_RBC_BYPASS_COUNT;
	val |= enable ? BM_CCR_RBC_BYPASS_COUNT : 0;
	writel(val, ccm_base + CCR);

	/*
	 * need to delay at least 2 cycles of CKIL(32K)
	 * due to hardware design requirement, which is
	 * ~61us, here we use 65us for safe
	 */
	udelay(65);

	/* restore GPC interrupt mask settings */
	imx_gpc_restore_all();
}

static void imx6q_enable_wb(bool enable)
{
	u32 val;

	/* configure well bias enable bit */
	val = readl_relaxed(ccm_base + CLPCR);
	val &= ~BM_CLPCR_WB_PER_AT_LPM;
	val |= enable ? BM_CLPCR_WB_PER_AT_LPM : 0;
	writel_relaxed(val, ccm_base + CLPCR);

	/* configure well bias count */
	val = readl_relaxed(ccm_base + CCR);
	val &= ~BM_CCR_WB_COUNT;
	val |= enable ? BM_CCR_WB_COUNT : 0;
	writel_relaxed(val, ccm_base + CCR);
}

int imx6q_set_lpm(enum mxc_cpu_pwr_mode mode)
{
	struct irq_desc *iomuxc_irq_desc;
	u32 val = readl_relaxed(ccm_base + CLPCR);

	val &= ~BM_CLPCR_LPM;
	switch (mode) {
	case WAIT_CLOCKED:
		break;
	case WAIT_UNCLOCKED:
		val |= 0x1 << BP_CLPCR_LPM;
		val |= BM_CLPCR_ARM_CLK_DIS_ON_LPM;
		val &= ~BM_CLPCR_VSTBY;
		val &= ~BM_CLPCR_SBYOS;
		if (cpu_is_imx6sl())
			val |= BM_CLPCR_BYP_MMDC_CH0_LPM_HS;
		else
			val |= BM_CLPCR_BYP_MMDC_CH1_LPM_HS;
		break;
	case STOP_POWER_ON:
		val |= 0x2 << BP_CLPCR_LPM;
		val &= ~BM_CLPCR_VSTBY;
		val &= ~BM_CLPCR_SBYOS;
		val |= BM_CLPCR_BYP_MMDC_CH1_LPM_HS;
		break;
	case WAIT_UNCLOCKED_POWER_OFF:
		val |= 0x1 << BP_CLPCR_LPM;
		val &= ~BM_CLPCR_VSTBY;
		val &= ~BM_CLPCR_SBYOS;
		break;
	case STOP_POWER_OFF:
		val |= 0x2 << BP_CLPCR_LPM;
		val |= 0x3 << BP_CLPCR_STBY_COUNT;
		val |= BM_CLPCR_VSTBY;
		val |= BM_CLPCR_SBYOS;
		if (cpu_is_imx6sl()) {
			val |= BM_CLPCR_BYPASS_PMIC_READY;
			val |= BM_CLPCR_BYP_MMDC_CH0_LPM_HS;
		} else {
			val |= BM_CLPCR_BYP_MMDC_CH1_LPM_HS;
		}
		break;
	default:
		return -EINVAL;
	}

	/*
	 * ERR007265: CCM: When improper low-power sequence is used,
	 * the SoC enters low power mode before the ARM core executes WFI.
	 *
	 * Software workaround:
	 * 1) Software should trigger IRQ #32 (IOMUX) to be always pending
	 *    by setting IOMUX_GPR1_GINT.
	 * 2) Software should then unmask IRQ #32 in GPC before setting CCM
	 *    Low-Power mode.
	 * 3) Software should mask IRQ #32 right after CCM Low-Power mode
	 *    is set (set bits 0-1 of CCM_CLPCR).
	 */
	iomuxc_irq_desc = irq_to_desc(32);
	imx_gpc_irq_unmask(&iomuxc_irq_desc->irq_data);
	writel_relaxed(val, ccm_base + CLPCR);
	imx_gpc_irq_mask(&iomuxc_irq_desc->irq_data);

	return 0;
}

static int imx6q_suspend_finish(unsigned long val)
{
	if (!imx6_suspend_in_ocram_fn) {
		cpu_do_idle();
	} else {
		/*
		 * call low level suspend function in ocram,
		 * as we need to float DDR IO.
		 */
		local_flush_tlb_all();
		imx6_suspend_in_ocram_fn(suspend_ocram_base);
	}

	return 0;
}

static int imx6q_pm_enter(suspend_state_t state)
{
	struct regmap *g;

	/*
	 * L2 can exit by 'reset' or Inband beacon (from remote EP)
	 * toggling phy_powerdown has same effect as 'inband beacon'
	 * So, toggle bit18 of GPR1, used as a workaround of errata
	 * "PCIe PCIe does not support L2 Power Down"
	 */
	if (IS_ENABLED(CONFIG_PCI_IMX6)) {
		g = syscon_regmap_lookup_by_compatible("fsl,imx6q-iomuxc-gpr");
		if (IS_ERR(g)) {
			pr_err("failed to find fsl,imx6q-iomux-gpr regmap\n");
			return PTR_ERR(g);
		}
		regmap_update_bits(g, IOMUXC_GPR1, IMX6Q_GPR1_PCIE_TEST_PD,
				IMX6Q_GPR1_PCIE_TEST_PD);
	}

	switch (state) {
	case PM_SUSPEND_STANDBY:
		imx6q_set_lpm(STOP_POWER_ON);
		imx6q_set_cache_lpm_in_wait(true);
		imx_gpc_pre_suspend(false);
		if (cpu_is_imx6sl())
			imx6sl_set_wait_clk(true);
		/* Zzz ... */
		cpu_do_idle();
		if (cpu_is_imx6sl())
			imx6sl_set_wait_clk(false);
		imx_gpc_post_resume();
		imx6q_set_lpm(WAIT_CLOCKED);
		break;
	case PM_SUSPEND_MEM:
		imx6q_set_cache_lpm_in_wait(false);
		imx6q_set_lpm(STOP_POWER_OFF);
		imx6q_enable_wb(true);
		/*
		 * For suspend into ocram, asm code already take care of
		 * RBC setting, so we do NOT need to do that here.
		 */
		if (!imx6_suspend_in_ocram_fn)
			imx6q_enable_rbc(true);
		imx_gpc_pre_suspend(true);
		imx_anatop_pre_suspend();
		imx_set_cpu_jump(0, v7_cpu_resume);
		/* Zzz ... */
		cpu_suspend(0, imx6q_suspend_finish);
		if (cpu_is_imx6q() || cpu_is_imx6dl())
			imx_smp_prepare();
		imx_anatop_post_resume();
		imx_gpc_post_resume();
		imx6q_enable_wb(false);
		imx6q_set_lpm(WAIT_CLOCKED);
		break;
	default:
		return -EINVAL;
	}

	/*
	 * L2 can exit by 'reset' or Inband beacon (from remote EP)
	 * toggling phy_powerdown has same effect as 'inband beacon'
	 * So, toggle bit18 of GPR1, used as a workaround of errata
	 * "PCIe PCIe does not support L2 Power Down"
	 */
	if (IS_ENABLED(CONFIG_PCI_IMX6)) {
		regmap_update_bits(g, IOMUXC_GPR1, IMX6Q_GPR1_PCIE_TEST_PD,
				!IMX6Q_GPR1_PCIE_TEST_PD);
	}

	return 0;
}

static int imx6q_pm_valid(suspend_state_t state)
{
	return (state == PM_SUSPEND_STANDBY || state == PM_SUSPEND_MEM);
}

static const struct platform_suspend_ops imx6q_pm_ops = {
	.enter = imx6q_pm_enter,
	.valid = imx6q_pm_valid,
};

void __init imx6q_pm_set_ccm_base(void __iomem *base)
{
	ccm_base = base;
}

static int __init imx6_pm_get_base(struct imx6_pm_base *base,
				const char *compat)
{
	struct device_node *node;
	struct resource res;
	int ret = 0;

	node = of_find_compatible_node(NULL, NULL, compat);
	if (!node) {
		ret = -ENODEV;
		goto out;
	}

	ret = of_address_to_resource(node, 0, &res);
	if (ret)
		goto put_node;

	base->pbase = res.start;
	base->vbase = ioremap(res.start, resource_size(&res));
	if (!base->vbase)
		ret = -ENOMEM;

put_node:
	of_node_put(node);
out:
	return ret;
}

static int __init imx6q_ocram_suspend_init(const struct imx6_pm_socdata
					*socdata)
{
	phys_addr_t ocram_pbase;
	struct device_node *node;
	struct platform_device *pdev;
	struct imx6_cpu_pm_info *pm_info;
	struct gen_pool *ocram_pool;
	unsigned long ocram_base;
	int i, ret = 0;
	const u32 *mmdc_offset_array;

	if (!socdata) {
		pr_warn("%s: invalid argument!\n", __func__);
		return -EINVAL;
	}

	node = of_find_compatible_node(NULL, NULL, "mmio-sram");
	if (!node) {
		pr_warn("%s: failed to find ocram node!\n", __func__);
		return -ENODEV;
	}

	pdev = of_find_device_by_node(node);
	if (!pdev) {
		pr_warn("%s: failed to find ocram device!\n", __func__);
		ret = -ENODEV;
		goto put_node;
	}

	ocram_pool = dev_get_gen_pool(&pdev->dev);
	if (!ocram_pool) {
		pr_warn("%s: ocram pool unavailable!\n", __func__);
		ret = -ENODEV;
		goto put_node;
	}

	ocram_base = gen_pool_alloc(ocram_pool, MX6Q_SUSPEND_OCRAM_SIZE);
	if (!ocram_base) {
		pr_warn("%s: unable to alloc ocram!\n", __func__);
		ret = -ENOMEM;
		goto put_node;
	}

	ocram_pbase = gen_pool_virt_to_phys(ocram_pool, ocram_base);

	suspend_ocram_base = __arm_ioremap_exec(ocram_pbase,
		MX6Q_SUSPEND_OCRAM_SIZE, false);

	pm_info = suspend_ocram_base;
	pm_info->pbase = ocram_pbase;
	pm_info->resume_addr = virt_to_phys(v7_cpu_resume);
	pm_info->pm_info_size = sizeof(*pm_info);

	/*
	 * ccm physical address is not used by asm code currently,
	 * so get ccm virtual address directly, as we already have
	 * it from ccm driver.
	 */
	pm_info->ccm_base.vbase = ccm_base;

	ret = imx6_pm_get_base(&pm_info->mmdc_base, socdata->mmdc_compat);
	if (ret) {
		pr_warn("%s: failed to get mmdc base %d!\n", __func__, ret);
		goto put_node;
	}

	ret = imx6_pm_get_base(&pm_info->src_base, socdata->src_compat);
	if (ret) {
		pr_warn("%s: failed to get src base %d!\n", __func__, ret);
		goto src_map_failed;
	}

	ret = imx6_pm_get_base(&pm_info->iomuxc_base, socdata->iomuxc_compat);
	if (ret) {
		pr_warn("%s: failed to get iomuxc base %d!\n", __func__, ret);
		goto iomuxc_map_failed;
	}

	ret = imx6_pm_get_base(&pm_info->gpc_base, socdata->gpc_compat);
	if (ret) {
		pr_warn("%s: failed to get gpc base %d!\n", __func__, ret);
		goto gpc_map_failed;
	}

	ret = imx6_pm_get_base(&pm_info->l2_base, "arm,pl310-cache");
	if (ret) {
		pr_warn("%s: failed to get pl310-cache base %d!\n",
			__func__, ret);
		goto pl310_cache_map_failed;
	}

	pm_info->cpu_type = socdata->cpu_type;
	pm_info->mmdc_io_num = socdata->mmdc_io_num;
	mmdc_offset_array = socdata->mmdc_io_offset;

	for (i = 0; i < pm_info->mmdc_io_num; i++) {
		pm_info->mmdc_io_val[i][0] =
			mmdc_offset_array[i];
		pm_info->mmdc_io_val[i][1] =
			readl_relaxed(pm_info->iomuxc_base.vbase +
			mmdc_offset_array[i]);
	}

	imx6_suspend_in_ocram_fn = fncpy(
		suspend_ocram_base + sizeof(*pm_info),
		&imx6_suspend,
		MX6Q_SUSPEND_OCRAM_SIZE - sizeof(*pm_info));

	goto put_node;

pl310_cache_map_failed:
	iounmap(&pm_info->gpc_base.vbase);
gpc_map_failed:
	iounmap(&pm_info->iomuxc_base.vbase);
iomuxc_map_failed:
	iounmap(&pm_info->src_base.vbase);
src_map_failed:
	iounmap(&pm_info->mmdc_base.vbase);
put_node:
	of_node_put(node);

	return ret;
}

static void __init imx6_pm_common_init(const struct imx6_pm_socdata
					*socdata)
{
	struct regmap *gpr;
	int ret;

	WARN_ON(!ccm_base);

	ret = imx6q_ocram_suspend_init(socdata);
	if (ret)
		pr_warn("%s: failed to initialize ocram suspend %d!\n",
			__func__, ret);

	/*
	 * This is for SW workaround step #1 of ERR007265, see comments
	 * in imx6q_set_lpm for details of this errata.
	 * Force IOMUXC irq pending, so that the interrupt to GPC can be
	 * used to deassert dsm_request signal when the signal gets
	 * asserted unexpectedly.
	 */
	gpr = syscon_regmap_lookup_by_compatible("fsl,imx6q-iomuxc-gpr");
	if (!IS_ERR(gpr))
		regmap_update_bits(gpr, IOMUXC_GPR1, IMX6Q_GPR1_GINT_MASK,
				   IMX6Q_GPR1_GINT_MASK);


	suspend_set_ops(&imx6q_pm_ops);
}

void __init imx6q_pm_init(void)
{
	imx6_pm_common_init(&imx6q_pm_data);
}

void __init imx6dl_pm_init(void)
{
	imx6_pm_common_init(NULL);
}

void __init imx6sl_pm_init(void)
{
	imx6_pm_common_init(NULL);
}