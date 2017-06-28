/*
 *    Based on code from Cortina Systems, Inc.
 *
 *    Copyright (C) 2011, 2012 by Cortina Systems, Inc.
 *    Copyright (C) 2011, 2012 Cavium, Inc.
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 */
#if 1

# include <common.h>
# include <command.h>
# include <asm/arch/cvmx.h>
# include <asm/arch/cvmx-mdio.h>
# include <errno.h>
# include <malloc.h>
# include <phy.h>
# include <cortina_cs4321.h>
# include <miiphy.h>
#else
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/phy.h>
#include <linux/of.h>
#endif

#include "cs4321-regs.h"

struct cs4321_private {
	enum cortina_cs4321_host_mode mode;
	int fw_loaded;
	int initialized;
};

#ifdef __U_BOOT__
#define MII_ADDR_C45					0

#define CS4321_API_VERSION_MAJOR	3
#define CS4321_API_VERSION_MINOR	0
#define CS4321_API_VERSION_UPDATE	113

int debug_phyio = 0;

enum phy_state {
	PHY_DOWN = 0,
	PHY_STARTING,
	PHY_READY,
	PHY_PENDING,
	PHY_UP,
	PHY_AN,
	PHY_RUNNING,
	PHY_NOLINK,
	PHY_FORCING,
	PHY_CHANGELINK,
	PHY_HALTED,
	PHY_RESUMING
};

#endif

struct cs4321_reg_modify {
	uint16_t reg;
	uint16_t mask_bits;
	uint16_t set_bits;
};

struct cs4321_multi_seq {
	int reg_offset;
	const struct cs4321_reg_modify *seq;
};

#include "cs4321-fw.inc"

static const struct cs4321_reg_modify cs4321_soft_reset_registers[] = {
	/* Enable all the clocks */
	{CS4321_GLOBAL_INGRESS_CLKEN, 0, 0xffff},
	{CS4321_GLOBAL_INGRESS_CLKEN2, 0, 0xffff},
	{CS4321_GLOBAL_EGRESS_CLKEN, 0, 0xffff},
	{CS4321_GLOBAL_EGRESS_CLKEN2, 0, 0xffff},
	/* Reset MPIF registers */
	{CS4321_GLOBAL_MPIF_RESET_DOTREG, 0, 0x0},
	/* Re-assert the reset */
	{CS4321_GLOBAL_MPIF_RESET_DOTREG, 0, 0xffff},

	/* Disable all the clocks */
	{CS4321_GLOBAL_INGRESS_CLKEN, 0, 0x0},
	{CS4321_GLOBAL_INGRESS_CLKEN2, 0, 0x0},
	{CS4321_GLOBAL_EGRESS_CLKEN, 0, 0x0},
	{CS4321_GLOBAL_EGRESS_CLKEN2, 0, 0x0},
	{0}
};

static const struct cs4321_reg_modify cs4321_68xx_4_nic_init[] = {
        /* Configure chip for common reference clock */
	{CS4321_LINE_SDS_COMMON_STXP0_TX_CONFIG, 0, 0x2700},
	/* Set GPIO3 to drive low to enable laser output */
	{CS4321_GPIO_GPIO3, 0, 0x11},
	{0}
};

#define CS4321_API_VERSION_VALUE			\
	((CS4321_API_VERSION_MAJOR & 0xF) << 12) |	\
	((CS4321_API_VERSION_MINOR & 0xF) << 8)  |	\
	(CS4321_API_VERSION_UPDATE)

static const struct cs4321_reg_modify cs4321_init_prefix_seq[] = {
	/* MPIF DeAssert System Reset */
	{CS4321_GLOBAL_MPIF_RESET_DOTREG, ~0x0001, 0},
	{CS4321_GLOBAL_SCRATCH7, 0, CS4321_API_VERSION_VALUE},
	/*
	 * Make sure to stall the microsequencer before configuring
	 * the path.
	 */
	{CS4321_GLOBAL_MSEQCLKCTRL, 0, 0x8004},
	{CS4321_MSEQ_OPTIONS, 0, 0xf},
	{CS4321_MSEQ_PC, 0, 0x0},
	/*
	 * Correct some of the h/w defaults that are incorrect.
	 *
	 * The default value of the bias current is incorrect and needs to
	 * be corrected. This is normally done by Microcode but it doesn't
	 * always run.
	 */
	{CS4321_DSP_SDS_SERDES_SRX_DAC_BIAS_SELECT0_MSB, 0, 0x20},
	/*
	 * By default need to power on the voltage monitor since it is required
	 * by the temperature monitor and this is used by the microcode.
	 */
	{CS4321_LINE_SDS_COMMON_SRX0_RX_CONFIG, 0, 0x0},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_ingress_local_timing_rxaui[] = {
	{CS4321_HOST_SDS_COMMON_STX0_TX_CONFIG_LOCAL_TIMING, 0, 0x0001},
	{CS4321_HOST_SDS_COMMON_STXP0_TX_CLKDIV_CTRL, 0, 0x4091},
	{CS4321_HOST_SDS_COMMON_STXP0_TX_CLKOUT_CTRL, 0, 0x1864},
	{CS4321_HOST_SDS_COMMON_STXP0_TX_CONFIG, 0, 0x090c},
	{CS4321_HOST_SDS_COMMON_STXP0_TX_PWRDN, 0, 0x0000},
	{CS4321_HOST_ML_SDS_COMMON_STXP0_TX_CLKDIV_CTRL, 0, 0x4019},
	{CS4321_HOST_ML_SDS_COMMON_STXP0_TX_CONFIG, 0, 0x090c},
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, ~0x2, 0x0002},
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, ~0x2, 0x0000},

	{CS4321_LINE_SDS_COMMON_SRX0_RX_CONFIG, 0, 0x0000},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_CPA, 0, 0x0057},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_LOOP_FILTER, 0, 0x0007},

	{CS4321_GLOBAL_INGRESS_SOFT_RESET, ~0x1, 0x0001},
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, ~0x1, 0x0000},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_egress_local_host_timing_mux_demux[] = {
	/* DMUXPD on, MUXPD on, EYEMODE off */
	{CS4321_HOST_SDS_COMMON_SRX0_RX_CONFIG, ~0x1300, 0},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_egress_local_timing_rxaui[] = {
	{CS4321_HOST_SDS_COMMON_SRX0_RX_CLKDIV_CTRL, 0, 0x40d1},
	{CS4321_HOST_SDS_COMMON_SRX0_RX_CONFIG, 0, 0x000c},
	{CS4321_HOST_ML_SDS_COMMON_SRX0_RX_CLKDIV_CTRL, 0, 0x401d},
	{CS4321_HOST_ML_SDS_COMMON_SRX0_RX_CONFIG, 0, 0x000c},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, ~0x1, 0x0001},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, ~0x1, 0x0000},
	{CS4321_LINE_SDS_COMMON_STX0_TX_CONFIG_LOCAL_TIMING, 0, 0x0001},
	{CS4321_LINE_SDS_COMMON_STXP0_TX_CLKOUT_CTRL, 0, 0x0864},
	{CS4321_LINE_SDS_COMMON_STXP0_TX_LOOP_FILTER, 0, 0x0027},
	{CS4321_LINE_SDS_COMMON_STXP0_TX_PWRDN, 0, 0x0000},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, ~0x2, 0x0002},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, ~0x2, 0x0000},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_line_power_down[] = {
	{CS4321_LINE_SDS_COMMON_STX0_TX_OUTPUT_CTRLA, 0, 0x0000},
	{CS4321_LINE_SDS_COMMON_STX0_TX_OUTPUT_CTRLB, 0, 0x0000},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_CONFIG, 0, 0x2040},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_VCO_CTRL, 0, 0x01e7},
	{CS4321_MSEQ_POWER_DOWN_LSB, 0, 0x0000},
	{CS4321_DSP_SDS_SERDES_SRX_DAC_ENABLEB_MSB, 0, 0xffff},
	{CS4321_DSP_SDS_SERDES_SRX_DAC_ENABLEB_LSB, 0, 0xffff},
	{CS4321_DSP_SDS_SERDES_SRX_AGC_MISC, 0, 0x0705},
	{CS4321_DSP_SDS_SERDES_SRX_DFE_MISC, 0, 0x002b},
	{CS4321_DSP_SDS_SERDES_SRX_FFE_PGA_CTRL, 0, 0x0021},
	{CS4321_DSP_SDS_SERDES_SRX_FFE_MISC, 0, 0x0013},
	{CS4321_DSP_SDS_SERDES_SRX_FFE_INBUF_CTRL, 0, 0x0001},
	{CS4321_DSP_SDS_SERDES_SRX_DFE0_SELECT, 0, 0x0001},
	{0}
};


