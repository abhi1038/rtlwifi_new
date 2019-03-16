// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2018  Realtek Corporation.
 */

#include "main.h"
#include "fw.h"
#include "tx.h"
#include "rx.h"
#include "phy.h"
#include "rtw8822b.h"
#include "rtw8822b_table.h"
#include "mac.h"
#include "reg.h"
#include "debug.h"

static void rtw8822b_config_trx_mode(struct rtw_dev *rtwdev, u8 tx_path,
				     u8 rx_path, bool is_tx2_path);

static void rtw8822be_efuse_parsing(struct rtw_efuse *efuse,
				    struct rtw8822b_efuse *map)
{
	ether_addr_copy(efuse->addr, map->e.mac_addr);
}

static int rtw8822b_read_efuse(struct rtw_dev *rtwdev, u8 *log_map)
{
	struct rtw_efuse *efuse = &rtwdev->efuse;
	struct rtw8822b_efuse *map;
	int i;

	map = (struct rtw8822b_efuse *)log_map;

	efuse->rfe_option = map->rfe_option;
	efuse->crystal_cap = map->xtal_k;
	efuse->pa_type_2g = map->pa_type;
	efuse->pa_type_5g = map->pa_type;
	efuse->lna_type_2g = map->lna_type_2g[0];
	efuse->lna_type_5g = map->lna_type_5g[0];
	efuse->channel_plan = map->channel_plan;
	efuse->country_code[0] = map->country_code[0];
	efuse->country_code[1] = map->country_code[1];
	efuse->bt_setting = map->rf_bt_setting;
	efuse->regd = map->rf_board_option & 0x7;

	for (i = 0; i < 4; i++)
		efuse->txpwr_idx_table[i] = map->txpwr_idx_table[i];

	switch (rtw_hci_type(rtwdev)) {
	case RTW_HCI_TYPE_PCIE:
		rtw8822be_efuse_parsing(efuse, map);
		break;
	default:
		/* unsupported now */
		return -ENOTSUPP;
	}

	return 0;
}

static void rtw8822b_phy_rfe_init(struct rtw_dev *rtwdev)
{
	/* chip top mux */
	rtw_write32_mask(rtwdev, 0x64, BIT(29) | BIT(28), 0x3);
	rtw_write32_mask(rtwdev, 0x4c, BIT(26) | BIT(25), 0x0);
	rtw_write32_mask(rtwdev, 0x40, BIT(2), 0x1);

	/* from s0 or s1 */
	rtw_write32_mask(rtwdev, 0x1990, 0x3f, 0x30);
	rtw_write32_mask(rtwdev, 0x1990, (BIT(11) | BIT(10)), 0x3);

	/* input or output */
	rtw_write32_mask(rtwdev, 0x974, 0x3f, 0x3f);
	rtw_write32_mask(rtwdev, 0x974, (BIT(11) | BIT(10)), 0x3);
}

static void rtw8822b_phy_set_param(struct rtw_dev *rtwdev)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u8 crystal_cap;
	bool is_tx2_path;

	/* power on BB/RF domain */
	rtw_write8_set(rtwdev, REG_SYS_FUNC_EN,
		       BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
	rtw_write8_set(rtwdev, REG_RF_CTRL,
		       BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
	rtw_write32_set(rtwdev, REG_WLRF1, BIT_WLRF1_BBRF_EN);

	/* pre init before header files config */
	rtw_write32_clr(rtwdev, REG_RXPSEL, BIT_RX_PSEL_RST);

	rtw_phy_load_tables(rtwdev);

	crystal_cap = rtwdev->efuse.crystal_cap & 0x3F;
	rtw_write32_mask(rtwdev, 0x24, 0x7e000000, crystal_cap);
	rtw_write32_mask(rtwdev, 0x28, 0x7e, crystal_cap);

	/* post init after header files config */
	rtw_write32_set(rtwdev, REG_RXPSEL, BIT_RX_PSEL_RST);

	is_tx2_path = false;
	rtw8822b_config_trx_mode(rtwdev, hal->antenna_tx, hal->antenna_rx,
				 is_tx2_path);
	rtw_phy_init(rtwdev);

	rtw8822b_phy_rfe_init(rtwdev);

	/* wifi path controller */
	rtw_write32_mask(rtwdev, 0x70, 0x4000000, 1);
	/* BB control */
	rtw_write32_mask(rtwdev, 0x4c, 0x01800000, 0x2);
	/* antenna mux switch */
	rtw_write8(rtwdev, 0x974, 0xff);
	rtw_write32_mask(rtwdev, 0x1990, 0x300, 0);
	rtw_write32_mask(rtwdev, 0xcbc, 0x80000, 0x0);
	/* SW control */
	rtw_write8(rtwdev, 0xcb4, 0x77);
	/* switch to WL side controller and gnt_wl gnt_bt debug signal */
	rtw_write32_mask(rtwdev, 0x70, 0xff000000, 0x0e);
	/* gnt_wl = 1, gnt_bt = 0 */
	rtw_write32(rtwdev, 0x1704, 0x7700);
	rtw_write32(rtwdev, 0x1700, 0xc00f0038);
	/* switch for WL 2G */
	rtw_write8(rtwdev, 0xcbd, 0x2);
}

#define WLAN_SLOT_TIME		0x09
#define WLAN_PIFS_TIME		0x19
#define WLAN_SIFS_CCK_CONT_TX	0xA
#define WLAN_SIFS_OFDM_CONT_TX	0xE
#define WLAN_SIFS_CCK_TRX	0x10
#define WLAN_SIFS_OFDM_TRX	0x10
#define WLAN_VO_TXOP_LIMIT	0x186 /* unit : 32us */
#define WLAN_VI_TXOP_LIMIT	0x3BC /* unit : 32us */
#define WLAN_RDG_NAV		0x05
#define WLAN_TXOP_NAV		0x1B
#define WLAN_CCK_RX_TSF		0x30
#define WLAN_OFDM_RX_TSF	0x30
#define WLAN_TBTT_PROHIBIT	0x04 /* unit : 32us */
#define WLAN_TBTT_HOLD_TIME	0x064 /* unit : 32us */
#define WLAN_DRV_EARLY_INT	0x04
#define WLAN_BCN_DMA_TIME	0x02

#define WLAN_RX_FILTER0		0x0FFFFFFF
#define WLAN_RX_FILTER2		0xFFFF
#define WLAN_RCR_CFG		0xE400220E
#define WLAN_RXPKT_MAX_SZ	12288
#define WLAN_RXPKT_MAX_SZ_512	(WLAN_RXPKT_MAX_SZ >> 9)

#define WLAN_AMPDU_MAX_TIME		0x70
#define WLAN_RTS_LEN_TH			0xFF
#define WLAN_RTS_TX_TIME_TH		0x08
#define WLAN_MAX_AGG_PKT_LIMIT		0x20
#define WLAN_RTS_MAX_AGG_PKT_LIMIT	0x20
#define FAST_EDCA_VO_TH		0x06
#define FAST_EDCA_VI_TH		0x06
#define FAST_EDCA_BE_TH		0x06
#define FAST_EDCA_BK_TH		0x06
#define WLAN_BAR_RETRY_LIMIT		0x01
#define WLAN_RA_TRY_RATE_AGG_LIMIT	0x08

#define WLAN_TX_FUNC_CFG1		0x30
#define WLAN_TX_FUNC_CFG2		0x30
#define WLAN_MAC_OPT_NORM_FUNC1		0x98
#define WLAN_MAC_OPT_LB_FUNC1		0x80
#define WLAN_MAC_OPT_FUNC2		0x30810041

#define WLAN_SIFS_CFG	(WLAN_SIFS_CCK_CONT_TX | \
			(WLAN_SIFS_OFDM_CONT_TX << BIT_SHIFT_SIFS_OFDM_CTX) | \
			(WLAN_SIFS_CCK_TRX << BIT_SHIFT_SIFS_CCK_TRX) | \
			(WLAN_SIFS_OFDM_TRX << BIT_SHIFT_SIFS_OFDM_TRX))