static const struct cs4321_reg_modify cs4321_init_dpath_ingress_rxaui_pcs_ra[] = {
	/* Set fen_radj, rx_fen_xgpcs */
	{CS4321_GLOBAL_INGRESS_FUNCEN, ~0x0081, 0x0081},
	/* Set rx_en_radj, rx_en_xgpcs */
	{CS4321_GLOBAL_INGRESS_CLKEN, ~0x0021, 0x0021},
	/* Set tx_en_hif, tx_en_radj */
	{CS4321_GLOBAL_INGRESS_CLKEN2, ~0x0120, 0x0120},

	{CS4321_GLOBAL_HOST_MULTILANE_CLKSEL, 0, 0x8000},
	{CS4321_GLOBAL_HOST_MULTILANE_FUNCEN, 0, 0x0006},

	{CS4321_GLOBAL_REF_SOFT_RESET, 0, 0xffff},
	{CS4321_GLOBAL_REF_SOFT_RESET, 0, 0x0000},
	/* MPIF DeAssert Ingress Reset */
	{CS4321_GLOBAL_MPIF_RESET_DOTREG, ~0x0004, 0},

	{CS4321_XGMAC_LINE_RX_CFG_COM, 0, 0x8010},
	{CS4321_XGPCS_LINE_RX_RXCNTRL, 0, 0x5000},

	{CS4321_RADJ_INGRESS_RX_NRA_MIN_IFG, 0, 0x0004},
	{CS4321_RADJ_INGRESS_RX_NRA_SETTLE, 0, 0x0000},
	{CS4321_RADJ_INGRESS_TX_ADD_FILL_CTRL, 0, 0xf001},
	{CS4321_RADJ_INGRESS_TX_ADD_FILL_DATA0, 0, 0x0707},
	{CS4321_RADJ_INGRESS_TX_ADD_FILL_DATA1, 0, 0x0707},
	{CS4321_RADJ_INGRESS_TX_PRA_MIN_IFG, 0, 0x0004},
	{CS4321_RADJ_INGRESS_TX_PRA_SETTLE, 0, 0x0000},
	{CS4321_RADJ_INGRESS_MISC_RESET, 0, 0x0000},

	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0x0002},
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0x0000},
	{CS4321_RADJ_INGRESS_MISC_RESET, 0, 0x0000},

	{CS4321_PM_CTRL, 0, 0x0000},
	{CS4321_HIF_COMMON_TXCONTROL3, 0, 0x0010},

	{CS4321_MSEQ_POWER_DOWN_LSB, 0, 0xe01f},

	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_egress_rxaui_pcs_ra[] = {
	/* Set tx_fen_xgpcs, fen_radj */
	{CS4321_GLOBAL_EGRESS_FUNCEN, ~0x0180, 0x0180},
	/* Set rx_en_hif, rx_en_radj */
	{CS4321_GLOBAL_EGRESS_CLKEN, ~0x0120, 0x0120},
	/* Set tx_en_radj, tx_en_xgpcs */
	{CS4321_GLOBAL_EGRESS_CLKEN2, ~0x0021, 0x0021},

	{CS4321_GLOBAL_HOST_MULTILANE_CLKSEL, 0, 0x8000},
	{CS4321_GLOBAL_HOST_MULTILANE_FUNCEN, 0, 0x0006},

	{CS4321_GLOBAL_REF_SOFT_RESET, 0, 0xffff},
	{CS4321_GLOBAL_REF_SOFT_RESET, 0, 0x0000},
	/* MPIF DeAssert Egress Reset */
	{CS4321_GLOBAL_MPIF_RESET_DOTREG, ~0x0002, 0},

	{CS4321_XGMAC_LINE_TX_CFG_COM, 0, 0xc000},
	{CS4321_XGMAC_LINE_TX_CFG_TX_IFG, 0, 0x0005},
	{CS4321_XGPCS_LINE_TX_TXCNTRL, 0, 0x0000},
	{CS4321_XGRS_LINE_TX_TXCNTRL, 0, 0xc000},

	{CS4321_RADJ_EGRESS_RX_NRA_MIN_IFG, 0, 0x0004},
	{CS4321_RADJ_EGRESS_RX_NRA_SETTLE, 0, 0x0000},
	{CS4321_RADJ_EGRESS_TX_ADD_FILL_CTRL, 0, 0xf001},
	{CS4321_RADJ_EGRESS_TX_ADD_FILL_DATA0, 0, 0x0707},
	{CS4321_RADJ_EGRESS_TX_ADD_FILL_DATA1, 0, 0x0707},
	{CS4321_RADJ_EGRESS_TX_PRA_MIN_IFG, 0, 0x0004},
	{CS4321_RADJ_EGRESS_TX_PRA_SETTLE, 0, 0x0000},
	{CS4321_RADJ_EGRESS_MISC_RESET, 0, 0x0000},

	{CS4321_PM_CTRL, 0, 0x0000},
	{CS4321_HIF_COMMON_TXCONTROL3, 0, 0x0010},
	{CS4321_MSEQ_POWER_DOWN_LSB, 0, 0xe01f},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_ingress_line_rx_1g[] = {
	{CS4321_LINE_SDS_COMMON_SRX0_RX_CLKDIV_CTRL, 0, 0x3023},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_LOOP_FILTER, 0, 0x0007},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_CPA, 0, 0x0077},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_ingress_host_rx_1g[] = {
	{CS4321_HOST_SDS_COMMON_STXP0_TX_CLKOUT_CTRL, 0, 0x1806},
	{CS4321_LINE_SDS_COMMON_RXVCO0_CONTROL, (uint16_t)~0x8000, 0x8000},
	{CS4321_LINE_SDS_COMMON_RXVCO0_CONTROL, (uint16_t)~0x8000, 0x0000},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_egress_line_rx_1g[] = {
	{CS4321_LINE_SDS_COMMON_STXP0_TX_CLKOUT_CTRL, 0, 0x1806},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_egress_host_rx_1g[] = {
	{CS4321_HOST_SDS_COMMON_SRX0_RX_CLKDIV_CTRL, 0, 0x3023},
	{CS4321_HOST_SDS_COMMON_SRX0_RX_LOOP_FILTER, 0, 0x0007},
	{CS4321_HOST_SDS_COMMON_SRX0_RX_CPA, 0, 0x0077},
	{CS4321_HOST_SDS_COMMON_RXVCO0_CONTROL, (uint16_t)~0x8000, 0x8000},
	{CS4321_HOST_SDS_COMMON_RXVCO0_CONTROL, (uint16_t)~0x8000, 0x0000},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_egress_line_through_timing_1g[] = {
	{CS4321_LINE_SDS_COMMON_TXELST0_CONTROL, 0, 0x0000},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_egress_host_through_timing_1g[] = {
	{CS4321_HOST_SDS_COMMON_TXELST0_CONTROL, 0, 0x0000},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_ingress_through_timing_mux_demux[] = {
	/* MUXPD on, EYE monitor on */
	{CS4321_LINE_SDS_COMMON_SRX0_RX_CONFIG, ~0x1200, 0},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_egress_through_timing_mux_demux[] = {
	/* DMUXPD on, MUXPD on, EYE monitor on */
	{CS4321_HOST_SDS_COMMON_SRX0_RX_CONFIG, ~0x1300, 0},
	{0},
};

static const struct cs4321_reg_modify cs4321_init_dpath_ingress_ra_1g[] = {
	/* ren_fen_gepcs = 1, fen_radj = 1, tx_fen_gepcs = 1 */
	{CS4321_GLOBAL_INGRESS_FUNCEN, ~0x484, 0x0484},
	/* rx_en_gepcs = 1, rx_en_radj = 1, rx_en_xgrs = 0, rx_en_xgpcs = 0 */
	{CS4321_GLOBAL_INGRESS_CLKEN, ~0x002D, 0x0024},
	/* rx_en_gepcs = 1, rx_en_radj = 1, rx_en_xgrs = 0, rx_en_xgpcs = 0 */
	{CS4321_GLOBAL_INGRESS_CLKEN2, ~0x002D, 0x0024},
	/* DeAassert MPIF  ingress reset */
	{CS4321_GLOBAL_MPIF_RESET_DOTREG, ~0x0004, 0x0000},
	/* Remove Line Rx/Host TX PCS Reset */
	/* line_rx_sr = 0, host_tx_sr = 0 */
	{CS4321_GLOBAL_GIGEPCS_SOFT_RESET, ~0x0201, 0x0000},
	/* set nra_in_pairs */
	{CS4321_RADJ_INGRESS_RX_NRA_EXTENT, 0, 0x0011},
	{CS4321_RADJ_INGRESS_TX_PRA_EXTENT, 0, 0x0011},
	/* Remove RA Reset */
	{CS4321_RADJ_INGRESS_MISC_RESET, 0, 0x0000},
	/* Remove PM Reset */
	{CS4321_PM_CTRL, 0, 0x0000},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_egress_ra_1g[] = {
	/* ren_fen_gepcs = 1, fen_radj = 1, tx_fen_gepcs = 1 */
	{CS4321_GLOBAL_EGRESS_FUNCEN, ~0x484, 0x0484},
	/* rx_en_gepcs = 1, rx_en_radj = 1, rx_en_xgrs = 0, rx_en_xgpcs = 0 */
	{CS4321_GLOBAL_EGRESS_CLKEN, ~0x002D, 0x0024},
	/* rx_en_gepcs = 1, rx_en_radj = 1, rx_en_xgrs = 0, rx_en_xgpcs = 0 */
	{CS4321_GLOBAL_EGRESS_CLKEN2, ~0x002D, 0x0024},
	/* DeAassert MPIF  ingress reset */
	{CS4321_GLOBAL_MPIF_RESET_DOTREG, ~0x0002, 0x0000},
	/* Remove Line Rx/Host TX PCS Reset */
	/* line_tx_sr = 0, host_rx_sr = 0 */
	{CS4321_GLOBAL_GIGEPCS_SOFT_RESET, ~0x0102, 0x0000},
	/* set nra_in_pairs */
	{CS4321_RADJ_EGRESS_RX_NRA_EXTENT, 0, 0x0011},
	{CS4321_RADJ_EGRESS_TX_PRA_EXTENT, 0, 0x0011},
	/* Remove RA Reset */
	{CS4321_RADJ_EGRESS_MISC_RESET, 0, 0x0000},
	/* Remove PM Reset */
	{CS4321_PM_CTRL, 0, 0x0000},
	{0}
};

static const struct cs4321_reg_modify cs4321_resync_vcos_1g[] = {
	{CS4321_HOST_SDS_COMMON_RXVCO0_CONTROL,  (u16)~0x8000, 0x8000},
	{CS4321_HOST_SDS_COMMON_RXVCO0_CONTROL,  (u16)~0x8000, 0},

	{CS4321_HOST_SDS_COMMON_TXVCO0_CONTROL,  (u16)~0x8000, 0x8000},
	{CS4321_HOST_SDS_COMMON_TXVCO0_CONTROL,  (u16)~0x8000, 0},

	{CS4321_LINE_SDS_COMMON_RXVCO0_CONTROL,  (u16)~0x8000, 0x8000},
	{CS4321_LINE_SDS_COMMON_RXVCO0_CONTROL,  (u16)~0x8000, 0},

	{CS4321_LINE_SDS_COMMON_TXVCO0_CONTROL,  (u16)~0x8000, 0x8000},
	{CS4321_LINE_SDS_COMMON_TXVCO0_CONTROL,  (u16)~0x8000, 0},

	{0}
};

static const struct cs4321_reg_modify cs4321_enable_aneg_1g[] = {
	/* Enable auto negotiation and restart it */
	{CS4321_GIGEPCS_LINE_DEV_ABILITY, 0, 0x1a0},
	{CS4321_GIGEPCS_LINE_CONTROL, 0, 0x1340},
	{CS4321_GIGEPCS_HOST_DEV_ABILITY, 0, 0x1a0},
	{CS4321_GIGEPCS_HOST_CONTROL, 0, 0x1340},
	{0}
};

static const struct cs4321_reg_modify cs4321_soft_reset[] = {
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0x0003},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, 0, 0x0003},
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0x0000},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, 0, 0x0000},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_line_if_mode_none[] = {
	/* Stall the microsequencer */
	{CS4321_GLOBAL_MSEQCLKCTRL, 0, (uint16_t)0x8004},
	{CS4321_MSEQ_OPTIONS, 0, 0x000f},
	{CS4321_MSEQ_PC, 0, 0x0000},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_unlock_tx_elastic_store_host[] = {
	{CS4321_LINE_SDS_COMMON_TXELST0_CONTROL, 0, 0x0000},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_rate_adj_1g[] = {
	{CS4321_RADJ_INGRESS_RX_NRA_MIN_IFG, 0, 0x0005},
	{CS4321_RADJ_INGRESS_TX_PRA_MIN_IFG, 0, 0x0005},
	{CS4321_RADJ_EGRESS_RX_NRA_MIN_IFG, 0, 0x0005},
	{CS4321_RADJ_EGRESS_TX_PRA_MIN_IFG, 0, 0x0005},
	{0}
};

/**
 * Initializes the host divider
 *
 * @see cs4321_init_line_frac_1g
 */
static const struct cs4321_reg_modify cs4321_init_host_frac_1g[] = {
	/* Initialize host divider, VCO rate: 10000, pilot: 100 */
	{CS4321_HOST_SDS_COMMON_FRAC0_RESET, 0, 0},
	/* Set the RDIV_SEL field to Fractional-N */
	{CS4321_HOST_SDS_COMMON_SRX0_RX_CLKDIV_CTRL, 0xff0f, 0x0070},
	/* Turn on the frac-N clock: provide SRX_PILOT */
	{CS4321_HOST_SDS_COMMON_FRAC0_EN, 0, 1},
	/* Setup to use a 24 bit accumulator */
	{CS4321_HOST_SDS_COMMON_FRAC0_WIDTH,
	 0, CS4321_FRACDIV_ACCUM_WIDTH_24BIT},
	/* floor(10000.0 / 8 / 100.0) = floor(12.5) */
	{CS4321_HOST_SDS_COMMON_FRAC0_INTDIV, 0, 12},
	/* (12.5 - 12) * 0x1000000 = 0x800000 */
	/* lower 16-bits */
	{CS4321_HOST_SDS_COMMON_FRAC0_NUMERATOR0, 0, 0},
	/* upper 8-bits */
	{CS4321_HOST_SDS_COMMON_FRAC0_NUMERATOR1, 0, 0x80},
	/* 0.8GHz clock */
	{CS4321_HOST_SDS_COMMON_FRAC0_1P6G_EN, 0, 1},
	/* CONFIGure stage 1 preload value */
	{CS4321_HOST_SDS_COMMON_FRAC0_STAGE1PRELOAD0, 0, 0x5DC6},
	{CS4321_HOST_SDS_COMMON_FRAC0_STAGE1PRELOAD1, 0, 0x34},
	/* CONFIGure stage 2 preload value */
	{CS4321_HOST_SDS_COMMON_FRAC0_STAGE2PRELOAD0, 0, 0x5DC6},
	{CS4321_HOST_SDS_COMMON_FRAC0_STAGE2PRELOAD1, 0, 0x34},
	/* CONFIGure stage 3 preload value */
	{CS4321_HOST_SDS_COMMON_FRAC0_STAGE3PRELOAD0, 0, 0x5DC6},
	{CS4321_HOST_SDS_COMMON_FRAC0_STAGE3PRELOAD1, 0, 0x34},
	/* Enable stage 1/2 but stage 3 is not necessary */
	{CS4321_HOST_SDS_COMMON_FRAC0_STAGE1_EN, 0, 1},
	{CS4321_HOST_SDS_COMMON_FRAC0_STAGE2_EN, 0, 1},
	{CS4321_HOST_SDS_COMMON_FRAC0_STAGE3_EN, 0, 0},
	/* Bring fractional divider out of reset */
	{CS4321_HOST_SDS_COMMON_FRAC0_RESET, 0, 1},
	{CS4321_HOST_SDS_COMMON_FRAC0_RESET, 0, 0},

	/* Re-trigger VCO coarse tuning */
	{CS4321_HOST_SDS_COMMON_RXVCO0_CONTROL, 0x7FFF, 0x8000},
	{CS4321_HOST_SDS_COMMON_RXVCO0_CONTROL, 0x7FFF, 0},
	{0},
};

/**
 * Initializes the line divider
 *
 * @see cs4321_init_host_frac_1g
 */
static const struct cs4321_reg_modify cs4321_init_line_frac_1g[] = {
	/* Initialize line divider, VCO rate: 10000, pilot: 100 */
	{CS4321_LINE_SDS_COMMON_FRAC0_RESET, 0, 0},
	/* Set the RDIV_SEL field to Fractional-N */
	{CS4321_LINE_SDS_COMMON_SRX0_RX_CLKDIV_CTRL, 0xff0f, 0x0070},
	/* Turn on the frac-N clock: provide SRX_PILOT */
	{CS4321_LINE_SDS_COMMON_FRAC0_EN, 0, 1},
	/* Setup to use a 24 bit accumulator */
	{CS4321_LINE_SDS_COMMON_FRAC0_WIDTH,
	 0, CS4321_FRACDIV_ACCUM_WIDTH_24BIT},
	/* floor(10000.0 / 8 / 100.0) = floor(12.5) */
	{CS4321_LINE_SDS_COMMON_FRAC0_INTDIV, 0, 12},
	/* (12.5 - 12) * 0x1000000 = 0x800000 */
	/* lower 16-bits */
	{CS4321_LINE_SDS_COMMON_FRAC0_NUMERATOR0, 0, 0},
	/* upper 8-bits */
	{CS4321_LINE_SDS_COMMON_FRAC0_NUMERATOR1, 0, 0x80},
	/* CONFIGure stage 1 preload value */
	{CS4321_LINE_SDS_COMMON_FRAC0_STAGE1PRELOAD0, 0, 0x5DC6},
	{CS4321_LINE_SDS_COMMON_FRAC0_STAGE1PRELOAD1, 0, 0x34},
	/* CONFIGure stage 2 preload value */
	{CS4321_LINE_SDS_COMMON_FRAC0_STAGE2PRELOAD0, 0, 0x5DC6},
	{CS4321_LINE_SDS_COMMON_FRAC0_STAGE2PRELOAD1, 0, 0x34},
	/* CONFIGure stage 3 preload value */
	{CS4321_LINE_SDS_COMMON_FRAC0_STAGE3PRELOAD0, 0, 0x5DC6},
	{CS4321_LINE_SDS_COMMON_FRAC0_STAGE3PRELOAD1, 0, 0x34},
	/* Don't need dithering enabled */
	{CS4321_LINE_SDS_COMMON_FRAC0_DITHER_EN, 0, 0},
	{CS4321_LINE_SDS_COMMON_FRAC0_DITHER_SEL, 0,
	 CS4321_FRACDIV_2EXP32_MINUS1},
	/* Enable stage 1/2 but stage 3 is not necessary */
	{CS4321_LINE_SDS_COMMON_FRAC0_STAGE1_EN, 0, 1},
	{CS4321_LINE_SDS_COMMON_FRAC0_STAGE2_EN, 0, 1},
	{CS4321_LINE_SDS_COMMON_FRAC0_STAGE3_EN, 0, 0},
	/* Bring fractional divider out of reset */
	{CS4321_LINE_SDS_COMMON_FRAC0_RESET, 0, 1},
	{CS4321_LINE_SDS_COMMON_FRAC0_RESET, 0, 0},

	/* Re-trigger VCO coarse tuning */
	{CS4321_LINE_SDS_COMMON_RXVCO0_CONTROL, 0x7FFF, 0x8000},
	{CS4321_LINE_SDS_COMMON_RXVCO0_CONTROL, 0x7FFF, 0},
	{0},
};

static const struct cs4321_reg_modify cs4321_retrigger_vcos_1g[] = {
	{CS4321_LINE_SDS_COMMON_TXVCO0_CONTROL, 0x7FFF, 0x8000},
	{CS4321_LINE_SDS_COMMON_TXVCO0_CONTROL, 0x7FFF, 0x0000},
	{CS4321_HOST_SDS_COMMON_TXVCO0_CONTROL, 0x7FFF, 0x8000},
	{CS4321_HOST_SDS_COMMON_TXVCO0_CONTROL, 0x7FFF, 0x0000},
	{CS4321_LINE_SDS_COMMON_RXVCO0_CONTROL, 0x7FFF, 0x8000},
	{CS4321_LINE_SDS_COMMON_RXVCO0_CONTROL, 0x7FFF, 0x0000},
	{CS4321_HOST_SDS_COMMON_RXVCO0_CONTROL, 0x7FFF, 0x8000},
	{CS4321_HOST_SDS_COMMON_RXVCO0_CONTROL, 0x7FFF, 0x0000},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_global_timer_156_25[] = {
	{CS4321_GLOBAL_GT_10KHZ_REF_CLK_CNT0, 0, 15625}, /* 156.25 * 100 */
	{CS4321_GLOBAL_GT_10KHZ_REF_CLK_CNT1, 0, 0},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_global_timer_100[] = {
	{CS4321_GLOBAL_GT_10KHZ_REF_CLK_CNT0, 0, 10000},
	{CS4321_GLOBAL_GT_10KHZ_REF_CLK_CNT1, 0, 0},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_mac_latency[] = {
	{CS4321_MAC_LAT_CTRL_CONFIG, 0, 0},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_ref_clk_src_xaui_rxaui[] = {
	/* Set edc_stxp_lptime_sel = 1, edc_stxp_pilot_sel = 7 */
	{CS4321_GLOBAL_MISC_CONFIG, (u16)~0xe700, 0x2700},
	/* Set STXP_PILOT_SEL = 7, STXP_LPTIME_SEL = 1 */
	{CS4321_LINE_SDS_COMMON_STXP0_TX_CONFIG, (u16)~0xe700, 0x2700},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_ref_clk_src[] = {
	/* Set edc_stxp_lptime_sel = 1, edc_stxp_pilot_sel = 7 */
	{CS4321_GLOBAL_MISC_CONFIG, (u16)~0xe700, 0x2700},
	/* Set STXP_PILOT_SEL = 7, STXP_LPTIME_SEL = 1 */
	{CS4321_LINE_SDS_COMMON_STXP0_TX_CONFIG, (u16)~0xe700, 0x2700},
	{CS4321_HOST_SDS_COMMON_STXP0_TX_CONFIG, (u16)~0xe700, 0x2700},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_polarity_inv[] = {
	/* Inversion disabled */
	/* config the slice not to invert polarity on egress */
	{CS4321_HOST_SDS_COMMON_SRX0_RX_CONFIG, ~0x0800, 0},
	/* config the slice not to invert polarity on ingress */
	{CS4321_MSEQ_ENABLE_MSB, ~0x4000, 0},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_lane_swap_xaui_rxaui[] = {
	{CS4321_HIF_COMMON_RXCONTROL0, ~0x00FF, 0x00E4},
	{CS4321_HIF_COMMON_TXCONTROL0, ~0x00FF, 0x00E4},
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0x0003},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, 0, 0xFFFF},
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, 0, 0},
	{CS4321_HOST_SDS_COMMON_RXVCO0_CONTROL, (uint16_t)(~0x8000), (uint16_t)0x8000},
	{CS4321_HOST_SDS_COMMON_RXVCO0_CONTROL, (uint16_t)(~0x8000), 0},
	{0}
};
#if 0
static const struct cs4321_reg_modify cs4321_hsif_elec_mode_set_cx1_pre[] = {
	/* Stop the micro-sequencer */
	{CS4321_GLOBAL_MSEQCLKCTRL, 0, 0x8004},
	{CS4321_MSEQ_OPTIONS, 0, 0xf},
	{CS4321_MSEQ_PC, 0, 0x0},

	{CS4321_MSEQ_COEF_DSP_DRIVE128, 0, 0x0114},
	{CS4321_MSEQ_COEF_INIT_SEL, 0, 0x0004},
	{CS4321_MSEQ_LEAK_INTERVAL_FFE, 0, 0x8010},
	{CS4321_MSEQ_BANKSELECT, 0, 0x2},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_CPA, 0, 0x55},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_LOOP_FILTER, 0, 0x3},
	{CS4321_DSP_SDS_SERDES_SRX_DFE0_SELECT, 0, 0x1},
	{CS4321_DSP_SDS_DSP_COEF_DFE0_SELECT, 0, 0x2},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_CPB, 0, 0x2003},
	{CS4321_DSP_SDS_SERDES_SRX_FFE_DELAY_CTRL, 0, 0xF047},
	{CS4321_MSEQ_RESET_COUNT_LSB, 0, 0x0},
	/* enable power savings, ignore optical module LOS */
	{CS4321_MSEQ_SPARE2_LSB, 0, 0x5},

	{CS4321_MSEQ_SPARE9_LSB, 0, 0x5},

	{CS4321_MSEQ_CAL_RX_PHSEL, 0, 0x23},
	{CS4321_DSP_SDS_DSP_COEF_LARGE_LEAK, 0, 0x2},
	{CS4321_DSP_SDS_SERDES_SRX_DAC_ENABLEB_LSB, 0, 0x5000},
	{CS4321_MSEQ_POWER_DOWN_LSB, 0, 0xFFFF},
	{CS4321_MSEQ_POWER_DOWN_MSB, 0, 0x0},
	{CS4321_MSEQ_CAL_RX_SLICER, 0, 0x80},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_SPARE, 0, 0xE0E0},
	{CS4321_DSP_SDS_SERDES_SRX_DAC_BIAS_SELECT1_MSB, 0, 0xff},

	{CS4321_MSEQ_SERDES_PARAM_LSB, 0, 0x0603},
	{CS4321_MSEQ_SPARE11_LSB, 0, 0x0603},

	{0}
};

static const struct cs4321_reg_modify cs4321_hsif_elec_mode_set_cx1_2in[] = {
	{CS4321_MSEQ_SPARE15_LSB, 0, 0x0603},
	{CS4321_MSEQ_SPARE21_LSB, 0, 0xE},
	{CS4321_MSEQ_SPARE23_LSB, 0, 0x0},
	{CS4321_MSEQ_CAL_RX_DFE_EQ, 0, 0x3},
	{0}
};

static const struct cs4321_reg_modify cs4321_hsif_elec_mode_set_cx1_post[] = {
	/* Restart the micro-sequencer */
	{CS4321_GLOBAL_MSEQCLKCTRL, 0, 0x4},
	{CS4321_MSEQ_OPTIONS, 0, 0x7},
	{0}
};
#endif

static const struct cs4321_reg_modify cs4321_assert_reset_ingress_block[] = {
	{CS4321_GLOBAL_MPIF_RESET_DOTREG, ~0x0004, 0x0004},
	{0}
};

static const struct cs4321_reg_modify cs4321_deassert_reset_ingress_block[] = {
	{CS4321_GLOBAL_MPIF_RESET_DOTREG, ~0x0004, 0},
	{0}
};

static const struct cs4321_reg_modify cs4321_assert_reset_egress_block[] = {
	{CS4321_GLOBAL_MPIF_RESET_DOTREG, ~0x0002, 0x0002},
	{0}
};

static const struct cs4321_reg_modify cs4321_deassert_reset_egress_block[] = {
	{CS4321_GLOBAL_MPIF_RESET_DOTREG, ~0x0002, 0},
	{0}
};

static const struct cs4321_reg_modify cs4321_hsif_elec_mode_set_none[] = {
	{CS4321_GLOBAL_MSEQCLKCTRL, 0, 0x8004},
	{CS4321_MSEQ_OPTIONS, 0, 0xf},
	{CS4321_MSEQ_PC, 0, 0x0},
	{0},
};

static const struct cs4321_reg_modify cs4321_hsif_elec_mode_set_sr_pre[] = {
	/* Stop the micro-sequencer */
	{CS4321_GLOBAL_MSEQCLKCTRL, 0, 0x8004},
	{CS4321_MSEQ_OPTIONS, 0, 0xf},
	{CS4321_MSEQ_PC, 0, 0x0},

	/* Configure the micro-sequencer for an SR transceiver */
	{CS4321_MSEQ_COEF_DSP_DRIVE128, 0, 0x0134},
	{CS4321_MSEQ_COEF_INIT_SEL, 0, 0x0006},
	{CS4321_MSEQ_LEAK_INTERVAL_FFE, 0, 0x8010},
	{CS4321_MSEQ_BANKSELECT, 0, 0x0},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_CPA, 0, 0x55},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_LOOP_FILTER, 0, 0x3},
	{CS4321_DSP_SDS_SERDES_SRX_DFE0_SELECT, 0, 0x1},
	{CS4321_DSP_SDS_DSP_COEF_DFE0_SELECT, 0, 0x2},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_CPB, 0, 0x2003},
	{CS4321_DSP_SDS_SERDES_SRX_FFE_DELAY_CTRL, 0, 0xF047},

	{CS4321_MSEQ_RESET_COUNT_LSB, 0, 0x0},
	/* enable power savings, ignore */
	{CS4321_MSEQ_SPARE2_LSB, 0, 0x5},
	/* enable power savings */
	{CS4321_MSEQ_SPARE9_LSB, 0, 0x5},

	{CS4321_MSEQ_CAL_RX_PHSEL, 0, 0x1e},
	{CS4321_DSP_SDS_DSP_COEF_LARGE_LEAK, 0, 0x2},
	{CS4321_DSP_SDS_SERDES_SRX_DAC_ENABLEB_LSB, 0, 0xD000},
	{CS4321_MSEQ_POWER_DOWN_LSB, 0, 0xFFFF},
	{CS4321_MSEQ_POWER_DOWN_MSB, 0, 0x0},
	{CS4321_MSEQ_CAL_RX_SLICER, 0, 0x80},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_SPARE, 0, 0xE0E0},
	{CS4321_DSP_SDS_SERDES_SRX_DAC_BIAS_SELECT1_MSB, 0, 0xff},

	{CS4321_DSP_SDS_DSP_PRECODEDINITFFE21, 0, 0x41},
	/* Setup the trace lengths for the micro-sequencer */
	{CS4321_MSEQ_SERDES_PARAM_LSB, 0, 0x0603},

	{0}
};

static const struct cs4321_reg_modify cs4321_hsif_elec_mode_set_sr_2in[] = {
	{CS4321_MSEQ_CAL_RX_EQADJ, 0, 0x0},
	{0}
};

static const struct cs4321_reg_modify cs4321_hsif_elec_mode_set_sr_post[] = {
	{CS4321_MSEQ_CAL_RX_DFE_EQ, 0, 0x0},
	/* Restart the micro-sequencer */
	{CS4321_GLOBAL_MSEQCLKCTRL, 0, 0x4},
	{CS4321_MSEQ_OPTIONS, 0, 0x7},
	{0}
};


static const struct cs4321_reg_modify cs4321_trace_line_driver_2in[] = {
	{CS4321_LINE_SDS_COMMON_STX0_TX_OUTPUT_CTRLA, 0, 0x201E},
	{CS4321_LINE_SDS_COMMON_STX0_TX_OUTPUT_CTRLB, 0, 0xC010},
	{0}
};

static const struct cs4321_reg_modify cs4321_trace_host_driver_2in[] = {
	{CS4321_HOST_SDS_COMMON_STX0_TX_OUTPUT_CTRLA, 0, 0x201E},
	{CS4321_HOST_SDS_COMMON_STX0_TX_OUTPUT_CTRLB, 0, 0xC010},
	{CS4321_HOST_ML_SDS_COMMON_STX0_TX_OUTPUT_CTRLA, 0, 0x201E},
	{CS4321_HOST_ML_SDS_COMMON_STX0_TX_OUTPUT_CTRLB, 0, 0xC010},
	{0}
};

static const struct cs4321_reg_modify cs4321_trace_line_equal_2in[] = {
	{CS4321_LINE_SDS_COMMON_SRX0_RX_MISC, 0, 0x0011},
	{0}
};

static const struct cs4321_reg_modify cs4321_resync_vcos_xaui_rxaui[] = {
	{CS4321_HOST_SDS_COMMON_RXVCO0_CONTROL,  (u16)~0x8000, 0x8000},
	{CS4321_HOST_SDS_COMMON_RXVCO0_CONTROL,  (u16)~0x8000, 0},

	{CS4321_HOST_SDS_COMMON_TXVCO0_CONTROL,  (u16)~0x8000, 0x8000},
	{CS4321_HOST_SDS_COMMON_TXVCO0_CONTROL,  (u16)~0x8000, 0},

	{CS4321_LINE_SDS_COMMON_RXVCO0_CONTROL,  (u16)~0x8000, 0x8000},
	{CS4321_LINE_SDS_COMMON_RXVCO0_CONTROL,  (u16)~0x8000, 0},

	{CS4321_LINE_SDS_COMMON_TXVCO0_CONTROL,  (u16)~0x8000, 0x8000},
	{CS4321_LINE_SDS_COMMON_TXVCO0_CONTROL,  (u16)~0x8000, 0},

	{CS4321_HOST_ML_SDS_COMMON_RXVCO0_CONTROL,  (u16)~0x8000, 0x8000},
	{CS4321_HOST_ML_SDS_COMMON_RXVCO0_CONTROL,  (u16)~0x8000, 0},

	{CS4321_HOST_ML_SDS_COMMON_TXVCO0_CONTROL,  (u16)~0x8000, 0x8000},
	{CS4321_HOST_ML_SDS_COMMON_TXVCO0_CONTROL,  (u16)~0x8000, 0},

	{0}
};

static const struct cs4321_reg_modify cs4321_powerup_ml_serdes[] = {
	{CS4321_HOST_ML_SDS_COMMON_SRX0_RX_CONFIG, ~0x0020, 0},
	{CS4321_HOST_ML_SDS_COMMON_STXP0_TX_PWRDN, ~0x0100, 0},
	{0}
};

static const struct cs4321_reg_modify cs4321_powerdown_ml_serdes[] = {
	{CS4321_HOST_ML_SDS_COMMON_SRX0_RX_CONFIG, ~0x0020, 0x0020},
	{CS4321_HOST_ML_SDS_COMMON_STXP0_TX_PWRDN, ~0x0100, 0x0100},
	{0}
};

static const struct cs4321_reg_modify cs4321_toggle_resets_xaui_rxaui[] = {
	/*
	 * Now that the device is configured toggle the ingress and
	 * egress soft resets to make sure the device re-syncs
	 * properly.
	 */
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0x3},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, 0, 0x3},
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0x0000},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, 0, 0x0000},

	{0}
};

static const struct cs4321_reg_modify cs4321_init_trace_2in_host_xaui[] = {
	{CS4321_HOST_SDS_COMMON_STX0_TX_OUTPUT_CTRLA, 0, 0x3030},
	{CS4321_HOST_SDS_COMMON_STX0_TX_OUTPUT_CTRLB, 0, 0xC003},
	{CS4321_HOST_ML_SDS_COMMON_STX0_TX_OUTPUT_CTRLA, 0, 0x3030},
	{CS4321_HOST_ML_SDS_COMMON_STX0_TX_OUTPUT_CTRLB, 0, 0xC003},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_line_equalize_2in[] = {
	{CS4321_LINE_SDS_COMMON_SRX0_RX_MISC, 0, 0x0011},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_egress_through_timing_xaui_1[] = {
	{CS4321_LINE_SDS_COMMON_STX0_TX_CONFIG_LOCAL_TIMING, 0, 0x0001},
	{CS4321_LINE_SDS_COMMON_STXP0_TX_CLKOUT_CTRL, 0, 0x0864},
	{CS4321_LINE_SDS_COMMON_STXP0_TX_LOOP_FILTER, 0, 0x0027},
	{CS4321_LINE_SDS_COMMON_STXP0_TX_PWRDN, 0, 0x0000},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_egress_through_timing_xaui_2e[] = {
	{CS4321_LINE_SDS_COMMON_STXP0_TX_CONFIG, (uint16_t)(~0xF800), 0x1000},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_egress_through_timing_xaui_2o[] = {
	{CS4321_LINE_SDS_COMMON_STXP0_TX_CONFIG, (uint16_t)(~0xF800), 0x0800},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_egress_through_timing_xaui_3[] = {
	{CS4321_HOST_SDS_COMMON_SRX0_RX_CLKDIV_CTRL, 0, 0x45d2},
	{CS4321_HOST_SDS_COMMON_SRX0_RX_CONFIG, 0, 0x000c},
	{CS4321_HOST_ML_SDS_COMMON_SRX0_RX_CLKDIV_CTRL, 0, 0x412d},
	{CS4321_HOST_ML_SDS_COMMON_SRX0_RX_CONFIG, 0, 0x000c},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_egress_through_timing_xaui_4e[] = {
	{CS4321_HOST_SDS_COMMON_SRX0_RX_CLKOUT_CTRL, 0, 0x6a05},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_egress_through_timing_xaui_4o[] = {
	{CS4321_HOST_SDS_COMMON_SRX0_RX_CLKOUT_CTRL, 0, 0x6a03},
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_egress_through_timing_xaui_5[] = {
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0x0003},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, 0, 0xffff},
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0x0000},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, 0, 0x0000},
	{CS4321_GLOBAL_REF_SOFT_RESET, 0, 0xffff},
	{CS4321_GLOBAL_REF_SOFT_RESET, 0, 0x0000},
#if 0
	/* Configure for 2 in tracelength to pass XAUI mask (2 inch results in
	 * same settings)
	 */
	/* Line driver */
	{CS4321_LINE_SDS_COMMON_STX0_TX_OUTPUT_CTRLA, 0, 0x101E},
	{CS4321_LINE_SDS_COMMON_STX0_TX_OUTPUT_CTRLB, 0, 0xC010},
	/* host XAUI driver */
	{CS4321_HOST_SDS_COMMON_STX0_TX_OUTPUT_CTRLA, 0, 0x3030},
	{CS4321_HOST_SDS_COMMON_STX0_TX_OUTPUT_CTRLB, 0, 0xc003},
	{CS4321_HOST_ML_SDS_COMMON_STX0_TX_OUTPUT_CTRLA, 0, 0x3030},
	{CS4321_HOST_ML_SDS_COMMON_STX0_TX_OUTPUT_CTRLB, 0, 0xc003},
	/* Line equalizer */
	{CS4321_LINE_SDS_COMMON_SRX0_RX_MISC, 0, 0x0011},
#endif
	{0}
};

static const struct cs4321_reg_modify
cs4321_init_ingress_local_timing_xaui[] = {
	{CS4321_LINE_SDS_COMMON_SRX0_RX_CONFIG, 0, 0x0000},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_CPA, 0, 0x0057},
	{CS4321_LINE_SDS_COMMON_SRX0_RX_LOOP_FILTER, 0, 0x0007},

	{CS4321_HOST_SDS_COMMON_STX0_TX_CONFIG_LOCAL_TIMING, 0, 0x0001},
	{CS4321_HOST_SDS_COMMON_STXP0_TX_CLKDIV_CTRL, 0, 0x4492},
	{CS4321_HOST_SDS_COMMON_STXP0_TX_CLKOUT_CTRL, 0, 0x1864},
	{CS4321_HOST_SDS_COMMON_STXP0_TX_CONFIG, 0, 0x090c},
	{CS4321_HOST_SDS_COMMON_STXP0_TX_PWRDN, 0, 0x0000},
	{CS4321_HOST_ML_SDS_COMMON_STXP0_TX_CLKDIV_CTRL, 0, 0x4429},
	{CS4321_HOST_ML_SDS_COMMON_STXP0_TX_CONFIG, 0, 0x090c},

	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0x0003},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, 0, 0xffff},
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0x0000},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, 0, 0x0000},
	{CS4321_GLOBAL_REF_SOFT_RESET, 0, 0xffff},
	{CS4321_GLOBAL_REF_SOFT_RESET, 0, 0x0000},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_ingress_1[]
    = {
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0x0002},
	{CS4321_GLOBAL_INGRESS_SOFT_RESET, 0, 0x0000},
	{CS4321_HOST_SDS_COMMON_STXP0_TX_PWRDN, 0, 0x0000},
	{0}
};