#define WLAN_TBTT_TIME	(WLAN_TBTT_PROHIBIT |\
			(WLAN_TBTT_HOLD_TIME << BIT_SHIFT_TBTT_HOLD_TIME_AP))

#define WLAN_NAV_CFG		(WLAN_RDG_NAV | (WLAN_TXOP_NAV << 16))
#define WLAN_RX_TSF_CFG		(WLAN_CCK_RX_TSF | (WLAN_OFDM_RX_TSF) << 8)

static int rtw8822b_mac_init(struct rtw_dev *rtwdev)
{
	u32 value32;

	/* protocol configuration */
	rtw_write8_clr(rtwdev, REG_SW_AMPDU_BURST_MODE_CTRL, BIT_PRE_TX_CMD);
	rtw_write8(rtwdev, REG_AMPDU_MAX_TIME_V1, WLAN_AMPDU_MAX_TIME);
	rtw_write8_set(rtwdev, REG_TX_HANG_CTRL, BIT_EN_EOF_V1);
	value32 = WLAN_RTS_LEN_TH | (WLAN_RTS_TX_TIME_TH << 8) |
		  (WLAN_MAX_AGG_PKT_LIMIT << 16) |
		  (WLAN_RTS_MAX_AGG_PKT_LIMIT << 24);
	rtw_write32(rtwdev, REG_PROT_MODE_CTRL, value32);
	rtw_write16(rtwdev, REG_BAR_MODE_CTRL + 2,
		    WLAN_BAR_RETRY_LIMIT | WLAN_RA_TRY_RATE_AGG_LIMIT << 8);
	rtw_write8(rtwdev, REG_FAST_EDCA_VOVI_SETTING, FAST_EDCA_VO_TH);
	rtw_write8(rtwdev, REG_FAST_EDCA_VOVI_SETTING + 2, FAST_EDCA_VI_TH);
	rtw_write8(rtwdev, REG_FAST_EDCA_BEBK_SETTING, FAST_EDCA_BE_TH);
	rtw_write8(rtwdev, REG_FAST_EDCA_BEBK_SETTING + 2, FAST_EDCA_BK_TH);
	/* EDCA configuration */
	rtw_write8_clr(rtwdev, REG_TIMER0_SRC_SEL, BIT_TSFT_SEL_TIMER0);
	rtw_write16(rtwdev, REG_TXPAUSE, 0x0000);
	rtw_write8(rtwdev, REG_SLOT, WLAN_SLOT_TIME);
	rtw_write8(rtwdev, REG_PIFS, WLAN_PIFS_TIME);
	rtw_write32(rtwdev, REG_SIFS, WLAN_SIFS_CFG);
	rtw_write16(rtwdev, REG_EDCA_VO_PARAM + 2, WLAN_VO_TXOP_LIMIT);
	rtw_write16(rtwdev, REG_EDCA_VI_PARAM + 2, WLAN_VI_TXOP_LIMIT);
	rtw_write32(rtwdev, REG_RD_NAV_NXT, WLAN_NAV_CFG);
	rtw_write16(rtwdev, REG_RXTSF_OFFSET_CCK, WLAN_RX_TSF_CFG);
	/* Set beacon cotnrol - enable TSF and other related functions */
	rtw_write8_set(rtwdev, REG_BCN_CTRL, BIT_EN_BCN_FUNCTION);
	/* Set send beacon related registers */
	rtw_write32(rtwdev, REG_TBTT_PROHIBIT, WLAN_TBTT_TIME);
	rtw_write8(rtwdev, REG_DRVERLYINT, WLAN_DRV_EARLY_INT);
	rtw_write8(rtwdev, REG_BCNDMATIM, WLAN_BCN_DMA_TIME);
	rtw_write8_clr(rtwdev, REG_TX_PTCL_CTRL + 1, BIT_SIFS_BK_EN >> 8);
	/* WMAC configuration */
	rtw_write32(rtwdev, REG_RXFLTMAP0, WLAN_RX_FILTER0);
	rtw_write16(rtwdev, REG_RXFLTMAP2, WLAN_RX_FILTER2);
	rtw_write32(rtwdev, REG_RCR, WLAN_RCR_CFG);
	rtw_write8(rtwdev, REG_RX_PKT_LIMIT, WLAN_RXPKT_MAX_SZ_512);
	rtw_write8(rtwdev, REG_TCR + 2, WLAN_TX_FUNC_CFG2);
	rtw_write8(rtwdev, REG_TCR + 1, WLAN_TX_FUNC_CFG1);
	rtw_write32(rtwdev, REG_WMAC_OPTION_FUNCTION + 8, WLAN_MAC_OPT_FUNC2);
	rtw_write8(rtwdev, REG_WMAC_OPTION_FUNCTION + 4, WLAN_MAC_OPT_NORM_FUNC1);

	return 0;
}

static void rtw8822b_set_channel_rfe_efem(struct rtw_dev *rtwdev, u8 channel)
{
	struct rtw_hal *hal = &rtwdev->hal;
	bool is_channel_2g = (channel <= 14) ? true : false;

	if (is_channel_2g) {
		rtw_write32s_mask(rtwdev, REG_RFESEL0, 0xffffff, 0x705770);
		rtw_write32s_mask(rtwdev, REG_RFESEL8, MASKBYTE1, 0x57);
		rtw_write32s_mask(rtwdev, REG_RFECTL, BIT(4), 0);
	} else {
		rtw_write32s_mask(rtwdev, REG_RFESEL0, 0xffffff, 0x177517);
		rtw_write32s_mask(rtwdev, REG_RFESEL8, MASKBYTE1, 0x75);
		rtw_write32s_mask(rtwdev, REG_RFECTL, BIT(5), 0);
	}

	rtw_write32s_mask(rtwdev, REG_RFEINV, BIT(11) | BIT(10) | 0x3f, 0x0);

	if (hal->antenna_rx == BB_PATH_AB ||
	    hal->antenna_tx == BB_PATH_AB) {
		/* 2TX or 2RX */
		rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa501);
	} else if (hal->antenna_rx == hal->antenna_tx) {
		/* TXA+RXA or TXB+RXB */
		rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa500);
	} else {
		/* TXB+RXA or TXA+RXB */
		rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa005);
	}
}

static void rtw8822b_set_channel_rfe_ifem(struct rtw_dev *rtwdev, u8 channel)
{
	struct rtw_hal *hal = &rtwdev->hal;
	bool is_channel_2g = (channel <= 14) ? true : false;

	if (is_channel_2g) {
		/* signal source */
		rtw_write32s_mask(rtwdev, REG_RFESEL0, 0xffffff, 0x745774);
		rtw_write32s_mask(rtwdev, REG_RFESEL8, MASKBYTE1, 0x57);
	} else {
		/* signal source */
		rtw_write32s_mask(rtwdev, REG_RFESEL0, 0xffffff, 0x477547);
		rtw_write32s_mask(rtwdev, REG_RFESEL8, MASKBYTE1, 0x75);
	}

	rtw_write32s_mask(rtwdev, REG_RFEINV, BIT(11) | BIT(10) | 0x3f, 0x0);

	if (is_channel_2g) {
		if (hal->antenna_rx == BB_PATH_AB ||
		    hal->antenna_tx == BB_PATH_AB) {
			/* 2TX or 2RX */
			rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa501);
		} else if (hal->antenna_rx == hal->antenna_tx) {
			/* TXA+RXA or TXB+RXB */
			rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa500);
		} else {
			/* TXB+RXA or TXA+RXB */
			rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa005);
		}
	} else {
		rtw_write32s_mask(rtwdev, REG_TRSW, MASKLWORD, 0xa5a5);
	}
}

enum {
	CCUT_IDX_1R_2G,
	CCUT_IDX_2R_2G,
	CCUT_IDX_1R_5G,
	CCUT_IDX_2R_5G,
	CCUT_IDX_NR,
};

struct cca_ccut {
	u32 reg82c[CCUT_IDX_NR];
	u32 reg830[CCUT_IDX_NR];
	u32 reg838[CCUT_IDX_NR];
};

static const struct cca_ccut cca_ifem_ccut = {
	{0x75C97010, 0x75C97010, 0x75C97010, 0x75C97010}, /*Reg82C*/
	{0x79a0eaaa, 0x79A0EAAC, 0x79a0eaaa, 0x79a0eaaa}, /*Reg830*/
	{0x87765541, 0x87746341, 0x87765541, 0x87746341}, /*Reg838*/
};

static const struct cca_ccut cca_efem_ccut = {
	{0x75B86010, 0x75B76010, 0x75B86010, 0x75B76010}, /*Reg82C*/
	{0x79A0EAA8, 0x79A0EAAC, 0x79A0EAA8, 0x79a0eaaa}, /*Reg830*/
	{0x87766451, 0x87766431, 0x87766451, 0x87766431}, /*Reg838*/
};

static const struct cca_ccut cca_ifem_ccut_ext = {
	{0x75da8010, 0x75da8010, 0x75da8010, 0x75da8010}, /*Reg82C*/
	{0x79a0eaaa, 0x97A0EAAC, 0x79a0eaaa, 0x79a0eaaa}, /*Reg830*/
	{0x87765541, 0x86666341, 0x87765561, 0x86666361}, /*Reg838*/
};

static void rtw8822b_get_cca_val(const struct cca_ccut *cca_ccut, u8 col,
				 u32 *reg82c, u32 *reg830, u32 *reg838)
{
	*reg82c = cca_ccut->reg82c[col];
	*reg830 = cca_ccut->reg830[col];
	*reg838 = cca_ccut->reg838[col];
}

struct rtw8822b_rfe_info {
	const struct cca_ccut *cca_ccut_2g;
	const struct cca_ccut *cca_ccut_5g;
	enum rtw_rfe_fem fem;
	bool ifem_ext;
	void (*rtw_set_channel_rfe)(struct rtw_dev *rtwdev, u8 channel);
};

#define I2GE5G_CCUT(set_ch) {						\
	.cca_ccut_2g = &cca_ifem_ccut,					\
	.cca_ccut_5g = &cca_efem_ccut,					\
	.fem = RTW_RFE_IFEM2G_EFEM5G,					\
	.ifem_ext = false,						\
	.rtw_set_channel_rfe = &rtw8822b_set_channel_rfe_ ## set_ch,	\
	}
#define IFEM_EXT_CCUT(set_ch) {						\
	.cca_ccut_2g = &cca_ifem_ccut_ext,				\
	.cca_ccut_5g = &cca_ifem_ccut_ext,				\
	.fem = RTW_RFE_IFEM,						\
	.ifem_ext = true,						\
	.rtw_set_channel_rfe = &rtw8822b_set_channel_rfe_ ## set_ch,	\
	}

static const struct rtw8822b_rfe_info rtw8822b_rfe_info[] = {
	[2] = I2GE5G_CCUT(efem),
	[5] = IFEM_EXT_CCUT(ifem),
};

static void rtw8822b_set_channel_cca(struct rtw_dev *rtwdev, u8 channel, u8 bw,
				     const struct rtw8822b_rfe_info *rfe_info)
{
	struct rtw_hal *hal = &rtwdev->hal;
	struct rtw_efuse *efuse = &rtwdev->efuse;
	const struct cca_ccut *cca_ccut;
	u8 col;
	u32 reg82c, reg830, reg838;
	bool is_efem_cca = false, is_ifem_cca = false, is_rfe_type = false;

	if (channel <= 14) {
		cca_ccut = rfe_info->cca_ccut_2g;

		if (hal->antenna_rx == BB_PATH_A ||
		    hal->antenna_rx == BB_PATH_B)
			col = CCUT_IDX_1R_2G;
		else
			col = CCUT_IDX_2R_2G;
	} else {
		cca_ccut = rfe_info->cca_ccut_5g;

		if (hal->antenna_rx == BB_PATH_A ||
		    hal->antenna_rx == BB_PATH_B)
			col = CCUT_IDX_1R_5G;
		else
			col = CCUT_IDX_2R_5G;
	}

	rtw8822b_get_cca_val(cca_ccut, col, &reg82c, &reg830, &reg838);

	switch (rfe_info->fem) {
	case RTW_RFE_IFEM:
	default:
		is_ifem_cca = true;
		if (rfe_info->ifem_ext)
			is_rfe_type = true;
		break;
	case RTW_RFE_EFEM:
		is_efem_cca = true;
		break;
	case RTW_RFE_IFEM2G_EFEM5G:
		if (channel <= 14)
			is_ifem_cca = true;
		else
			is_efem_cca = true;
		break;
	}

	if (is_ifem_cca) {
		if ((hal->cut_version == RTW_CHIP_VER_CUT_B &&
		     (col == CCUT_IDX_2R_2G || col == CCUT_IDX_2R_5G) &&
		     bw == RTW_CHANNEL_WIDTH_40) ||
		    (!is_rfe_type && col == CCUT_IDX_2R_5G &&
		     bw == RTW_CHANNEL_WIDTH_40) ||
		    (efuse->rfe_option == 5 && col == CCUT_IDX_2R_5G))
			reg830 = 0x79a0ea28;
	}

	rtw_write32_mask(rtwdev, REG_CCASEL, MASKDWORD, reg82c);
	rtw_write32_mask(rtwdev, REG_PDMFTH, MASKDWORD, reg830);
	rtw_write32_mask(rtwdev, REG_CCA2ND, MASKDWORD, reg838);

	if (is_efem_cca && !(hal->cut_version == RTW_CHIP_VER_CUT_B))
		rtw_write32_mask(rtwdev, REG_L1WT, MASKDWORD, 0x9194b2b9);

	if (bw == RTW_CHANNEL_WIDTH_20 &&
	    ((channel >= 52 && channel <= 64) ||
	     (channel >= 100 && channel <= 144)))
		rtw_write32_mask(rtwdev, REG_CCA2ND, 0xf0, 0x4);
}