/* even slice */
static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_2e[] = {
	{CS4321_GLOBAL_HOST_MULTILANE_CLKSEL, 0, 0x8000},
	{0}
};

/* odd slice */
static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_2o[] = {
	{CS4321_GLOBAL_HOST_MULTILANE_CLKSEL, 0, 0x8300},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_ingress_3[]
    = {
	/* Set the device in XAUI mode */
	{CS4321_GLOBAL_HOST_MULTILANE_FUNCEN, 0, 0x0005},

	/* Enable the XGPCS and the Rate Adjust block */
	/* Set fen_radj, rx_fen_xgpcs */
	{CS4321_GLOBAL_INGRESS_FUNCEN, ~0x0081, 0x0081},

	/* Setup the clock enables for the XGPCS and Rate Adjust block */
	/* Set rx_en_radj, rx_en_xgpcs */
	{CS4321_GLOBAL_INGRESS_CLKEN, ~0x0021, 0x0021},

	/* Setup the clock enables for the HIF and the Rate Adjust block */
	/* Set tx_en_hif, tx_en_radj */
	{CS4321_GLOBAL_INGRESS_CLKEN2, ~0x0120, 0x0120},

	{CS4321_GLOBAL_REF_SOFT_RESET, 0, 0xffff},
	{CS4321_GLOBAL_REF_SOFT_RESET, 0, 0x0000},

	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_ingress_4e[]
    = {
	{CS4321_XGMAC_LINE_RX_CFG_COM, 0, 0x8010},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_ingress_4o[]
    = {
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_ingress_5[]
    = {
	{CS4321_XGMAC_HOST_TX_CFG_TX_IFG, 0, 0x0005},
	{CS4321_XGPCS_LINE_RX_RXCNTRL, 0, 0x5000},
	{CS4321_XGRS_HOST_TX_TXCNTRL, 0, 0xc000},
	{CS4321_GIGEPCS_LINE_CONTROL, 0, 0x0000},
	{CS4321_GIGEPCS_HOST_CONTROL, 0, 0x0000},

	{CS4321_RADJ_INGRESS_RX_NRA_MIN_IFG, 0, 0x0004},
	{CS4321_RADJ_INGRESS_RX_NRA_SETTLE, 0, 0x0000},
	{CS4321_RADJ_INGRESS_TX_ADD_FILL_CTRL, 0, 0xf001},
	{CS4321_RADJ_INGRESS_TX_ADD_FILL_DATA0, 0, 0x0707},
	{CS4321_RADJ_INGRESS_TX_ADD_FILL_DATA1, 0, 0x0707},
	{CS4321_RADJ_INGRESS_TX_PRA_MIN_IFG, 0, 0x0004},
	{CS4321_RADJ_INGRESS_TX_PRA_SETTLE, 0, 0x0000},
	{CS4321_RADJ_INGRESS_MISC_RESET, 0, 0x0000},
	{CS4321_PM_CTRL, 0, 0x0002},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_6e[] = {
	{CS4321_HIF_COMMON_TXCONTROL3, 0, 0x0010},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_6o[] = {
	{CS4321_HIF_COMMON_TXCONTROL3, 0, 0x0011},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_egress_1[] = {
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, 0, 0x1},
	{CS4321_GLOBAL_EGRESS_SOFT_RESET, 0, 0x0000},
	{CS4321_LINE_SDS_COMMON_STXP0_TX_PWRDN, 0, 0x0000},

	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_egress_2e[] = {
	{CS4321_GLOBAL_HOST_MULTILANE_CLKSEL, 0, 0x8000},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_egress_2o[] = {
	{CS4321_GLOBAL_HOST_MULTILANE_CLKSEL, 0, 0x8300},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_egress_3[] = {
	/* Set the device in XAUI mode */
	{CS4321_GLOBAL_HOST_MULTILANE_FUNCEN, 0, 0x0005},

	/* Enable the XGPCS and the Rate Adjust block */
	/* Set tx_fen_xgpcs, fen_radj */
	{CS4321_GLOBAL_EGRESS_FUNCEN, ~0x0180, 0x0180},

	/* Setup the clock enables for the HIF and the Rate Adjust block */
	/* Set rx_en_hif, rx_en_radj */
	{CS4321_GLOBAL_EGRESS_CLKEN, ~0x0120, 0x0120},

	/* Setup the clock enables for the XGPCS and Rate Adjust block */
	/* Set tx_en_radj, tx_en_xgpcs */
	{CS4321_GLOBAL_EGRESS_CLKEN2, ~0x0021, 0x0021},

	{CS4321_GLOBAL_REF_SOFT_RESET, 0, 0xffff},
	{CS4321_GLOBAL_REF_SOFT_RESET, 0, 0x0000},

	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_egress_4e[]
    = {
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_egress_4o[]
    = {
	{CS4321_XGMAC_LINE_TX_CFG_COM, 0, 0xc000},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_egress_5[] = {
	{CS4321_XGMAC_LINE_TX_CFG_TX_IFG, 0, 0x0005},
	{CS4321_XGPCS_LINE_TX_TXCNTRL, 0, 0x0000},
	{CS4321_XGRS_LINE_TX_TXCNTRL, 0, 0xc000},

	{CS4321_RADJ_EGRESS_RX_NRA_MIN_IFG, 0, 0x0004},
	{CS4321_RADJ_EGRESS_RX_NRA_SETTLE, 0, 0x0000},
	{CS4321_RADJ_EGRESS_TX_ADD_FILL_CTRL, 0, 0xf001},
	{CS4321_RADJ_EGRESS_TX_ADD_FILL_DATA0, 0, 0x0707},
	{CS4321_RADJ_EGRESS_TX_ADD_FILL_DATA1, 0, 0x0707},
	{CS4321_RADJ_EGRESS_TX_PRA_MIN_IFG, 0, 0x0004},
	{CS4321_RADJ_EGRESS_TX_PRA_SETTLE, 0, 0x0000},
	{CS4321_RADJ_EGRESS_MISC_RESET, 0, 0x0000},
	{CS4321_PM_CTRL, 0, 0x0002},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_egress_6e[] = {
	{CS4321_HIF_COMMON_TXCONTROL3, 0, 0x0010},
	{0}
};

static const struct cs4321_reg_modify cs4321_init_dpath_xaui_pcs_ra_egress_6o[] = {
	{CS4321_HIF_COMMON_TXCONTROL3, 0, 0x0011},
	{0}
};

const struct cs4321_multi_seq cs4321_init_rxaui_seq[] = {
	{0, cs4321_init_prefix_seq},
	{0, cs4321_init_egress_local_timing_rxaui},
	{0, cs4321_init_ingress_local_timing_rxaui},
	{0, cs4321_init_lane_swap_xaui_rxaui},
	{0, cs4321_init_dpath_ingress_rxaui_pcs_ra},
	{0, cs4321_init_dpath_egress_rxaui_pcs_ra},
	{0, cs4321_resync_vcos_xaui_rxaui},
	{0, cs4321_powerup_ml_serdes},
	{0, cs4321_toggle_resets_xaui_rxaui},
	{0, cs4321_hsif_elec_mode_set_sr_pre},
	{0, cs4321_hsif_elec_mode_set_sr_2in},
	{0, cs4321_hsif_elec_mode_set_sr_post},
	{0, cs4321_trace_host_driver_2in},
	{0, cs4321_trace_line_driver_2in},
	{0, cs4321_init_line_equalize_2in},
	{0, cs4321_init_global_timer_156_25},
	{0, cs4321_init_mac_latency},
	{0, cs4321_init_ref_clk_src_xaui_rxaui},
	{0, cs4321_init_polarity_inv},
	{1, cs4321_init_prefix_seq},
	{1, cs4321_init_egress_local_timing_rxaui},
	{1, cs4321_init_ingress_local_timing_rxaui},
	{1, cs4321_init_lane_swap_xaui_rxaui},
	{1, cs4321_init_dpath_ingress_rxaui_pcs_ra},
	{1, cs4321_init_dpath_egress_rxaui_pcs_ra},
	{1, cs4321_resync_vcos_xaui_rxaui},
	{1, cs4321_powerup_ml_serdes},
	{1, cs4321_toggle_resets_xaui_rxaui},
	{1, cs4321_hsif_elec_mode_set_none},
	{1, cs4321_trace_host_driver_2in},
	{1, cs4321_trace_line_driver_2in},
	{1, cs4321_trace_line_equal_2in},
	{1, cs4321_init_global_timer_156_25},
	{1, cs4321_init_mac_latency},
	{1, cs4321_init_ref_clk_src_xaui_rxaui},
	{1, cs4321_init_polarity_inv},



	{0, NULL}
};

const struct cs4321_multi_seq cs4321_init_xaui_seq[] = {
	{0, cs4321_init_prefix_seq},
	/* Init egress even and odd */
	{0, cs4321_init_egress_through_timing_xaui_1},
	{0, cs4321_init_egress_through_timing_xaui_2e},
	{0, cs4321_init_egress_through_timing_xaui_3},
	{0, cs4321_init_egress_through_timing_xaui_4e},
	{0, cs4321_init_egress_through_timing_xaui_5},

	{1, cs4321_init_egress_through_timing_xaui_1},
	{1, cs4321_init_egress_through_timing_xaui_2o},
	{1, cs4321_init_egress_through_timing_xaui_3},
	{1, cs4321_init_egress_through_timing_xaui_4o},
	{1, cs4321_init_egress_through_timing_xaui_5},

	/* Init ingress even and odd */
	{0, cs4321_init_ingress_local_timing_xaui},
	{1, cs4321_init_ingress_local_timing_xaui},

	{0, cs4321_init_lane_swap_xaui_rxaui},

	/* dpath ingress even and odd */
	{0, cs4321_init_dpath_xaui_pcs_ra_ingress_1},
	{0, cs4321_init_dpath_xaui_pcs_ra_2e},
	{0, cs4321_init_dpath_xaui_pcs_ra_ingress_3},
	{0, cs4321_deassert_reset_ingress_block},
	{0, cs4321_init_dpath_xaui_pcs_ra_ingress_4e},
	{0, cs4321_init_dpath_xaui_pcs_ra_ingress_5},
	{0, cs4321_init_dpath_xaui_pcs_ra_6e},

	{1, cs4321_init_dpath_xaui_pcs_ra_ingress_1},
	{1, cs4321_init_dpath_xaui_pcs_ra_2o},
	{1, cs4321_init_dpath_xaui_pcs_ra_ingress_3},
	{0, cs4321_deassert_reset_ingress_block},
	{1, cs4321_init_dpath_xaui_pcs_ra_ingress_4o},
	{1, cs4321_init_dpath_xaui_pcs_ra_ingress_5},
	{1, cs4321_init_dpath_xaui_pcs_ra_6o},
	/*{1, cs4321_init_line_power_down},*/

	/* dpath egress even and odd */
	{0, cs4321_init_dpath_xaui_pcs_ra_egress_1},
	{0, cs4321_init_dpath_xaui_pcs_ra_2e},
	{0, cs4321_init_dpath_xaui_pcs_ra_egress_3},
	{0, cs4321_deassert_reset_egress_block},
	{0, cs4321_init_dpath_xaui_pcs_ra_egress_4e},
	{0, cs4321_init_dpath_xaui_pcs_ra_egress_5},
	{0, cs4321_init_dpath_xaui_pcs_ra_6e},

	{1, cs4321_init_dpath_xaui_pcs_ra_egress_1},
	{1, cs4321_init_dpath_xaui_pcs_ra_2o},
	{1, cs4321_init_dpath_xaui_pcs_ra_egress_3},
	{0, cs4321_deassert_reset_egress_block},
	{1, cs4321_init_dpath_xaui_pcs_ra_egress_4o},
	{1, cs4321_init_dpath_xaui_pcs_ra_egress_5},
	{1, cs4321_init_dpath_xaui_pcs_ra_6o},

	/* power down the odd slice's line side */
/*	{1, cs4321_init_line_power_down},*/

	{0, cs4321_resync_vcos_xaui_rxaui},
	{0, cs4321_powerup_ml_serdes},
	{0, cs4321_toggle_resets_xaui_rxaui},
	{0, cs4321_hsif_elec_mode_set_sr_pre},
	{0, cs4321_hsif_elec_mode_set_sr_2in},
	{0, cs4321_hsif_elec_mode_set_sr_post},
	{0, cs4321_init_trace_2in_host_xaui},
	{0, cs4321_trace_line_driver_2in},
	{0, cs4321_trace_line_equal_2in},
	{0, cs4321_init_global_timer_156_25},
	{0, cs4321_init_mac_latency},
	{0, cs4321_init_ref_clk_src_xaui_rxaui},
	{0, cs4321_init_polarity_inv},
	{1, cs4321_init_prefix_seq},
	{1, cs4321_init_lane_swap_xaui_rxaui},
	{1, cs4321_init_line_power_down},
	{1, cs4321_resync_vcos_xaui_rxaui},
	{1, cs4321_powerup_ml_serdes},
	{1, cs4321_toggle_resets_xaui_rxaui},
	{1, cs4321_hsif_elec_mode_set_none},
	{1, cs4321_init_trace_2in_host_xaui},
	{1, cs4321_trace_line_equal_2in},
	{1, cs4321_trace_line_driver_2in},
	{1, cs4321_init_global_timer_156_25},
	{1, cs4321_init_mac_latency},
	{1, cs4321_init_ref_clk_src_xaui_rxaui},
	{1, cs4321_init_polarity_inv},
	{0, NULL}
};

const struct cs4321_multi_seq cs4321_init_sgmii_seq[] = {
	{0, cs4321_init_prefix_seq},
	{0, cs4321_init_egress_host_rx_1g},
	{0, cs4321_init_egress_line_rx_1g},
	{0, cs4321_init_unlock_tx_elastic_store_host},
	{0, cs4321_init_egress_local_host_timing_mux_demux},

	{0, cs4321_init_ingress_line_rx_1g},
	{0, cs4321_init_ingress_host_rx_1g},
	{0, cs4321_init_egress_host_through_timing_1g},
	{0, cs4321_init_ingress_through_timing_mux_demux},
	{0, cs4321_init_dpath_ingress_ra_1g},
	{0, cs4321_powerdown_ml_serdes},
	{0, cs4321_init_dpath_egress_ra_1g},
	{0, cs4321_resync_vcos_1g},
	{0, cs4321_soft_reset},
	{0, cs4321_init_line_if_mode_none},
 	{0, cs4321_init_global_timer_100},
	{0, cs4321_init_mac_latency},
	{0, cs4321_init_ref_clk_src},
	{0, cs4321_init_polarity_inv},
	{0, cs4321_init_rate_adj_1g},
	{0, cs4321_init_host_frac_1g},
	{0, cs4321_init_line_frac_1g},
	{0, cs4321_retrigger_vcos_1g},
	{0, cs4321_enable_aneg_1g},
	{0, NULL}
};

static inline void msleep(ulong time)
{
	mdelay(time);
}


static int cs4321_phy_read_x(struct phy_device *phydev, int off, u16 regnum)
{
	int ret;
	struct phy_device dummy;

	memcpy(&dummy, phydev, sizeof(dummy));
	dummy.addr += off;
#if 0
	ret =  mdiobus_read(phydev->bus, phydev->addr + off,
			    MII_ADDR_C45 | regnum);
#endif
	ret = phy_read(&dummy, 0, regnum);
	if (debug_phyio)
		debug("r h: 0x%x, addr: 0x%04x, data: 0x%04x\n",
		      phydev->addr << 8 | off, regnum, ret);
	return ret;
}

static int cs4321_phy_write_x(struct phy_device *phydev, int off, u16 regnum,
			      u16 val)
{
	struct phy_device dummy;

	memcpy(&dummy, phydev, sizeof(dummy));
	dummy.addr += off;
	if (debug_phyio)
		debug("w h: 0x%x, addr: 0x%04x, data: 0x%04x\n",
		      phydev->addr << 8 | off, regnum, val);
#if 0
	return mdiobus_write(phydev->bus, phydev->addr + off,
			     MII_ADDR_C45 | regnum, val);
#endif
	return phy_write(&dummy, 0, regnum, val);
}

static int cs4321_phy_read(struct phy_device *phydev, u16 regnum)
{
	return cs4321_phy_read_x(phydev, 0, regnum);
}

static int cs4321_phy_write(struct phy_device *phydev, u16 regnum, u16 val)
{
	return cs4321_phy_write_x(phydev, 0, regnum, val);
}

static int cs4321_write_seq_x(struct phy_device *phydev, int off,
			      const struct cs4321_reg_modify *seq)
{
	int last_reg = -1;
	int last_val = 0;
	int ret = 0;

	while (seq->reg) {
		if (seq->mask_bits) {
			if (last_reg != seq->reg) {
				ret = cs4321_phy_read_x(phydev, off, seq->reg);
				if (ret < 0)
					goto err;
				last_val = ret;
			}
			last_val &= seq->mask_bits;
		} else {
			last_val = 0;
		}
		last_val |= seq->set_bits;
		ret = cs4321_phy_write_x(phydev, off, seq->reg, last_val);
		if (ret < 0)
			goto err;
		seq++;
	}
err:
	return ret;
}

static int cs4321_write_multi_seq_x(struct phy_device *phydev, int off,
				  const struct cs4321_multi_seq *m)
{
	int ret = 0;

	while (m->seq) {
		debug("// %s(%d, %d, 0x%p)\n", __func__, phydev->addr, off, m->seq);
		ret = cs4321_write_seq_x(phydev, m->reg_offset + off, m->seq);
		if (ret)
			goto err;
		m++;
	}

err:
	return ret;
}

static int cs4321_write_multi_seq(struct phy_device *phydev,
				  const struct cs4321_multi_seq *m)
{
	return cs4321_write_multi_seq_x(phydev, 0, m);
}

static int cs4321_get_hw_revision(struct phy_device *phydev)
{
	static uint32_t revision = 0;

	if (revision == 0) {
		uint16_t data;

		data = cs4321_phy_read(phydev, CS4321_GLOBAL_CHIP_ID_MSB);
		switch (data) {
		case 0x1002:
			revision = CS4321_HW_REVA;
			break;
		default:
			revision = CS4321_HW_REVB;
			break;
		}
	}
	return revision;
}

static int cs4321_write_seq(struct phy_device *phydev,
			    const struct cs4321_reg_modify *seq)
{
	return cs4321_write_seq_x(phydev, 0, seq);
}

static int cs4321_write_firmware(struct phy_device *phydev, int off)
{
	int bank;
	uint32_t *value;
	uint16_t addr;
	int ret = 0;
	int checksum_status;

	debug("%s: Entry, addr: %d, offset: %d\n", __func__, phydev->addr, off);
	/* Toggle the checksum calculator.  Note that the image will assert
	 * the checksum bit to start calculating the checksum.
	 */
	cs4321_phy_write_x(phydev, off, CS4321_GLOBAL_DWNLD_CHECKSUM_CTRL, 0x01);
	cs4321_phy_write_x(phydev, off, CS4321_GLOBAL_DWNLD_CHECKSUM_CTRL, 0x00);

	ret = cs4321_write_seq_x(phydev, off, cs4321_pre_ucode_load_init);
	if (ret) {
		printf("%s: Error writing pre-ucode load init sequence\n",
		       __func__);
		return ret;
	}

	for (bank = 0; bank < CS4321_BANK_COUNT; bank++) {
		cs4321_phy_write_x(phydev, off, CS4321_MSEQ_BANKSELECT, bank);
		value = &cs4321_microcode_image_banks[bank][0];
		for (addr = CS4321_BANK_START_ADDR;
		     addr < CS4321_BANK_START_ADDR + CS4321_BANK_SIZE; addr++) {
			cs4321_phy_write_x(phydev, off, 0x201, *value >> 16);
			cs4321_phy_write_x(phydev, off, 0x202, *value & 0xffff);
			cs4321_phy_write_x(phydev, off, 0x200, addr);
			value++;
		}
	}

	debug("%s: Writing other firmware starting at address 0x%x\n",
	      __func__, CS4321_BANK_OTHER_START_ADDR);
	value = &cs4321_microcode_image_other[0];
	for (addr = CS4321_BANK_OTHER_START_ADDR;
	     addr < CS4321_BANK_OTHER_START_ADDR +CS4321_BANK_OTHER_SIZE;
	     addr++) {
		cs4321_phy_write_x(phydev, off, 0x201, *value >> 16);
		cs4321_phy_write_x(phydev, off, 0x202, *value & 0xffff);
		cs4321_phy_write_x(phydev, off, 0x200, addr);
		value++;
	}

	debug("%s: Writing post sequence\n", __func__);
	ret = cs4321_write_seq_x(phydev, off, cs4321_post_ucode_load_init);
	if (ret) {
		printf("%s: Error writing post ucode load init sequence\n",
		       __func__);
	}

	checksum_status = cs4321_phy_read_x(phydev, off,
					  CS4321_GLOBAL_DWNLD_CHECKSUM_STATUS);
	if (checksum_status == 0x1) {
		printf("ERROR: Calculated checksum does not match image checksum\n");
		ret = -1;
	}
	debug("%s: Finished\n", __func__);
	return ret;
}

static int cs4321_reset_no_fw(struct phy_device *phydev, int off)
{
	int ret;
	int retry;

	ret = cs4321_phy_write_x(phydev, off,
					 CS4321_GLOBAL_MPIF_SOFT_RESET, 0xdead);
	if (ret)
		goto err;

	msleep(100);

	/* Disable eeprom loading */
	ret = cs4321_phy_write_x(phydev, off,
				 CS4321_EEPROM_LOADER_CONTROL, 2);
	if (ret)
		goto err;

	retry = 0;
	do {
		if (retry > 0)
			msleep(1);
		ret = cs4321_phy_read_x(phydev, off,
					CS4321_EEPROM_LOADER_STATUS);
		if (ret < 0)
			goto err;
		retry++;
	} while ((ret & 4) == 0 && retry < 10);

	if ((ret & 4) == 0) {
		debug("%s: Error waiting for loader status\n", __func__);
		ret = -ENXIO;
		goto err;
	}

	ret = cs4321_write_seq_x(phydev, off,
				 cs4321_soft_reset_registers);
	if (ret)
		goto err;

err:
	return ret;
}

int cs4321_init_rxaui(struct phy_device *phydev)
{
	debug("%s: Initializing RXAUI interface at address %d\n",
	      __func__, phydev->addr);
	return cs4321_write_multi_seq(phydev, cs4321_init_rxaui_seq);
}

int cs4321_init_xaui(struct phy_device *phydev)
{
	int ret;
	debug("%s: Initializing XAUI interface at address %d\n",
	      __func__, phydev->addr);

	ret = cs4321_write_multi_seq(phydev, cs4321_init_xaui_seq);
	return ret;
}

int cs4321_init_sgmii(struct phy_device *phydev)
{
	debug("%s: Initializing SGMII interface at address %d\n",
	      __func__, phydev->addr);
	return cs4321_write_multi_seq(phydev, cs4321_init_sgmii_seq);
}

int cs4321_reset(struct phy_device *phydev)
{
	struct cs4321_private *p = (struct cs4321_private *)phydev->priv;
	int ret;

	debug("%s: Entry, addr: %d\n", __func__, phydev->addr);
	debug_phyio = 1;
	/* The reset sequence is performed for each of the four phys */
	cs4321_reset_no_fw(phydev, 0);

	/* SGMII mode does not use the firmware */
	if (p->mode != CS4321_HOST_MODE_SGMII) {
		debug("Performing XAUI/RXAUI initialization\n");
		/* The firmware is only loaded on the even PHY numbers */
		cs4321_reset_no_fw(phydev, 0);
		ret = cs4321_write_firmware(phydev, 0);
		if (ret)
			goto err;
	}

	ret = cs4321_write_seq(phydev, cs4321_68xx_4_nic_init);
	if (ret)
		goto err;

err:
	return ret;
}

int cs4321_config_init(struct phy_device *phydev)
{
	int ret;
	struct cs4321_private *p = phydev->priv;
	const struct cs4321_multi_seq *init_seq;
	int i;

	debug("%s: Entry, addr: %d\n", __func__, phydev->addr);

	p->mode = cortina_cs4321_get_host_mode(phydev);

	switch (p->mode) {
	case CS4321_HOST_MODE_XAUI:
		debug("%s: init sequence: %s\n", __func__, "XAUI");
		init_seq = cs4321_init_xaui_seq;
		break;
	case CS4321_HOST_MODE_RXAUI:
		debug("%s: init sequence: %s\n", __func__, "RXAUI");
		init_seq = cs4321_init_rxaui_seq;
		break;
	case CS4321_HOST_MODE_SGMII:
		debug("%s: init sequence: %s\n", __func__, "SGMII");
		init_seq = cs4321_init_sgmii_seq;
		break;
	default:
		printf("Unknown PHY mode %d\n", p->mode);
		return -1;
	}
	debug_phyio = 1;
	ret = cs4321_reset(phydev);
	if (ret) {
		printf("%s: cs4321_reset returned error\n", __func__);
		goto err;
	}

	debug("%s: Writing initialization sequence\n", __func__);

	ret = cs4321_write_multi_seq(phydev, init_seq);
	if (ret)
		goto err;

	debug_phyio = 0;
#ifndef __U_BOOT__
	phydev->state = PHY_NOLINK;
#endif
	p->initialized = 1;
err:
	return ret;
}


int cs4321_probe(struct phy_device *phydev)
{
	int ret = 0;
	int id_lsb, id_msb;
#ifndef __U_BOOT__
	const char *prop_val;
#endif
	struct cs4321_private *p;

	debug("%s: Entry\n", __func__);
	debug("%s: addr: %d, eth index: %d\n", __func__,
	      phydev->addr, phydev->dev->index);
	debug("%s: bus: 0x%p, drv: 0x%p, dev: 0x%p\n",
	      __func__, phydev->bus, phydev->drv, phydev->dev);
	/*
	 * CS4312 keeps its ID values in non-standard registers, make
	 * sure we are talking to what we think we are.
	 */
	id_lsb = cs4321_phy_read(phydev, CS4321_GLOBAL_CHIP_ID_LSB);
	if (id_lsb < 0) {
		debug("%s: Error reading LSB\n", __func__);
		ret = id_lsb;
		goto err;
	}

	id_msb = cs4321_phy_read(phydev, CS4321_GLOBAL_CHIP_ID_MSB);
	if (id_msb < 0) {
		debug("%s: Error reading MSB\n", __func__);
		ret = id_msb;
		goto err;
	}

	debug("%s: id: %04x:%04x\n", __func__, id_msb, id_lsb);
	if (id_lsb != 0x23E5 || id_msb != 0x1002) {
		debug("%s: Unrecognized lsb 0x%x, msb 0x%x\n",
		      __func__, id_lsb, id_msb);
		ret = -ENODEV;
		goto err;
	}
#ifdef __U_BOOT__

	p = (struct cs4321_private *)calloc(sizeof(struct cs4321_private), 1);
	if (!p) {
		ret = -ENOMEM;
		debug("%s: Cannot alloc %u bytes\n",
		      __func__, sizeof(struct cs4321_private));
		goto err;
	}
#else
	ret =
	    of_property_read_string(phydev->dev.of_node, "cortina,host-mode",
				    &prop_val);

	if (ret)
		goto err;

	if (strcmp(prop_val, "rxaui") == 0)
		host_mode = RXAUI;
	else if (strcmp(prop_val, "xaui") == 0)
		host_mode = XAUI;
	else {
		dev_err(&phydev->dev,
			"Invalid \"cortina,host-mode\" property: \"%s\"\n",
			prop_val);
		ret = -EINVAL;
		goto err;
	}
	p = devm_kzalloc(&phydev->dev, sizeof(*p), GFP_KERNEL);
	if (!p) {
		ret = -ENOMEM;
		goto err;
	}
#endif
	p->mode = CS4321_HOST_MODE_UNKNOWN;
	p->fw_loaded = 0;
	phydev->priv = p;

err:
	return ret;
}

int cs4321_config_aneg(struct phy_device *phydev)
{
	return -EINVAL;
}

int cs4321_read_status(struct phy_device *phydev)
{
	int value;

	if (is_10g_interface(phydev->interface)) {
		value = cs4321_phy_read(phydev, CS4321_GPIO_GPIO_INTS);
		phydev->speed = SPEED_10000;
		phydev->duplex = DUPLEX_FULL;
		phydev->link = !!(value & 3);
	} else {
		value = cs4321_phy_read(phydev, CS4321_GIGEPCS_LINE_STATUS);
		phydev->speed = SPEED_1000;
		phydev->duplex = DUPLEX_FULL;
		phydev->link = !!(value & 4);
	}
	return value < 0 ? -1 : 0;
}

#ifndef __U_BOOT__
int cs4321_ack_interrupt(struct phy_device *phydev)
{
	return cs4321_phy_write(phydev, CS4321_GPIO_GPIO_INT, 6);
}

int cs4321_config_intr(struct phy_device *phydev)
{
	int ret = 0;
	int int_en;

	int_en = cs4321_phy_read(phydev, CS4321_GPIO_GPIO_INTE);
	if (int_en < 0) {
		ret = int_en;
		goto err;
	}
	if (phydev->interrupts == PHY_INTERRUPT_ENABLED)
		int_en |= 7;
	else
		int_en &= ~7;

	ret = cs4321_phy_write(phydev, CS4321_GPIO_GPIO_INTE, int_en);

err:
	return ret;
}

int cs4321_did_interrupt(struct phy_device *phydev)
{
	int int_bits;

	int_bits = cs4321_phy_read(phydev, CS4321_GPIO_GPIO_INTS);
	if (int_bits < 0)
		goto err;

	return (int_bits & 7) != 0;

err:
	return 1;
}

static struct of_device_id cs4321_match[] = {
	{
	 .compatible = "cortina,cs4321",
	 },
	{},
};

MODULE_DEVICE_TABLE(of, cs4321_match);

static struct phy_driver cs4321_phy_driver = {
	.phy_id = 0xffffffff,
	.phy_id_MASK = 0xffffffff,
	.name = "Cortina CS4321",
	.config_init = cs4321_config_init,
	.probe = cs4321_probe,
	.config_aneg = cs4321_config_aneg,
	.read_status = cs4321_read_status,
	.ack_interrupt = cs4321_ack_interrupt,
	.config_intr = cs4321_config_intr,
	.did_interrupt = cs4321_did_interrupt,
	.driver = {
		   .owner = THIS_MODULE,
		   .of_match_table = cs4321_match,
		   },
};

static int __init cs4321_drv_init(void)
{
	int ret;

	ret = phy_driver_register(&cs4321_phy_driver);

	return ret;
}

module_init(cs4321_drv_init);

static void __exit cs4321_drv_exit(void)
{
	phy_driver_unregister(&cs4321_phy_driver);
}

module_exit(cs4321_drv_exit);
#else /* U-Boot */

struct phy_driver cs4321_driver;

int phy_cortina_init(void)
{
	phy_register(&cs4321_driver);
	return 0;
}

int phy_reset(struct phy_device *phydev)
{
	struct cs4321_private *p = (struct cs4321_private *)phydev->priv;
	if (!p->initialized)
		return 0;

	return cs4321_reset(phydev);
}

int cs4321_config(struct phy_device *phydev)
{
	/* For now assume 10000baseT.  Fill in later */
	if (is_10g_interface(phydev->interface))
		phydev->supported = phydev->advertising =
			 SUPPORTED_10000baseT_Full;
	else
		phydev->supported = phydev->advertising =
			 SUPPORTED_1000baseT_Full |
			 SUPPORTED_Autoneg;
	return 0;
}

int cs4321_startup(struct phy_device *phydev)
{
	int ret;
	phydev->speed = is_10g_interface(phydev->interface) ?
			SPEED_10000 : SPEED_1000;
	phydev->duplex = DUPLEX_FULL;

	debug("%s: Entry\n", __func__);
	ret = cs4321_config_init(phydev);
	if (ret) {
		printf("%s: config init returned %d\n", __func__, ret);
		return ret;
	}
	return cs4321_read_status(phydev);
}

int cs4321_shutdown(struct phy_device *phydev)
{
	/* TODO: shut down the link */
	return 0;
}

int get_phy_id(struct mii_dev *bus, int addr, int devad, uint32_t *phy_id)
{
	int phy_reg;

	phy_reg = bus->read(bus, addr, 0, CS4321_GLOBAL_CHIP_ID_MSB);
	if (phy_reg < 0)
		return -EIO;

	*phy_id = (phy_reg & 0xffff) << 16;

	phy_reg = bus->read(bus, addr, 0, CS4321_GLOBAL_CHIP_ID_LSB);
	if (phy_reg < 0)
		return -EIO;
	*phy_id |= (phy_reg & 0xffff);

	debug("%s: phy_id: 0x%x\n", __func__, *phy_id);

	return 0;
}

struct phy_driver cs4321_driver = {
	.name = "Cortina CS4321",
	.uid = 0x100223e5,
	.mask = 0x0fffffff,	/* Cortina is weird.  chip version is 4 MSBs */
	.features = 0,
	.probe = cs4321_probe,
	.config = cs4321_config,
	.startup = cs4321_startup,
	.shutdown = cs4321_shutdown,
	.reset = NULL
};


#endif

static inline uint16_t cs4321_twos_compliment(uint16_t input)
{
	return ((input & 0x8000) == 0) ? input : 65536 - (input & 0x7fff);
}

/**
 * Programs the internal temperature LUT
 */
static int cs4321_hsif_mon_temp_program_lut(struct phy_device *phydev,
					    uint16_t reclaim_offset)
{
	int i;
	int status;
	static const uint16_t cs4321_lut_mon_temp[] = {
	    0x6059, 0x81FD, /* Index 0 -> DSP_SDS_TEMPMON_MON_LUT_RANGE0,VALUE0 */
	    0x685E, 0x82E2, /* Index 1 -> DSP_SDS_TEMPMON_MON_LUT_RANGE1,VALUE1 */
	    0x6E9A, 0x826F, /* Index 2 */
	    0x74F9, 0x81FC, /* Index 3 */
	    0x7B79, 0x81CD, /* Index 4 */
	    0x8235, 0x819F, /* Index 5 */
	    0x892D, 0x8114, /* Index 6 */
	    0x9026, 0x808A, /* Index 7 */
	    0xFFFF, 0x0000, /* Index 8 */
	    0x0000, 0x0000, /* Index 9 */
	    0x0000, 0x0000, /* Index 10 */
	    0x0000, 0x0000, /* Index 11 */
	    0x0000, 0x0000, /* Index 12 */
	    0x0000, 0x0000, /* Index 13 */
	    0x0000, 0x0000, /* Index 14 */
	    0x0000, 0x0000, /* Index 15 -> DSP_SDS_TEMPMON_MON_LUT_RANGE15,VALUE15*/
	};
	for (i = 0; i < 32; i += 2) {
		uint16_t range = cs4321_lut_mon_temp[i];
		uint16_t value =
			 cs4321_twos_compliment(cs4321_lut_mon_temp[i + 1])
			 + reclaim_offset;
		int index = i / 2;
		cs4321_phy_write(phydev,
				 CS4321_DSP_SDS_TEMPMON_MON_LUT_RANGE0 + index,
				 range);
		cs4321_phy_write(phydev,
				 CS4321_DSP_SDS_TEMPMON_MON_LUT_VALUE0 + index,
				 value);
	}
	return 0;
}

/**
 * Outputs the current PHY temperature in C
 *
 * @param[in] phydev - phy device
 * @param[out] temp - temperature
 *
 * @return 0 for success, -1 on error
 */
static int cs4321_hsif_min_temp_read(struct phy_device *phydev, int *temp)
{
	int status = 0;
	int data;
	int is_reclaimed_part ;
	int use_register_lut = 1;

	/* Read misc config to check for reclaimed part */
	data = cs4321_phy_read(phydev, CS4321_EFUSE_PDF_MISC_CHIP_CONFIG);
	if (data < 0)
		goto err;
	is_reclaimed_part = ((data & 0x8000) == 0x8000);

	/* Power on voltage monitor */
	data = cs4321_phy_read(phydev, CS4321_LINE_SDS_COMMON_SRX0_RX_CONFIG);
	if (data < 0)
		goto err;
	data &= ~0x2000;
	cs4321_phy_write(phydev, CS4321_LINE_SDS_COMMON_SRX0_RX_CONFIG, data);
	udelay(10);

	if (use_register_lut) {
		uint16_t cal_constant = 0;
		if (is_reclaimed_part) {
			data = cs4321_phy_read(phydev,
					       CS4321_EFUSE_PDF_VOL_MON_LUT7);
			if (data < 0)
				goto err;
			cal_constant = (uint16_t)data;
			cs4321_phy_write(phydev,
					 CS4321_DSP_SDS_TEMPMON_MON_CONTROL0,
					 0x323);
		} else {
			cs4321_phy_write(phydev,
					 CS4321_DSP_SDS_TEMPMON_MON_CONTROL0,
					 0x333);
		}
		cs4321_phy_write(phydev,
				 CS4321_DSP_SDS_TEMPMON_MON_CONTROL1, 0x0010);
		cs4321_hsif_mon_temp_program_lut(phydev, cal_constant);
		mdelay(1);
	} else {
		cs4321_phy_write(phydev, CS4321_DSP_SDS_TEMPMON_MON_CONTROL0,
				 0x0033);
		cs4321_phy_write(phydev, CS4321_DSP_SDS_TEMPMON_MON_CONTROL1,
				 0x0010);
	}
	data = cs4321_phy_read(phydev, CS4321_DSP_SDS_TEMPMON_MON_STATUS2);
	if (data < 0)
		goto err;
	/* This is the equivelent of ((data / 256.0) * -1.122) + 210.37 */
	*temp = ((data * -1122) + (256 * 210370)) / 256000;
	return 0;

err:
	return -1;
}

int do_cs4321(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	struct phy_device *phydev;
	int temp;

	if (argc < 2) {
		printf("Too few args, argc=%d\n", argc);
		return CMD_RET_USAGE;
	}

	if (argv[1][0] == 't') {
		if (argc < 3) {
			puts("Ethernet device not specified\n");
			return CMD_RET_USAGE;
		}
		phydev = mdio_phydev_for_ethname(argv[2]);
		if (!phydev) {
			printf("Could not find phy device for %s\n", argv[2]);
			return -1;
		}
		if (cs4321_hsif_min_temp_read(phydev, &temp))
			printf("Error reading temperature\n");
		else
			printf("%s: %d C\n", argv[2], temp);
		return 0;
	}
	printf("Unknown arg %s\n", argv[1]);
	return 1;
}

U_BOOT_CMD(
	cs4321, 3, 1, do_cs4321,
	"Cortina CS43XX PHY system",
	"temp [eth device] - display temperature\n");