static const u8 low_band[15] = {0x7, 0x6, 0x6, 0x5, 0x0, 0x0, 0x7, 0xff, 0x6,
				0x5, 0x0, 0x0, 0x7, 0x6, 0x6};
static const u8 middle_band[23] = {0x6, 0x5, 0x0, 0x0, 0x7, 0x6, 0x6, 0xff, 0x0,
				   0x0, 0x7, 0x6, 0x6, 0x5, 0x0, 0xff, 0x7, 0x6,
				   0x6, 0x5, 0x0, 0x0, 0x7};
static const u8 high_band[15] = {0x5, 0x5, 0x0, 0x7, 0x7, 0x6, 0x5, 0xff, 0x0,
				 0x7, 0x7, 0x6, 0x5, 0x5, 0x0};

static void rtw8822b_set_channel_rf(struct rtw_dev *rtwdev, u8 channel, u8 bw)
{
#define RF18_BAND_MASK		(BIT(16) | BIT(9) | BIT(8))
#define RF18_BAND_2G		(0)
#define RF18_BAND_5G		(BIT(16) | BIT(8))
#define RF18_CHANNEL_MASK	(MASKBYTE0)
#define RF18_RFSI_MASK		(BIT(18) | BIT(17))
#define RF18_RFSI_GE_CH80	(BIT(17))
#define RF18_RFSI_GT_CH144	(BIT(18))
#define RF18_BW_MASK		(BIT(11) | BIT(10))
#define RF18_BW_20M		(BIT(11) | BIT(10))
#define RF18_BW_40M		(BIT(11))
#define RF18_BW_80M		(BIT(10))
#define RFBE_MASK		(BIT(17) | BIT(16) | BIT(15))

	struct rtw_hal *hal = &rtwdev->hal;
	u32 rf_reg18, rf_reg_be;

	rf_reg18 = rtw_read_rf(rtwdev, RF_PATH_A, 0x18, RFREG_MASK);

	rf_reg18 &= ~(RF18_BAND_MASK | RF18_CHANNEL_MASK | RF18_RFSI_MASK |
		      RF18_BW_MASK);

	rf_reg18 |= (channel <= 14 ? RF18_BAND_2G : RF18_BAND_5G);
	rf_reg18 |= (channel & RF18_CHANNEL_MASK);
	if (channel > 144)
		rf_reg18 |= RF18_RFSI_GT_CH144;
	else if (channel >= 80)
		rf_reg18 |= RF18_RFSI_GE_CH80;

	switch (bw) {
	case RTW_CHANNEL_WIDTH_5:
	case RTW_CHANNEL_WIDTH_10:
	case RTW_CHANNEL_WIDTH_20:
	default:
		rf_reg18 |= RF18_BW_20M;
		break;
	case RTW_CHANNEL_WIDTH_40:
		rf_reg18 |= RF18_BW_40M;
		break;
	case RTW_CHANNEL_WIDTH_80:
		rf_reg18 |= RF18_BW_80M;
		break;
	}

	if (channel <= 14)
		rf_reg_be = 0x0;
	else if (channel >= 36 && channel <= 64)
		rf_reg_be = low_band[(channel - 36) >> 1];
	else if (channel >= 100 && channel <= 144)
		rf_reg_be = middle_band[(channel - 100) >> 1];
	else if (channel >= 149 && channel <= 177)
		rf_reg_be = high_band[(channel - 149) >> 1];
	else
		goto err;

	rtw_write_rf(rtwdev, RF_PATH_A, RF_MALSEL, RFBE_MASK, rf_reg_be);

	/* need to set 0xdf[18]=1 before writing RF18 when channel 144 */
	if (channel == 144)
		rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTDBG, BIT(18), 0x1);
	else
		rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTDBG, BIT(18), 0x0);

	rtw_write_rf(rtwdev, RF_PATH_A, 0x18, RFREG_MASK, rf_reg18);
	if (hal->rf_type > RF_1T1R)
		rtw_write_rf(rtwdev, RF_PATH_B, 0x18, RFREG_MASK, rf_reg18);

	rtw_write_rf(rtwdev, RF_PATH_A, RF_XTALX2, BIT(19), 0);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_XTALX2, BIT(19), 1);

	return;

err:
	WARN_ON(1);
}

static void rtw8822b_toggle_igi(struct rtw_dev *rtwdev)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u32 igi;

	igi = rtw_read32_mask(rtwdev, REG_RXIGI_A, 0x7f);
	rtw_write32_mask(rtwdev, REG_RXIGI_A, 0x7f, igi - 2);
	rtw_write32_mask(rtwdev, REG_RXIGI_A, 0x7f, igi);
	rtw_write32_mask(rtwdev, REG_RXIGI_B, 0x7f, igi - 2);
	rtw_write32_mask(rtwdev, REG_RXIGI_B, 0x7f, igi);

	rtw_write32_mask(rtwdev, REG_RXPSEL, MASKBYTE0, 0x0);
	rtw_write32_mask(rtwdev, REG_RXPSEL, MASKBYTE0,
			 hal->antenna_rx | (hal->antenna_rx << 4));
}

static void rtw8822b_set_channel_rxdfir(struct rtw_dev *rtwdev, u8 bw)
{
	if (bw == RTW_CHANNEL_WIDTH_40) {
		/* RX DFIR for BW40 */
		rtw_write32_mask(rtwdev, REG_ACBB0, BIT(29) | BIT(28), 0x1);
		rtw_write32_mask(rtwdev, REG_ACBBRXFIR, BIT(29) | BIT(28), 0x0);
		rtw_write32s_mask(rtwdev, REG_TXDFIR, BIT(31), 0x0);
	} else if (bw == RTW_CHANNEL_WIDTH_80) {
		/* RX DFIR for BW80 */
		rtw_write32_mask(rtwdev, REG_ACBB0, BIT(29) | BIT(28), 0x2);
		rtw_write32_mask(rtwdev, REG_ACBBRXFIR, BIT(29) | BIT(28), 0x1);
		rtw_write32s_mask(rtwdev, REG_TXDFIR, BIT(31), 0x0);
	} else {
		/* RX DFIR for BW20, BW10 and BW5*/
		rtw_write32_mask(rtwdev, REG_ACBB0, BIT(29) | BIT(28), 0x2);
		rtw_write32_mask(rtwdev, REG_ACBBRXFIR, BIT(29) | BIT(28), 0x2);
		rtw_write32s_mask(rtwdev, REG_TXDFIR, BIT(31), 0x1);
	}
}

static void rtw8822b_set_channel_bb(struct rtw_dev *rtwdev, u8 channel, u8 bw,
				    u8 primary_ch_idx)
{
	struct rtw_efuse *efuse = &rtwdev->efuse;
	u8 rfe_option = efuse->rfe_option;
	u32 val32;

	if (channel <= 14) {
		rtw_write32_mask(rtwdev, REG_RXPSEL, BIT(28), 0x1);
		rtw_write32_mask(rtwdev, REG_CCK_CHECK, BIT(7), 0x0);
		rtw_write32_mask(rtwdev, REG_ENTXCCK, BIT(18), 0x0);
		rtw_write32_mask(rtwdev, REG_RXCCAMSK, 0x0000FC00, 15);

		rtw_write32_mask(rtwdev, REG_ACGG2TBL, 0x1f, 0x0);
		rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x96a);
		if (channel == 14) {
			rtw_write32_mask(rtwdev, REG_TXSF2, MASKDWORD, 0x00006577);
			rtw_write32_mask(rtwdev, REG_TXSF6, MASKLWORD, 0x0000);
		} else {
			rtw_write32_mask(rtwdev, REG_TXSF2, MASKDWORD, 0x384f6577);
			rtw_write32_mask(rtwdev, REG_TXSF6, MASKLWORD, 0x1525);
		}

		rtw_write32_mask(rtwdev, REG_RFEINV, 0x300, 0x2);
	} else if (channel > 35) {
		rtw_write32_mask(rtwdev, REG_ENTXCCK, BIT(18), 0x1);
		rtw_write32_mask(rtwdev, REG_CCK_CHECK, BIT(7), 0x1);
		rtw_write32_mask(rtwdev, REG_RXPSEL, BIT(28), 0x0);
		rtw_write32_mask(rtwdev, REG_RXCCAMSK, 0x0000FC00, 34);

		if (channel >= 36 && channel <= 64)
			rtw_write32_mask(rtwdev, REG_ACGG2TBL, 0x1f, 0x1);
		else if (channel >= 100 && channel <= 144)
			rtw_write32_mask(rtwdev, REG_ACGG2TBL, 0x1f, 0x2);
		else if (channel >= 149)
			rtw_write32_mask(rtwdev, REG_ACGG2TBL, 0x1f, 0x3);

		if (channel >= 36 && channel <= 48)
			rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x494);
		else if (channel >= 52 && channel <= 64)
			rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x453);
		else if (channel >= 100 && channel <= 116)
			rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x452);
		else if (channel >= 118 && channel <= 177)
			rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x412);

		rtw_write32_mask(rtwdev, 0xcbc, 0x300, 0x1);
	}

	switch (bw) {
	case RTW_CHANNEL_WIDTH_20:
	default:
		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xFFCFFC00;
		val32 |= (RTW_CHANNEL_WIDTH_20);
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x1);
		break;
	case RTW_CHANNEL_WIDTH_40:
		if (primary_ch_idx == 1)
			rtw_write32_set(rtwdev, REG_RXSB, BIT(4));
		else
			rtw_write32_clr(rtwdev, REG_RXSB, BIT(4));

		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xFF3FF300;
		val32 |= (((primary_ch_idx & 0xf) << 2) | RTW_CHANNEL_WIDTH_40);
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x1);
		break;
	case RTW_CHANNEL_WIDTH_80:
		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xFCEFCF00;
		val32 |= (((primary_ch_idx & 0xf) << 2) | RTW_CHANNEL_WIDTH_80);
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x1);

		if (rfe_option == 2) {
			rtw_write32_mask(rtwdev, REG_L1PKWT, 0x0000f000, 0x6);
			rtw_write32_mask(rtwdev, REG_ADC40, BIT(10), 0x1);
		}
		break;
	case RTW_CHANNEL_WIDTH_5:
		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xEFEEFE00;
		val32 |= ((BIT(6) | RTW_CHANNEL_WIDTH_20));
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x0);
		rtw_write32_mask(rtwdev, REG_ADC40, BIT(31), 0x1);
		break;
	case RTW_CHANNEL_WIDTH_10:
		val32 = rtw_read32_mask(rtwdev, REG_ADCCLK, MASKDWORD);
		val32 &= 0xEFFEFF00;
		val32 |= ((BIT(7) | RTW_CHANNEL_WIDTH_20));
		rtw_write32_mask(rtwdev, REG_ADCCLK, MASKDWORD, val32);

		rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x0);
		rtw_write32_mask(rtwdev, REG_ADC40, BIT(31), 0x1);
		break;
	}
}

static void rtw8822b_set_channel(struct rtw_dev *rtwdev, u8 channel, u8 bw,
				 u8 primary_chan_idx)
{
	struct rtw_efuse *efuse = &rtwdev->efuse;
	const struct rtw8822b_rfe_info *rfe_info;

	if (WARN(efuse->rfe_option >= ARRAY_SIZE(rtw8822b_rfe_info),
		 "rfe_option %d is out of boundary\n", efuse->rfe_option))
		return;

	rfe_info = &rtw8822b_rfe_info[efuse->rfe_option];

	rtw8822b_set_channel_bb(rtwdev, channel, bw, primary_chan_idx);
	rtw_set_channel_mac(rtwdev, channel, bw, primary_chan_idx);
	rtw8822b_set_channel_rf(rtwdev, channel, bw);
	rtw8822b_set_channel_rxdfir(rtwdev, bw);
	rtw8822b_toggle_igi(rtwdev);
	rtw8822b_set_channel_cca(rtwdev, channel, bw, rfe_info);
	(*rfe_info->rtw_set_channel_rfe)(rtwdev, channel);
}

static void rtw8822b_config_trx_mode(struct rtw_dev *rtwdev, u8 tx_path,
				     u8 rx_path, bool is_tx2_path)
{
	struct rtw_efuse *efuse = &rtwdev->efuse;
	const struct rtw8822b_rfe_info *rfe_info;
	u8 ch = rtwdev->hal.current_channel;
	u8 tx_path_sel, rx_path_sel;
	int counter;

	if (WARN(efuse->rfe_option >= ARRAY_SIZE(rtw8822b_rfe_info),
		 "rfe_option %d is out of boundary\n", efuse->rfe_option))
		return;

	rfe_info = &rtw8822b_rfe_info[efuse->rfe_option];

	if ((tx_path | rx_path) & BB_PATH_A)
		rtw_write32_mask(rtwdev, REG_AGCTR_A, MASKLWORD, 0x3231);
	else
		rtw_write32_mask(rtwdev, REG_AGCTR_A, MASKLWORD, 0x1111);

	if ((tx_path | rx_path) & BB_PATH_B)
		rtw_write32_mask(rtwdev, REG_AGCTR_B, MASKLWORD, 0x3231);
	else
		rtw_write32_mask(rtwdev, REG_AGCTR_B, MASKLWORD, 0x1111);

	rtw_write32_mask(rtwdev, REG_CDDTXP, (BIT(19) | BIT(18)), 0x3);
	rtw_write32_mask(rtwdev, REG_TXPSEL, (BIT(29) | BIT(28)), 0x1);
	rtw_write32_mask(rtwdev, REG_TXPSEL, BIT(30), 0x1);

	if (tx_path & BB_PATH_A) {
		rtw_write32_mask(rtwdev, REG_CDDTXP, 0xfff00000, 0x001);
		rtw_write32_mask(rtwdev, REG_ADCINI, 0xf0000000, 0x8);
	} else if (tx_path & BB_PATH_B) {
		rtw_write32_mask(rtwdev, REG_CDDTXP, 0xfff00000, 0x002);
		rtw_write32_mask(rtwdev, REG_ADCINI, 0xf0000000, 0x4);
	}

	if (tx_path == BB_PATH_A || tx_path == BB_PATH_B)
		rtw_write32_mask(rtwdev, REG_TXPSEL1, 0xfff0, 0x01);
	else
		rtw_write32_mask(rtwdev, REG_TXPSEL1, 0xfff0, 0x43);

	tx_path_sel = (tx_path << 4) | tx_path;
	rtw_write32_mask(rtwdev, REG_TXPSEL, MASKBYTE0, tx_path_sel);

	if (tx_path != BB_PATH_A && tx_path != BB_PATH_B) {
		if (is_tx2_path || rtwdev->mp_mode) {
			rtw_write32_mask(rtwdev, REG_CDDTXP, 0xfff00000, 0x043);
			rtw_write32_mask(rtwdev, REG_ADCINI, 0xf0000000, 0xc);
		}
	}

	rtw_write32_mask(rtwdev, REG_RXDESC, BIT(22), 0x0);
	rtw_write32_mask(rtwdev, REG_RXDESC, BIT(18), 0x0);

	if (rx_path & BB_PATH_A)
		rtw_write32_mask(rtwdev, REG_ADCINI, 0x0f000000, 0x0);
	else if (rx_path & BB_PATH_B)
		rtw_write32_mask(rtwdev, REG_ADCINI, 0x0f000000, 0x5);

	rx_path_sel = (rx_path << 4) | rx_path;
	rtw_write32_mask(rtwdev, REG_RXPSEL, MASKBYTE0, rx_path_sel);

	if (rx_path == BB_PATH_A || rx_path == BB_PATH_B) {
		rtw_write32_mask(rtwdev, REG_ANTWT, BIT(16), 0x0);
		rtw_write32_mask(rtwdev, REG_HTSTFWT, BIT(28), 0x0);
		rtw_write32_mask(rtwdev, REG_MRC, BIT(23), 0x0);
	} else {
		rtw_write32_mask(rtwdev, REG_ANTWT, BIT(16), 0x1);
		rtw_write32_mask(rtwdev, REG_HTSTFWT, BIT(28), 0x1);
		rtw_write32_mask(rtwdev, REG_MRC, BIT(23), 0x1);
	}

	for (counter = 100; counter > 0; counter--) {
		u32 rf_reg33;

		rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWE, RFREG_MASK, 0x80000);
		rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWA, RFREG_MASK, 0x00001);

		udelay(2);
		rf_reg33 = rtw_read_rf(rtwdev, RF_PATH_A, 0x33, RFREG_MASK);

		if (rf_reg33 == 0x00001)
			break;
	}

	if (WARN(counter <= 0, "write RF mode table fail\n"))
		return;

	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWE, RFREG_MASK, 0x80000);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWA, RFREG_MASK, 0x00001);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWD1, RFREG_MASK, 0x00034);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWD0, RFREG_MASK, 0x4080c);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWE, RFREG_MASK, 0x00000);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWE, RFREG_MASK, 0x00000);

	rtw8822b_toggle_igi(rtwdev);
	rtw8822b_set_channel_cca(rtwdev, 1, RTW_CHANNEL_WIDTH_20, rfe_info);
	(*rfe_info->rtw_set_channel_rfe)(rtwdev, ch);
}

static void query_phy_status_page0(struct rtw_dev *rtwdev, u8 *phy_status,
				   struct rtw_rx_pkt_stat *pkt_stat)
{
	s8 min_rx_power = -120;
	u8 pwdb = GET_PHY_STAT_P0_PWDB(phy_status);

	pkt_stat->rx_power[RF_PATH_A] = pwdb - 110;
	pkt_stat->rssi = rtw_phy_rf_power_2_rssi(pkt_stat->rx_power, 1);
	pkt_stat->bw = RTW_CHANNEL_WIDTH_20;
	pkt_stat->signal_power = max(pkt_stat->rx_power[RF_PATH_A],
				     min_rx_power);
}

static void query_phy_status_page1(struct rtw_dev *rtwdev, u8 *phy_status,
				   struct rtw_rx_pkt_stat *pkt_stat)
{
	u8 rxsc, bw;
	s8 min_rx_power = -120;

	if (pkt_stat->rate > DESC_RATE11M && pkt_stat->rate < DESC_RATEMCS0)
		rxsc = GET_PHY_STAT_P1_L_RXSC(phy_status);
	else
		rxsc = GET_PHY_STAT_P1_HT_RXSC(phy_status);

	if (rxsc >= 1 && rxsc <= 8)
		bw = RTW_CHANNEL_WIDTH_20;
	else if (rxsc >= 9 && rxsc <= 12)
		bw = RTW_CHANNEL_WIDTH_40;
	else if (rxsc >= 13)
		bw = RTW_CHANNEL_WIDTH_80;
	else
		bw = GET_PHY_STAT_P1_RF_MODE(phy_status);

	pkt_stat->rx_power[RF_PATH_A] = GET_PHY_STAT_P1_PWDB_A(phy_status) - 110;
	pkt_stat->rx_power[RF_PATH_B] = GET_PHY_STAT_P1_PWDB_B(phy_status) - 110;
	pkt_stat->rssi = rtw_phy_rf_power_2_rssi(pkt_stat->rx_power, 2);
	pkt_stat->bw = bw;
	pkt_stat->signal_power = max3(pkt_stat->rx_power[RF_PATH_A],
				      pkt_stat->rx_power[RF_PATH_B],
				      min_rx_power);
}

static void query_phy_status(struct rtw_dev *rtwdev, u8 *phy_status,
			     struct rtw_rx_pkt_stat *pkt_stat)
{
	u8 page;

	page = *phy_status & 0xf;

	switch (page) {
	case 0:
		query_phy_status_page0(rtwdev, phy_status, pkt_stat);
		break;
	case 1:
		query_phy_status_page1(rtwdev, phy_status, pkt_stat);
		break;
	default:
		rtw_warn(rtwdev, "unused phy status page (%d)\n", page);
		return;
	}
}

static void rtw8822b_query_rx_desc(struct rtw_dev *rtwdev, u8 *rx_desc,
				   struct rtw_rx_pkt_stat *pkt_stat,
				   struct ieee80211_rx_status *rx_status)
{
	struct ieee80211_hdr *hdr;
	u32 desc_sz = rtwdev->chip->rx_pkt_desc_sz;
	u8 *phy_status = NULL;

	memset(pkt_stat, 0, sizeof(*pkt_stat));

	pkt_stat->phy_status = GET_RX_DESC_PHYST(rx_desc);
	pkt_stat->icv_err = GET_RX_DESC_ICV_ERR(rx_desc);
	pkt_stat->crc_err = GET_RX_DESC_CRC32(rx_desc);
	pkt_stat->decrypted = !GET_RX_DESC_SWDEC(rx_desc);
	pkt_stat->is_c2h = GET_RX_DESC_C2H(rx_desc);
	pkt_stat->pkt_len = GET_RX_DESC_PKT_LEN(rx_desc);
	pkt_stat->drv_info_sz = GET_RX_DESC_DRV_INFO_SIZE(rx_desc);
	pkt_stat->shift = GET_RX_DESC_SHIFT(rx_desc);
	pkt_stat->rate = GET_RX_DESC_RX_RATE(rx_desc);
	pkt_stat->cam_id = GET_RX_DESC_MACID(rx_desc);
	pkt_stat->ppdu_cnt = GET_RX_DESC_PPDU_CNT(rx_desc);
	pkt_stat->tsf_low = GET_RX_DESC_TSFL(rx_desc);

	/* drv_info_sz is in unit of 8-bytes */
	pkt_stat->drv_info_sz *= 8;

	/* c2h cmd pkt's rx/phy status is not interested */
	if (pkt_stat->is_c2h)
		return;

	hdr = (struct ieee80211_hdr *)(rx_desc + desc_sz + pkt_stat->shift +
				       pkt_stat->drv_info_sz);
	if (pkt_stat->phy_status) {
		phy_status = rx_desc + desc_sz + pkt_stat->shift;
		query_phy_status(rtwdev, phy_status, pkt_stat);
	}

	rtw_rx_fill_rx_status(rtwdev, pkt_stat, hdr, rx_status, phy_status);
}

static void
rtw8822b_set_tx_power_index_by_rate(struct rtw_dev *rtwdev, u8 path, u8 rs)
{
	struct rtw_hal *hal = &rtwdev->hal;
	static const u32 offset_txagc[2] = {0x1d00, 0x1d80};
	static u32 phy_pwr_idx;
	u8 rate, rate_idx, pwr_index, shift;
	int j;

	for (j = 0; j < rtw_rate_size[rs]; j++) {
		rate = rtw_rate_section[rs][j];
		pwr_index = hal->tx_pwr_tbl[path][rate];
		shift = rate & 0x3;
		phy_pwr_idx |= ((u32)pwr_index << (shift * 8));
		if (shift == 0x3) {
			rate_idx = rate & 0xfc;
			rtw_write32(rtwdev, offset_txagc[path] + rate_idx,
				    phy_pwr_idx);
			phy_pwr_idx = 0;
		}
	}
}

static void rtw8822b_set_tx_power_index(struct rtw_dev *rtwdev)
{
	struct rtw_hal *hal = &rtwdev->hal;
	int rs, path;

	for (path = 0; path < hal->rf_path_num; path++) {
		for (rs = 0; rs < RTW_RATE_SECTION_MAX; rs++)
			rtw8822b_set_tx_power_index_by_rate(rtwdev, path, rs);
	}
}

static bool rtw8822b_check_rf_path(u8 antenna)
{
	switch (antenna) {
	case BB_PATH_A:
	case BB_PATH_B:
	case BB_PATH_AB:
		return true;
	default:
		return false;
	}
}

static void rtw8822b_set_antenna(struct rtw_dev *rtwdev, u8 antenna_tx,
				 u8 antenna_rx)
{
	struct rtw_hal *hal = &rtwdev->hal;

	rtw_dbg(rtwdev, RTW_DBG_PHY, "config RF path, tx=0x%x rx=0x%x\n",
		antenna_tx, antenna_rx);

	if (!rtw8822b_check_rf_path(antenna_tx)) {
		rtw_info(rtwdev, "unsupport tx path, set to default path ab\n");
		antenna_tx = BB_PATH_AB;
	}
	if (!rtw8822b_check_rf_path(antenna_rx)) {
		rtw_info(rtwdev, "unsupport rx path, set to default path ab\n");
		antenna_rx = BB_PATH_AB;
	}
	hal->antenna_tx = antenna_tx;
	hal->antenna_rx = antenna_rx;
	rtw8822b_config_trx_mode(rtwdev, antenna_tx, antenna_rx, false);
}

static void rtw8822b_cfg_ldo25(struct rtw_dev *rtwdev, bool enable)
{
	u8 ldo_pwr;

	ldo_pwr = rtw_read8(rtwdev, REG_LDO_EFUSE_CTRL + 3);
	ldo_pwr = enable ? ldo_pwr | BIT(7) : ldo_pwr & ~BIT(7);
	rtw_write8(rtwdev, REG_LDO_EFUSE_CTRL + 3, ldo_pwr);
}

static void rtw8822b_false_alarm_statistics(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u32 cck_enable;
	u32 cck_fa_cnt;
	u32 ofdm_fa_cnt;

	cck_enable = rtw_read32(rtwdev, 0x808) & BIT(28);
	cck_fa_cnt = rtw_read16(rtwdev, 0xa5c);
	ofdm_fa_cnt = rtw_read16(rtwdev, 0xf48);

	dm_info->cck_fa_cnt = cck_fa_cnt;
	dm_info->ofdm_fa_cnt = ofdm_fa_cnt;
	dm_info->total_fa_cnt = ofdm_fa_cnt;
	dm_info->total_fa_cnt += cck_enable ? cck_fa_cnt : 0;

	rtw_write32_set(rtwdev, 0x9a4, BIT(17));
	rtw_write32_clr(rtwdev, 0x9a4, BIT(17));
	rtw_write32_clr(rtwdev, 0xa2c, BIT(15));
	rtw_write32_set(rtwdev, 0xa2c, BIT(15));
	rtw_write32_set(rtwdev, 0xb58, BIT(0));
	rtw_write32_clr(rtwdev, 0xb58, BIT(0));
}

static void rtw8822b_do_iqk(struct rtw_dev *rtwdev)
{
	static int do_iqk_cnt;
	struct rtw_iqk_para para = {.clear = 0, .segment_iqk = 0};
	u32 rf_reg, iqk_fail_mask;
	int counter;
	bool reload;

	rtw_fw_do_iqk(rtwdev, &para);

	for (counter = 0; counter < 300; counter++) {
		rf_reg = rtw_read_rf(rtwdev, RF_PATH_A, RF_DTXLOK, RFREG_MASK);
		if (rf_reg == 0xabcde)
			break;
		msleep(20);
	}
	rtw_write_rf(rtwdev, RF_PATH_A, RF_DTXLOK, RFREG_MASK, 0x0);

	reload = !!rtw_read32_mask(rtwdev, REG_IQKFAILMSK, BIT(16));
	iqk_fail_mask = rtw_read32_mask(rtwdev, REG_IQKFAILMSK, GENMASK(0, 7));
	rtw_dbg(rtwdev, RTW_DBG_PHY,
		"iqk counter=%d reload=%d do_iqk_cnt=%d n_iqk_fail(mask)=0x%02x\n",
		counter, reload, ++do_iqk_cnt, iqk_fail_mask);
}

static struct rtw_pwr_seq_cmd trans_carddis_to_cardemu_8822b[] = {
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_POLLING, BIT(1), BIT(1)},
	{0x004A,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3) | BIT(4) | BIT(7), 0},
	{0x0300,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0x0301,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0xFFFF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 0,
	 RTW_PWR_CMD_END, 0, 0},
};

static struct rtw_pwr_seq_cmd trans_cardemu_to_act_8822b[] = {
	{0x0012,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0x0012,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0020,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0001,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_DELAY, 1, RTW_PWR_DELAY_MS},
	{0x0000,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, (BIT(4) | BIT(3) | BIT(2)), 0},
	{0x0075,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0006,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_POLLING, BIT(1), BIT(1)},
	{0x0075,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0xFF1A,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0x0006,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(7), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, (BIT(4) | BIT(3)), 0},
	{0x10C3,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_POLLING, BIT(0), 0},
	{0x0020,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3), BIT(3)},
	{0x10A8,
	 RTW_PWR_CUT_C_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0x10A9,
	 RTW_PWR_CUT_C_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0xef},
	{0x10AA,
	 RTW_PWR_CUT_C_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x0c},
	{0x0068,
	 RTW_PWR_CUT_C_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(4), BIT(4)},
	{0x0029,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0xF9},
	{0x0024,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(2), 0},
	{0x0074,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), BIT(5)},
	{0x00AF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), BIT(5)},
	{0xFFFF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 0,
	 RTW_PWR_CMD_END, 0, 0},
};

static struct rtw_pwr_seq_cmd trans_act_to_cardemu_8822b[] = {
	{0x0003,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(2), 0},
	{0x0093,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3), 0},
	{0x001F,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0x00EF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0xFF1A,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x30},
	{0x0049,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0x0006,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0002,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0x10C3,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), BIT(1)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_POLLING, BIT(1), 0},
	{0x0020,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3), 0},
	{0x0000,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), BIT(5)},
	{0xFFFF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 0,
	 RTW_PWR_CMD_END, 0, 0},
};

static struct rtw_pwr_seq_cmd trans_cardemu_to_carddis_8822b[] = {
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(7), BIT(7)},
	{0x0007,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x20},
	{0x0067,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(2), BIT(2)},
	{0x004A,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0067,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), 0},
	{0x0067,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(4), 0},
	{0x004F,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0067,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0x0046,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(6), BIT(6)},
	{0x0067,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(2), 0},
	{0x0046,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(7), BIT(7)},
	{0x0062,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(4), BIT(4)},
	{0x0081,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(7) | BIT(6), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3) | BIT(4), BIT(3)},
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_POLLING, BIT(1), 0},
	{0x0090,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0x0044,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0x0040,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x90},
	{0x0041,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x00},
	{0x0042,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x04},
	{0xFFFF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 0,
	 RTW_PWR_CMD_END, 0, 0},
};

static struct rtw_pwr_seq_cmd *card_enable_flow_8822b[] = {
	trans_carddis_to_cardemu_8822b,
	trans_cardemu_to_act_8822b,
	NULL
};

static struct rtw_pwr_seq_cmd *card_disable_flow_8822b[] = {
	trans_act_to_cardemu_8822b,
	trans_cardemu_to_carddis_8822b,
	NULL
};

static struct rtw_intf_phy_para usb2_param_8822b[] = {
	{0xFFFF, 0x00,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_ALL,
	 RTW_INTF_PHY_PLATFORM_ALL},
};

static struct rtw_intf_phy_para usb3_param_8822b[] = {
	{0x0001, 0xA841,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_D,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0xFFFF, 0x0000,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_ALL,
	 RTW_INTF_PHY_PLATFORM_ALL},
};

static struct rtw_intf_phy_para pcie_gen1_param_8822b[] = {
	{0x0001, 0xA841,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0002, 0x60C6,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0008, 0x3596,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0009, 0x321C,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x000A, 0x9623,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0020, 0x94FF,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0021, 0xFFCF,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0026, 0xC006,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0029, 0xFF0E,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x002A, 0x1840,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0xFFFF, 0x0000,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_ALL,
	 RTW_INTF_PHY_PLATFORM_ALL},
};

static struct rtw_intf_phy_para pcie_gen2_param_8822b[] = {
	{0x0001, 0xA841,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0002, 0x60C6,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0008, 0x3597,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0009, 0x321C,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x000A, 0x9623,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0020, 0x94FF,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0021, 0xFFCF,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0026, 0xC006,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x0029, 0xFF0E,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0x002A, 0x3040,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_C,
	 RTW_INTF_PHY_PLATFORM_ALL},
	{0xFFFF, 0x0000,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_ALL,
	 RTW_INTF_PHY_PLATFORM_ALL},
};

static struct rtw_intf_phy_para_table phy_para_table_8822b = {
	.usb2_para	= usb2_param_8822b,
	.usb3_para	= usb3_param_8822b,
	.gen1_para	= pcie_gen1_param_8822b,
	.gen2_para	= pcie_gen2_param_8822b,
	.n_usb2_para	= ARRAY_SIZE(usb2_param_8822b),
	.n_usb3_para	= ARRAY_SIZE(usb2_param_8822b),
	.n_gen1_para	= ARRAY_SIZE(pcie_gen1_param_8822b),
	.n_gen2_para	= ARRAY_SIZE(pcie_gen2_param_8822b),
};

static const struct rtw_rfe_def rtw8822b_rfe_defs[] = {
	[2] = RTW_DEF_RFE(8822b, 2, 2),
	[5] = RTW_DEF_RFE(8822b, 5, 5),
};

static struct rtw_hw_reg rtw8822b_dig[] = {
	[0] = { .addr = 0xc50, .mask = 0x7f },
	[1] = { .addr = 0xe50, .mask = 0x7f },
};

static struct rtw_page_table page_table_8822b[] = {
	{64, 64, 64, 64, 1},
	{64, 64, 64, 64, 1},
	{64, 64, 0, 0, 1},
	{64, 64, 64, 0, 1},
	{64, 64, 64, 64, 1},
};

static struct rtw_rqpn rqpn_table_8822b[] = {
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_EXTRA, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_EXTRA, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_HIGH,
	 RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_EXTRA, RTW_DMA_MAPPING_HIGH},
};

static struct rtw_chip_ops rtw8822b_ops = {
	.phy_set_param		= rtw8822b_phy_set_param,
	.read_efuse		= rtw8822b_read_efuse,
	.query_rx_desc		= rtw8822b_query_rx_desc,
	.set_channel		= rtw8822b_set_channel,
	.mac_init		= rtw8822b_mac_init,
	.read_rf		= rtw_phy_read_rf,
	.write_rf		= rtw_phy_write_rf_reg_sipi,
	.set_tx_power_index	= rtw8822b_set_tx_power_index,
	.set_antenna		= rtw8822b_set_antenna,
	.cfg_ldo25		= rtw8822b_cfg_ldo25,
	.false_alarm_statistics	= rtw8822b_false_alarm_statistics,
	.do_iqk			= rtw8822b_do_iqk,
};

struct rtw_chip_info rtw8822b_hw_spec = {
	.ops = &rtw8822b_ops,
	.id = RTW_CHIP_TYPE_8822B,
	.fw_name = "rtw88/rtw8822b_fw.bin",
	.tx_pkt_desc_sz = 48,
	.tx_buf_desc_sz = 16,
	.rx_pkt_desc_sz = 24,
	.rx_buf_desc_sz = 8,
	.phy_efuse_size = 1024,
	.log_efuse_size = 768,
	.ptct_efuse_size = 96,
	.txff_size = 262144,
	.rxff_size = 24576,
	.txgi_factor = 1,
	.is_pwr_by_rate_dec = true,
	.max_power_index = 0x3f,
	.csi_buf_pg_num = 0,
	.band = RTW_BAND_2G | RTW_BAND_5G,
	.page_size = 128,
	.dig_min = 0x1c,
	.ht_supported = true,
	.vht_supported = true,
	.sys_func_en = 0xDC,
	.pwr_on_seq = card_enable_flow_8822b,
	.pwr_off_seq = card_disable_flow_8822b,
	.page_table = page_table_8822b,
	.rqpn_table = rqpn_table_8822b,
	.intf_table = &phy_para_table_8822b,
	.dig = rtw8822b_dig,
	.rf_base_addr = {0x2800, 0x2c00},
	.rf_sipi_addr = {0xc90, 0xe90},
	.mac_tbl = &rtw8822b_mac_tbl,
	.agc_tbl = &rtw8822b_agc_tbl,
	.bb_tbl = &rtw8822b_bb_tbl,
	.rf_tbl = {&rtw8822b_rf_a_tbl, &rtw8822b_rf_b_tbl},
	.rfe_defs = rtw8822b_rfe_defs,
	.rfe_defs_size = ARRAY_SIZE(rtw8822b_rfe_defs),
};
EXPORT_SYMBOL(rtw8822b_hw_spec);

MODULE_FIRMWARE("rtw88/rtw8822b_fw.bin");
