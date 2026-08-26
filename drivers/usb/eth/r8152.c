// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015 Realtek Semiconductor Corp. All rights reserved.
 *
 */

#include <dm.h>
#include <errno.h>
#include <log.h>
#include <malloc.h>
#include <memalign.h>
#include <net.h>
#include <usb.h>
#include <linux/delay.h>
#include <linux/mii.h>
#include <linux/bitops.h>
#include <linux/mdio.h>
#include "usb_ether.h"
#include "r8152.h"

struct r8152_version {
	unsigned short tcr;
	unsigned short version;
	bool           supports_gmii;
	bool           supports_xgmii;
};

static const struct r8152_version r8152_versions[] = {
	{ 0x4c00, RTL_VER_01, 0, 0},
	{ 0x4c10, RTL_VER_02, 0, 0},
	{ 0x5c00, RTL_VER_03, 1, 0},
	{ 0x5c10, RTL_VER_04, 1, 0},
	{ 0x5c20, RTL_VER_05, 1, 0},
	{ 0x5c30, RTL_VER_06, 1, 0},
	{ 0x4800, RTL_VER_07, 0, 0},
	{ 0x6000, RTL_VER_08, 1, 0},
	{ 0x6010, RTL_VER_09, 1, 0},
	{ 0x7010, RTL_TEST_01, 1, 1},
	{ 0x7020, RTL_VER_10, 1, 1},
	{ 0x7030, RTL_VER_11, 1, 1},
	{ 0x7400, RTL_VER_12, 1, 1},
	{ 0x7410, RTL_VER_13, 1, 1},
	{ 0x6400, RTL_VER_14, 1, 0},
	{ 0x7420, RTL_VER_15, 1, 1},
};

static
int get_registers(struct r8152 *tp, u16 value, u16 index, u16 size, void *data)
{
	ALLOC_CACHE_ALIGN_BUFFER(void *, tmp, size);
	int ret;

	ret = usb_control_msg(tp->udev, usb_rcvctrlpipe(tp->udev, 0),
		RTL8152_REQ_GET_REGS, RTL8152_REQT_READ,
		value, index, tmp, size, 500);
	memcpy(data, tmp, size);
	return ret;
}

static
int set_registers(struct r8152 *tp, u16 value, u16 index, u16 size, void *data)
{
	ALLOC_CACHE_ALIGN_BUFFER(void *, tmp, size);

	memcpy(tmp, data, size);
	return usb_control_msg(tp->udev, usb_sndctrlpipe(tp->udev, 0),
			       RTL8152_REQ_SET_REGS, RTL8152_REQT_WRITE,
			       value, index, tmp, size, 500);
}

int generic_ocp_read(struct r8152 *tp, u16 index, u16 size,
		     void *data, u16 type)
{
	u16 burst_size = 64;
	int ret;
	int txsize;

	/* both size and index must be 4 bytes align */
	if ((size & 3) || !size || (index & 3) || !data)
		return -EINVAL;

	if (index + size > 0xffff)
		return -EINVAL;

	while (size) {
		txsize = min(size, burst_size);
		ret = get_registers(tp, index, type, txsize, data);
		if (ret < 0)
			break;

		index += txsize;
		data += txsize;
		size -= txsize;
	}

	return ret;
}

int generic_ocp_write(struct r8152 *tp, u16 index, u16 byteen,
		      u16 size, void *data, u16 type)
{
	int ret;
	u16 byteen_start, byteen_end, byte_en_to_hw;
	u16 burst_size = 512;
	int txsize;

	/* both size and index must be 4 bytes align */
	if ((size & 3) || !size || (index & 3) || !data)
		return -EINVAL;

	if (index + size > 0xffff)
		return -EINVAL;

	byteen_start = byteen & BYTE_EN_START_MASK;
	byteen_end = byteen & BYTE_EN_END_MASK;

	byte_en_to_hw = byteen_start | (byteen_start << 4);
	ret = set_registers(tp, index, type | byte_en_to_hw, 4, data);
	if (ret < 0)
		return ret;

	index += 4;
	data += 4;
	size -= 4;

	if (size) {
		size -= 4;

		while (size) {
			txsize = min(size, burst_size);

			ret = set_registers(tp, index,
					    type | BYTE_EN_DWORD,
					    txsize, data);
			if (ret < 0)
				return ret;

			index += txsize;
			data += txsize;
			size -= txsize;
		}

		byte_en_to_hw = byteen_end | (byteen_end >> 4);
		ret = set_registers(tp, index, type | byte_en_to_hw, 4, data);
		if (ret < 0)
			return ret;
	}

	return ret;
}

int pla_ocp_read(struct r8152 *tp, u16 index, u16 size, void *data)
{
	return generic_ocp_read(tp, index, size, data, MCU_TYPE_PLA);
}

int pla_ocp_write(struct r8152 *tp, u16 index, u16 byteen, u16 size, void *data)
{
	return generic_ocp_write(tp, index, byteen, size, data, MCU_TYPE_PLA);
}

int usb_ocp_read(struct r8152 *tp, u16 index, u16 size, void *data)
{
	return generic_ocp_read(tp, index, size, data, MCU_TYPE_USB);
}

int usb_ocp_write(struct r8152 *tp, u16 index, u16 byteen, u16 size, void *data)
{
	return generic_ocp_write(tp, index, byteen, size, data, MCU_TYPE_USB);
}

u32 ocp_read_dword(struct r8152 *tp, u16 type, u16 index)
{
	__le32 data;

	generic_ocp_read(tp, index, sizeof(data), &data, type);

	return __le32_to_cpu(data);
}

void ocp_write_dword(struct r8152 *tp, u16 type, u16 index, u32 data)
{
	__le32 tmp = __cpu_to_le32(data);

	generic_ocp_write(tp, index, BYTE_EN_DWORD, sizeof(tmp), &tmp, type);
}

u16 ocp_read_word(struct r8152 *tp, u16 type, u16 index)
{
	u32 data;
	__le32 tmp;
	u8 shift = index & 2;

	index &= ~3;

	generic_ocp_read(tp, index, sizeof(tmp), &tmp, type);

	data = __le32_to_cpu(tmp);
	data >>= (shift * 8);
	data &= 0xffff;

	return data;
}

void ocp_write_word(struct r8152 *tp, u16 type, u16 index, u32 data)
{
	u32 mask = 0xffff;
	__le32 tmp;
	u16 byen = BYTE_EN_WORD;
	u8 shift = index & 2;

	data &= mask;

	if (index & 2) {
		byen <<= shift;
		mask <<= (shift * 8);
		data <<= (shift * 8);
		index &= ~3;
	}

	tmp = __cpu_to_le32(data);

	generic_ocp_write(tp, index, byen, sizeof(tmp), &tmp, type);
}

u8 ocp_read_byte(struct r8152 *tp, u16 type, u16 index)
{
	u32 data;
	__le32 tmp;
	u8 shift = index & 3;

	index &= ~3;

	generic_ocp_read(tp, index, sizeof(tmp), &tmp, type);

	data = __le32_to_cpu(tmp);
	data >>= (shift * 8);
	data &= 0xff;

	return data;
}

void ocp_write_byte(struct r8152 *tp, u16 type, u16 index, u32 data)
{
	u32 mask = 0xff;
	__le32 tmp;
	u16 byen = BYTE_EN_BYTE;
	u8 shift = index & 3;

	data &= mask;

	if (index & 3) {
		byen <<= shift;
		mask <<= (shift * 8);
		data <<= (shift * 8);
		index &= ~3;
	}

	tmp = __cpu_to_le32(data);

	generic_ocp_write(tp, index, byen, sizeof(tmp), &tmp, type);
}

u16 ocp_reg_read(struct r8152 *tp, u16 addr)
{
	u16 ocp_base, ocp_index;

	ocp_base = addr & 0xf000;
	if (ocp_base != tp->ocp_base) {
		ocp_write_word(tp, MCU_TYPE_PLA, PLA_OCP_GPHY_BASE, ocp_base);
		tp->ocp_base = ocp_base;
	}

	ocp_index = (addr & 0x0fff) | 0xb000;
	return ocp_read_word(tp, MCU_TYPE_PLA, ocp_index);
}

void ocp_reg_write(struct r8152 *tp, u16 addr, u16 data)
{
	u16 ocp_base, ocp_index;

	ocp_base = addr & 0xf000;
	if (ocp_base != tp->ocp_base) {
		ocp_write_word(tp, MCU_TYPE_PLA, PLA_OCP_GPHY_BASE, ocp_base);
		tp->ocp_base = ocp_base;
	}

	ocp_index = (addr & 0x0fff) | 0xb000;
	ocp_write_word(tp, MCU_TYPE_PLA, ocp_index, data);
}

static void r8152_mdio_write(struct r8152 *tp, u32 reg_addr, u32 value)
{
	ocp_reg_write(tp, OCP_BASE_MII + reg_addr * 2, value);
}

static int r8152_mdio_read(struct r8152 *tp, u32 reg_addr)
{
	return ocp_reg_read(tp, OCP_BASE_MII + reg_addr * 2);
}

void sram_write(struct r8152 *tp, u16 addr, u16 data)
{
	ocp_reg_write(tp, OCP_SRAM_ADDR, addr);
	ocp_reg_write(tp, OCP_SRAM_DATA, data);
}

u16 sram_read(struct r8152 *tp, u16 addr)
{
	ocp_reg_write(tp, OCP_SRAM_ADDR, addr);
	return ocp_reg_read(tp, OCP_SRAM_DATA);
}

static void
ocp_dword_w0w1(struct r8152 *tp, u16 type, u16 index, u32 clear, u32 set)
{
	u32 ocp_data;

	ocp_data = ocp_read_dword(tp, type, index);
	ocp_data = (ocp_data & ~clear) | set;
	ocp_write_dword(tp, type, index, ocp_data);
}

static void
ocp_word_w0w1(struct r8152 *tp, u16 type, u16 index, u16 clear, u16 set)
{
	u32 ocp_data;

	ocp_data = ocp_read_word(tp, type, index);
	ocp_data = (ocp_data & ~clear) | set;
	ocp_write_word(tp, type, index, ocp_data);
}

static void
ocp_byte_w0w1(struct r8152 *tp, u16 type, u16 index, u8 clear, u8 set)
{
	u32 ocp_data;

	ocp_data = ocp_read_byte(tp, type, index);
	ocp_data = (ocp_data & ~clear) | set;
	ocp_write_byte(tp, type, index, ocp_data);
}

static void
ocp_dword_clr_bits(struct r8152 *tp, u16 type, u16 index, u32 clear)
{
	ocp_dword_w0w1(tp, type, index, clear, 0);
}

static void
ocp_dword_set_bits(struct r8152 *tp, u16 type, u16 index, u32 set)
{
	ocp_dword_w0w1(tp, type, index, 0, set);
}

static void
ocp_word_clr_bits(struct r8152 *tp, u16 type, u16 index, u16 clear)
{
	ocp_word_w0w1(tp, type, index, clear, 0);
}

void
ocp_word_set_bits(struct r8152 *tp, u16 type, u16 index, u16 set)
{
	ocp_word_w0w1(tp, type, index, 0, set);
}

static void
ocp_word_test_and_clr_bits(struct r8152 *tp, u16 type, u16 index, u16 clear)
{
	u32 ocp_data;

	ocp_data = ocp_read_word(tp, type, index);
	if (ocp_data & clear)
		ocp_write_word(tp, type, index, ocp_data & ~clear);
}

static void
ocp_byte_clr_bits(struct r8152 *tp, u16 type, u16 index, u8 clear)
{
	ocp_byte_w0w1(tp, type, index, clear, 0);
}

static void
ocp_byte_set_bits(struct r8152 *tp, u16 type, u16 index, u8 set)
{
	ocp_byte_w0w1(tp, type, index, 0, set);
}

static void ocp_reg_w0w1(struct r8152 *tp, u16 addr, u16 clear, u16 set)
{
	u16 data;

	data = ocp_reg_read(tp, addr);
	data = (data & ~clear) | set;
	ocp_reg_write(tp, addr, data);
}

void ocp_reg_clr_bits(struct r8152 *tp, u16 addr, u16 clear)
{
	ocp_reg_w0w1(tp, addr, clear, 0);
}

void ocp_reg_set_bits(struct r8152 *tp, u16 addr, u16 set)
{
	ocp_reg_w0w1(tp, addr, 0, set);
}

static void sram_write_w0w1(struct r8152 *tp, u16 addr, u16 clear, u16 set)
{
	u16 data;

	data = sram_read(tp, addr);
	data = (data & ~clear) | set;
	ocp_reg_write(tp, OCP_SRAM_DATA, data);
}

static void sram_set_bits(struct r8152 *tp, u16 addr, u16 set)
{
	return sram_write_w0w1(tp, addr, 0, set);
}

static void sram_clr_bits(struct r8152 *tp, u16 addr, u16 clear)
{
	return sram_write_w0w1(tp, addr, clear, 0);
}

static void r8152_mdio_test_and_clr_bit(struct r8152 *tp, u16 addr, u16 clear)
{
	u16 data;

	data = r8152_mdio_read(tp, addr);
	if (data & clear) {
		data &= ~clear;
		r8152_mdio_write(tp, addr, data);
	}
}

int r8152_wait_for_bit(struct r8152 *tp, bool ocp_reg, u16 type, u16 index,
		       const u32 mask, bool set, unsigned int timeout)
{
	u32 val;

	while (--timeout) {
		if (ocp_reg)
			val = ocp_reg_read(tp, index);
		else
			val = ocp_read_dword(tp, type, index);

		if (!set)
			val = ~val;

		if ((val & mask) == mask)
			return 0;

		mdelay(1);
	}

	debug("%s: Timeout (index=%04x mask=%08x timeout=%d)\n",
	      __func__, index, mask, timeout);

	return -ETIMEDOUT;
}

static void r8152b_reset_packet_filter(struct r8152 *tp)
{
	u32 ocp_data;

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_FMC);
	ocp_data &= ~FMC_FCR_MCU_EN;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_FMC, ocp_data);
	ocp_data |= FMC_FCR_MCU_EN;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_FMC, ocp_data);
}

static void rtl8152_wait_fifo_empty(struct r8152 *tp)
{
	int ret;

	ret = r8152_wait_for_bit(tp, 0, MCU_TYPE_PLA, PLA_PHY_PWR,
				 PLA_PHY_PWR_TXEMP, 1, R8152_WAIT_TIMEOUT);
	if (ret)
		debug("Timeout waiting for FIFO empty\n");

	ret = r8152_wait_for_bit(tp, 0, MCU_TYPE_PLA, PLA_TCR0,
				 TCR0_TX_EMPTY, 1, R8152_WAIT_TIMEOUT);
	if (ret)
		debug("Timeout waiting for TX empty\n");
}

static void rtl8152_nic_reset(struct r8152 *tp)
{
	int i;

	switch (tp->version) {
	case RTL_TEST_01:
	case RTL_VER_10:
	case RTL_VER_11:
		ocp_byte_clr_bits(tp, MCU_TYPE_PLA, PLA_CR, PLA_CR_TE);
		ocp_word_clr_bits(tp, MCU_TYPE_USB, USB_BMU_RESET,
				  BMU_RESET_EP_IN);
		ocp_word_set_bits(tp, MCU_TYPE_USB, USB_USB_CTRL,
				  CDC_ECM_EN);
		ocp_byte_clr_bits(tp, MCU_TYPE_PLA, PLA_CR, PLA_CR_RE);
		ocp_word_set_bits(tp, MCU_TYPE_USB, USB_BMU_RESET,
				  BMU_RESET_EP_IN);
		ocp_word_clr_bits(tp, MCU_TYPE_USB, USB_USB_CTRL,
				  CDC_ECM_EN);
		break;
	case RTL_VER_01:
	case RTL_VER_02:
	case RTL_VER_03:
	case RTL_VER_04:
	case RTL_VER_05:
	case RTL_VER_06:
	case RTL_VER_07:
	case RTL_VER_08:
	case RTL_VER_09:
	case RTL_VER_12:
	case RTL_VER_13:
	case RTL_VER_15:
		ocp_write_byte(tp, MCU_TYPE_PLA, PLA_CR, PLA_CR_RST);
		for (i = 0; i < 1000; i++) {
			u32 ocp_data;

			ocp_data = ocp_read_byte(tp, MCU_TYPE_PLA, PLA_CR);
			if (!(ocp_data & PLA_CR_RST))
				break;
			mdelay(1);
		}
		break;
	default:
		ocp_byte_clr_bits(tp, MCU_TYPE_PLA, PLA_CR,
				  PLA_CR_RE | PLA_CR_TE);
		break;

	}
}

static u16 rtl8152_get_speed(struct r8152 *tp)
{
	/*
	 * Must be a word: _2500bps is BIT(10) and _500bps is BIT(8), so a
	 * byte read cannot see the speeds the RTL8156 family reports.
	 */
	return ocp_read_word(tp, MCU_TYPE_PLA, PLA_PHYSTATUS);
}

static void rtl_set_eee_plus(struct r8152 *tp)
{
	u32 ocp_data;

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_EEEP_CR);
	ocp_data &= ~EEEP_CR_EEEP_TX;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_EEEP_CR, ocp_data);
}

static void rxdy_gated_en(struct r8152 *tp, bool enable)
{
	u32 ocp_data;

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_MISC_1);
	if (enable)
		ocp_data |= RXDY_GATED_EN;
	else
		ocp_data &= ~RXDY_GATED_EN;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_MISC_1, ocp_data);
}

static void rtl8152_set_rx_mode(struct r8152 *tp)
{
	u32 ocp_data;
	__le32 tmp[2];

	tmp[0] = 0xffffffff;
	tmp[1] = 0xffffffff;

	pla_ocp_write(tp, PLA_MAR, BYTE_EN_DWORD, sizeof(tmp), tmp);

	ocp_data = ocp_read_dword(tp, MCU_TYPE_PLA, PLA_RCR);
	ocp_data |= RCR_APM | RCR_AM | RCR_AB;
	ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RCR, ocp_data);
}

/*
 * Inter-frame gap and the 10M idle timer, per negotiated speed.
 * Ported from Linux drivers/net/usb/r8152.c rtl_set_ifg().
 */
static void rtl_set_ifg(struct r8152 *tp, u16 speed)
{
	if ((speed & (_10bps | _100bps)) && !(speed & FULL_DUP)) {
		ocp_word_w0w1(tp, MCU_TYPE_PLA, PLA_TCR1, IFG_MASK, IFG_144NS);

		ocp_word_clr_bits(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL4,
				  TX10MIDLE_EN);
	} else {
		ocp_word_w0w1(tp, MCU_TYPE_PLA, PLA_TCR1, IFG_MASK, IFG_96NS);

		ocp_word_set_bits(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL4,
				  TX10MIDLE_EN);
	}
}

static inline void r8153b_rx_agg_chg_indicate(struct r8152 *tp)
{
	ocp_write_byte(tp, MCU_TYPE_USB, USB_UPT_RXDMA_OWN,
		       OWN_UPDATE | OWN_CLEAR);
}

static int rtl_enable(struct r8152 *tp)
{
	r8152b_reset_packet_filter(tp);

	ocp_byte_set_bits(tp, MCU_TYPE_PLA, PLA_CR, PLA_CR_RE | PLA_CR_TE);

	switch (tp->version) {
	case RTL_VER_01:
	case RTL_VER_02:
	case RTL_VER_03:
	case RTL_VER_04:
	case RTL_VER_05:
	case RTL_VER_06:
	case RTL_VER_07:
		break;
	default:
		r8153b_rx_agg_chg_indicate(tp);
		break;
	}

	rxdy_gated_en(tp, false);

	rtl8152_set_rx_mode(tp);

	return 0;
}

static int rtl8152_enable(struct r8152 *tp)
{
	rtl_set_eee_plus(tp);

	return rtl_enable(tp);
}

static void r8153_set_rx_early_timeout(struct r8152 *tp)
{
	u32 ocp_data = tp->coalesce / 8;

	switch (tp->version) {
	case RTL_VER_03:
	case RTL_VER_04:
	case RTL_VER_05:
	case RTL_VER_06:
		ocp_write_word(tp, MCU_TYPE_USB, USB_RX_EARLY_TIMEOUT,
			       ocp_data);
		break;

	case RTL_VER_08:
	case RTL_VER_09:
	case RTL_VER_14:
		/* The RTL8153B uses USB_RX_EXTRA_AGGR_TMR for rx timeout
		 * primarily. For USB_RX_EARLY_TIMEOUT, we fix it to 1264ns.
		 */
		ocp_write_word(tp, MCU_TYPE_USB, USB_RX_EARLY_TIMEOUT,
			       RX_AUXILIARY_TIMER / 8);
		ocp_write_word(tp, MCU_TYPE_USB, USB_RX_EXTRA_AGGR_TMR,
			       ocp_data);
		break;

	case RTL_VER_10:
	case RTL_VER_11:
	case RTL_VER_12:
	case RTL_VER_13:
	case RTL_VER_15:
		ocp_write_word(tp, MCU_TYPE_USB, USB_RX_EARLY_TIMEOUT,
			       640 / 8);
		ocp_write_word(tp, MCU_TYPE_USB, USB_RX_EXTRA_AGGR_TMR,
			       ocp_data);
		break;

	default:
		debug("** %s Invalid Device\n", __func__);
		break;
	}
}

static void r8153_set_rx_early_size(struct r8152 *tp)
{
	u32 ocp_data = (RTL8152_AGG_BUF_SZ - RTL8153_RMS -
			sizeof(struct rx_desc));

	switch (tp->version) {
	case RTL_VER_03:
	case RTL_VER_04:
	case RTL_VER_05:
	case RTL_VER_06:
		ocp_write_word(tp, MCU_TYPE_USB, USB_RX_EARLY_SIZE,
			       ocp_data / 4);
		break;

	case RTL_VER_08:
	case RTL_VER_09:
	case RTL_TEST_01:
	case RTL_VER_10:
	case RTL_VER_11:
	case RTL_VER_12:
	case RTL_VER_13:
	case RTL_VER_14:
	case RTL_VER_15:
		ocp_write_word(tp, MCU_TYPE_USB, USB_RX_EARLY_SIZE,
			       ocp_data / 8);
		break;

	default:
		debug("** %s Invalid Device\n", __func__);
		break;
	}
}

static int rtl8153_enable(struct r8152 *tp)
{
	rtl_set_eee_plus(tp);
	r8153_set_rx_early_timeout(tp);
	r8153_set_rx_early_size(tp);

	switch (tp->version) {
	case RTL_VER_14:
		ocp_word_clr_bits(tp, MCU_TYPE_USB, USB_FW_TASK,
				  FC_PATCH_TASK);
		mdelay(2);
		ocp_word_set_bits(tp, MCU_TYPE_USB, USB_FW_TASK,
				  FC_PATCH_TASK);
		break;
	default:
		break;
	}

	return rtl_enable(tp);
}

static u32 fc_pause_on_auto(struct r8152 *tp)
{
	return (2048 + 6 * 1024);
}

static u32 fc_pause_off_auto(struct r8152 *tp)
{
	return (2048 + 14 * 1024);
}

static void r8156_fc_parameter(struct r8152 *tp)
{
	u32 pause_on = fc_pause_on_auto(tp);
	u32 pause_off = fc_pause_off_auto(tp);

	ocp_write_word(tp, MCU_TYPE_PLA, PLA_RX_FIFO_FULL, pause_on / 16);
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_RX_FIFO_EMPTY, pause_off / 16);
}

/*
 * Ported from Linux drivers/net/usb/r8152.c rtl8156_enable().  The version
 * tests matter: RTL8156B and later default to packet-count based RX
 * aggregation, so USB_RX_AGGR_NUM has to be cleared or the bulk-IN never
 * completes for a single packet and every receive times out.
 */
static int rtl8156_enable(struct r8152 *tp)
{
	u16 speed;

	if (tp->version < RTL_VER_12)
		r8156_fc_parameter(tp);

	rtl_set_eee_plus(tp);

	if (tp->version >= RTL_VER_12)
		ocp_word_clr_bits(tp, MCU_TYPE_USB, USB_RX_AGGR_NUM,
				  RX_AGGR_NUM_MASK);

	r8153_set_rx_early_timeout(tp);
	r8153_set_rx_early_size(tp);

	speed = rtl8152_get_speed(tp);
	rtl_set_ifg(tp, speed);

	if (speed & _2500bps)
		ocp_word_clr_bits(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL4,
				  IDLE_SPDWN_EN);
	else
		ocp_word_set_bits(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL4,
				  IDLE_SPDWN_EN);

	if (tp->version < RTL_VER_12) {
		if (speed & _1000bps)
			ocp_write_word(tp, MCU_TYPE_PLA, PLA_EEE_TXTWSYS, 0x11);
		else if (speed & _500bps)
			ocp_write_word(tp, MCU_TYPE_PLA, PLA_EEE_TXTWSYS, 0x3d);
	}

	if (tp->udev->speed == USB_SPEED_HIGH) {
		/* USB 0xb45e[3:0] l1_nyet_hird */
		if (is_flow_control(speed))
			ocp_word_w0w1(tp, MCU_TYPE_USB, USB_L1_CTRL, 0xf, 0xf);
		else
			ocp_word_w0w1(tp, MCU_TYPE_USB, USB_L1_CTRL, 0xf, 0x1);
	}

	ocp_word_clr_bits(tp, MCU_TYPE_USB, USB_FW_TASK, FC_PATCH_TASK);
	mdelay(2);
	ocp_word_set_bits(tp, MCU_TYPE_USB, USB_FW_TASK, FC_PATCH_TASK);

	return rtl_enable(tp);
}

static void rtl_disable(struct r8152 *tp)
{
	u32 ocp_data;

	ocp_data = ocp_read_dword(tp, MCU_TYPE_PLA, PLA_RCR);
	ocp_data &= ~RCR_ACPT_ALL;
	ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RCR, ocp_data);

	rxdy_gated_en(tp, true);

	rtl8152_wait_fifo_empty(tp);
	rtl8152_nic_reset(tp);
}

static void r8152_power_cut_en(struct r8152 *tp, bool enable)
{
	u32 ocp_data;

	ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_UPS_CTRL);
	if (enable)
		ocp_data |= POWER_CUT;
	else
		ocp_data &= ~POWER_CUT;
	ocp_write_word(tp, MCU_TYPE_USB, USB_UPS_CTRL, ocp_data);

	ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_PM_CTRL_STATUS);
	ocp_data &= ~RESUME_INDICATE;
	ocp_write_word(tp, MCU_TYPE_USB, USB_PM_CTRL_STATUS, ocp_data);
}

static void rtl_rx_vlan_en(struct r8152 *tp, bool enable)
{
	switch (tp->version) {
	case RTL_VER_01:
	case RTL_VER_02:
	case RTL_VER_03:
	case RTL_VER_04:
	case RTL_VER_05:
	case RTL_VER_06:
	case RTL_VER_07:
	case RTL_VER_08:
	case RTL_VER_09:
	case RTL_VER_14:
		if (enable)
			ocp_word_set_bits(tp, MCU_TYPE_PLA, PLA_CPCR,
					  CPCR_RX_VLAN);
		else
			ocp_word_clr_bits(tp, MCU_TYPE_PLA, PLA_CPCR,
					  CPCR_RX_VLAN);
		break;

	case RTL_TEST_01:
	case RTL_VER_10:
	case RTL_VER_11:
	case RTL_VER_12:
	case RTL_VER_13:
	case RTL_VER_15:
	default:
		if (enable)
			ocp_word_set_bits(tp, MCU_TYPE_PLA, PLA_RCR1,
					  OUTER_VLAN | INNER_VLAN);
		else
			ocp_word_clr_bits(tp, MCU_TYPE_PLA, PLA_RCR1,
					  OUTER_VLAN | INNER_VLAN);
		break;
	}
}

static void r8153_mac_clk_speed_down(struct r8152 *tp, bool enable)
{
	/* MAC clock speed down */
	if (enable)
		ocp_word_set_bits(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL2,
				  MAC_CLK_SPDWN_EN);
	else
		ocp_word_clr_bits(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL2,
				  MAC_CLK_SPDWN_EN);
}

static void r8156_mac_clk_spd(struct r8152 *tp, bool enable)
{
	/* MAC clock speed down */
	if (enable) {
		/* aldps_spdwn_ratio, tp10_spdwn_ratio */
		ocp_write_word(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL,
			       0x0403);

		/* eee_spdwn_ratio */
		ocp_word_w0w1(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL2,
				    EEE_SPDWN_RATIO_MASK,
				    MAC_CLK_SPDWN_EN | 0x03);
	} else {
		ocp_word_clr_bits(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL2,
				  MAC_CLK_SPDWN_EN);
	}
}

static void r8153_u1u2en(struct r8152 *tp, bool enable)
{
	u8 u1u2[8];

	if (enable)
		memset(u1u2, 0xff, sizeof(u1u2));
	else
		memset(u1u2, 0x00, sizeof(u1u2));

	usb_ocp_write(tp, USB_TOLERANCE, BYTE_EN_SIX_BYTES, sizeof(u1u2), u1u2);
}

static void r8153b_u1u2en(struct r8152 *tp, bool enable)
{
	u16 ocp_data;

	ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_LPM_CONFIG);
	if (enable)
		ocp_data |= LPM_U1U2_EN;
	else
		ocp_data &= ~LPM_U1U2_EN;

	ocp_write_word(tp, MCU_TYPE_USB, USB_LPM_CONFIG, ocp_data);
}

static void r8153_u2p3en(struct r8152 *tp, bool enable)
{
	u32 ocp_data;

	ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_U2P3_CTRL);
	if (enable && tp->version != RTL_VER_03 && tp->version != RTL_VER_04)
		ocp_data |= U2P3_ENABLE;
	else
		ocp_data &= ~U2P3_ENABLE;
	ocp_write_word(tp, MCU_TYPE_USB, USB_U2P3_CTRL, ocp_data);
}

bool r8156b_flash_used(struct r8152 *tp)
{
	u32 ocp_data;

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_GPHY_CTRL);
	if (!(ocp_data & GPHY_FLASH))
		return false;

	ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_GPHY_CTRL);
	if (!(ocp_data & BYPASS_FLASH))
		return true;
	else
		return false;
}

static u16 r8153_phy_status(struct r8152 *tp, u16 desired)
{
	u16 data;
	int i;

	for (i = 0; i < 500; i++) {
		data = ocp_reg_read(tp, OCP_PHY_STATUS);
		data &= PHY_STAT_MASK;
		if (desired) {
			if (data == desired)
				break;
		} else if (data == PHY_STAT_LAN_ON || data == PHY_STAT_PWRDN ||
			   data == PHY_STAT_EXT_INIT) {
			break;
		}

		mdelay(20);
	}

	return data;
}

static void r8153_power_cut_en(struct r8152 *tp, bool enable)
{
	u32 ocp_data;

	ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_POWER_CUT);
	if (enable)
		ocp_data |= PWR_EN | PHASE2_EN;
	else
		ocp_data &= ~(PWR_EN | PHASE2_EN);
	ocp_write_word(tp, MCU_TYPE_USB, USB_POWER_CUT, ocp_data);

	ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_MISC_0);
	ocp_data &= ~PCUT_STATUS;
	ocp_write_word(tp, MCU_TYPE_USB, USB_MISC_0, ocp_data);
}

static void r8153b_power_cut_en(struct r8152 *tp, bool enable)
{
	if (enable)
		ocp_word_set_bits(tp, MCU_TYPE_USB, USB_POWER_CUT,
				  PWR_EN | PHASE2_EN);
	else
		ocp_word_clr_bits(tp, MCU_TYPE_USB, USB_POWER_CUT,
				  PWR_EN);

	ocp_word_clr_bits(tp, MCU_TYPE_USB, USB_MISC_0, PCUT_STATUS);
}

static void r8153_teredo_off(struct r8152 *tp)
{
	switch (tp->version) {
	case RTL_VER_01:
	case RTL_VER_02:
	case RTL_VER_03:
	case RTL_VER_04:
	case RTL_VER_05:
	case RTL_VER_06:
	case RTL_VER_07:
		ocp_word_clr_bits(tp, MCU_TYPE_PLA, PLA_TEREDO_CFG,
				  TEREDO_SEL | TEREDO_RS_EVENT_MASK |
				  OOB_TEREDO_EN);
		break;

	case RTL_TEST_01:
	case RTL_VER_10:
	case RTL_VER_11:
	case RTL_VER_12:
	case RTL_VER_13:
	case RTL_VER_14:
	case RTL_VER_15:
	default:
		/* The bit 0 ~ 7 are relative with teredo settings. They are
		 * W1C (write 1 to clear), so set all 1 to disable it.
		 */
		ocp_write_byte(tp, MCU_TYPE_PLA, PLA_TEREDO_CFG, 0xff);
		break;
	}

	ocp_write_word(tp, MCU_TYPE_PLA, PLA_WDT6_CTRL, WDT6_SET_MODE);
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_REALWOW_TIMER, 0);
	ocp_write_dword(tp, MCU_TYPE_PLA, PLA_TEREDO_TIMER, 0);
}

static void rtl_reset_bmu(struct r8152 *tp)
{
	u8 ocp_data;

	ocp_data = ocp_read_byte(tp, MCU_TYPE_USB, USB_BMU_RESET);
	ocp_data &= ~(BMU_RESET_EP_IN | BMU_RESET_EP_OUT);
	ocp_write_byte(tp, MCU_TYPE_USB, USB_BMU_RESET, ocp_data);
	ocp_data |= BMU_RESET_EP_IN | BMU_RESET_EP_OUT;
	ocp_write_byte(tp, MCU_TYPE_USB, USB_BMU_RESET, ocp_data);
}

void rtl_reset_ocp_base(struct r8152 *tp)
{
	tp->ocp_base = -1;
}

static int rtl_phy_patch_request(struct r8152 *tp, bool request, bool wait)
{
	u16 check;
	int i;

	if (request) {
		ocp_reg_set_bits(tp, OCP_PHY_PATCH_CMD, PATCH_REQUEST);
		check = 0;
	} else {
		ocp_reg_clr_bits(tp, OCP_PHY_PATCH_CMD, PATCH_REQUEST);
		check = PATCH_READY;
	}

	for (i = 0; wait && i < 5000; i++) {
		u16 data;

		mdelay(1);
		data = ocp_reg_read(tp, OCP_PHY_PATCH_STAT);
		if ((data & PATCH_READY) ^ check)
			break;
	}

	if (request && wait && i == 5000) {
		ocp_reg_clr_bits(tp, OCP_PHY_PATCH_CMD, PATCH_REQUEST);
		return -ETIME;
	}

	return 0;
}

static int r8152_read_mac(struct r8152 *tp, unsigned char *macaddr)
{
	int ret;
	unsigned char enetaddr[8] = {0};

	ret = pla_ocp_read(tp, PLA_IDR, 8, enetaddr);
	if (ret < 0)
		return ret;

	memcpy(macaddr, enetaddr, ETH_ALEN);
	return 0;
}

static void r8152b_disable_aldps(struct r8152 *tp)
{
	ocp_reg_write(tp, OCP_ALDPS_CONFIG, ENPDNPS | LINKENA | DIS_SDSAVE);
	mdelay(20);
}

static void r8152b_enable_aldps(struct r8152 *tp)
{
	ocp_reg_write(tp, OCP_ALDPS_CONFIG, ENPWRSAVE | ENPDNPS |
		LINKENA | DIS_SDSAVE);
}

static void rtl8152_disable(struct r8152 *tp)
{
	r8152b_disable_aldps(tp);
	rtl_disable(tp);
	r8152b_enable_aldps(tp);
}

static void r8152b_hw_phy_cfg(struct r8152 *tp)
{
	u16 data;

	data = r8152_mdio_read(tp, MII_BMCR);
	if (data & BMCR_PDOWN) {
		data &= ~BMCR_PDOWN;
		r8152_mdio_write(tp, MII_BMCR, data);
	}

	r8152b_firmware(tp);
}

static void r8156b_wait_loading_flash(struct r8152 *tp)
{
	if (r8156b_flash_used(tp)) {
		int i;

		for (i = 0; i < 100; i++) {
			u32 ocp_data;

			ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_GPHY_CTRL);
			if (ocp_data & GPHY_PATCH_DONE)
				break;
			mdelay(2);
		}
	}
}

static void rtl8152_reinit_ll(struct r8152 *tp)
{
	u32 ocp_data;
	int ret;

	ret = r8152_wait_for_bit(tp, 0, MCU_TYPE_PLA, PLA_PHY_PWR,
				 PLA_PHY_PWR_LLR, 1, R8152_WAIT_TIMEOUT);
	if (ret)
		debug("Timeout waiting for link list ready\n");

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_SFF_STS_7);
	ocp_data |= RE_INIT_LL;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_SFF_STS_7, ocp_data);

	ret = r8152_wait_for_bit(tp, 0, MCU_TYPE_PLA, PLA_PHY_PWR,
				 PLA_PHY_PWR_LLR, 1, R8152_WAIT_TIMEOUT);
	if (ret)
		debug("Timeout waiting for link list ready\n");
}

static void r8152b_exit_oob(struct r8152 *tp)
{
	u32 ocp_data;

	ocp_data = ocp_read_dword(tp, MCU_TYPE_PLA, PLA_RCR);
	ocp_data &= ~RCR_ACPT_ALL;
	ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RCR, ocp_data);

	rxdy_gated_en(tp, true);
	r8152b_hw_phy_cfg(tp);

	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_NORAML);
	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_CR, 0x00);

	ocp_data = ocp_read_byte(tp, MCU_TYPE_PLA, PLA_OOB_CTRL);
	ocp_data &= ~NOW_IS_OOB;
	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_OOB_CTRL, ocp_data);

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_SFF_STS_7);
	ocp_data &= ~MCU_BORW_EN;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_SFF_STS_7, ocp_data);

	rtl8152_reinit_ll(tp);
	rtl8152_nic_reset(tp);

	/* rx share fifo credit full threshold */
	ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL0, RXFIFO_THR1_NORMAL);

	if (tp->udev->speed == USB_SPEED_FULL ||
	    tp->udev->speed == USB_SPEED_LOW) {
		/* rx share fifo credit near full threshold */
		ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL1,
				RXFIFO_THR2_FULL);
		ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL2,
				RXFIFO_THR3_FULL);
	} else {
		/* rx share fifo credit near full threshold */
		ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL1,
				RXFIFO_THR2_HIGH);
		ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL2,
				RXFIFO_THR3_HIGH);
	}

	/* TX share fifo free credit full threshold */
	ocp_write_dword(tp, MCU_TYPE_PLA, PLA_TXFIFO_CTRL, TXFIFO_THR_NORMAL);

	ocp_write_byte(tp, MCU_TYPE_USB, USB_TX_AGG, TX_AGG_MAX_THRESHOLD);
	ocp_write_dword(tp, MCU_TYPE_USB, USB_RX_BUF_TH, RX_THR_HIGH);
	ocp_write_dword(tp, MCU_TYPE_USB, USB_TX_DMA,
			TEST_MODE_DISABLE | TX_SIZE_ADJUST1);

	ocp_write_word(tp, MCU_TYPE_PLA, PLA_RMS, RTL8152_RMS);

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_TCR0);
	ocp_data |= TCR0_AUTO_FIFO;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_TCR0, ocp_data);
}

static void r8152b_enter_oob(struct r8152 *tp)
{
	u32 ocp_data;

	ocp_data = ocp_read_byte(tp, MCU_TYPE_PLA, PLA_OOB_CTRL);
	ocp_data &= ~NOW_IS_OOB;
	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_OOB_CTRL, ocp_data);

	ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL0, RXFIFO_THR1_OOB);
	ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL1, RXFIFO_THR2_OOB);
	ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL2, RXFIFO_THR3_OOB);

	rtl_disable(tp);

	rtl8152_reinit_ll(tp);

	ocp_write_word(tp, MCU_TYPE_PLA, PLA_RMS, RTL8152_RMS);

	rtl_rx_vlan_en(tp, false);

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_BDC_CR);
	ocp_data |= ALDPS_PROXY_MODE;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_BDC_CR, ocp_data);

	ocp_data = ocp_read_byte(tp, MCU_TYPE_PLA, PLA_OOB_CTRL);
	ocp_data |= NOW_IS_OOB | DIS_MCU_CLROOB;
	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_OOB_CTRL, ocp_data);

	rxdy_gated_en(tp, false);

	ocp_data = ocp_read_dword(tp, MCU_TYPE_PLA, PLA_RCR);
	ocp_data |= RCR_APM | RCR_AM | RCR_AB;
	ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RCR, ocp_data);
}

static void r8153b_mcu_spdown_en(struct r8152 *tp, bool enable)
{
	if (enable)
		ocp_word_set_bits(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL3,
				  PLA_MCU_SPDWN_EN);
	else
		ocp_word_clr_bits(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL3,
				  PLA_MCU_SPDWN_EN);
}

static void r8153_hw_phy_cfg(struct r8152 *tp)
{
	u32 ocp_data;
	u16 data;

	if (tp->version == RTL_VER_03 || tp->version == RTL_VER_04 ||
	    tp->version == RTL_VER_05)
		ocp_reg_write(tp, OCP_ADC_CFG, CKADSEL_L | ADC_EN | EN_EMI_L);

	data = r8152_mdio_read(tp, MII_BMCR);
	if (data & BMCR_PDOWN) {
		data &= ~BMCR_PDOWN;
		r8152_mdio_write(tp, MII_BMCR, data);
	}

	r8153_firmware(tp);

	if (tp->version == RTL_VER_03) {
		data = ocp_reg_read(tp, OCP_EEE_CFG);
		data &= ~CTAP_SHORT_EN;
		ocp_reg_write(tp, OCP_EEE_CFG, data);
	}

	data = ocp_reg_read(tp, OCP_POWER_CFG);
	data |= EEE_CLKDIV_EN;
	ocp_reg_write(tp, OCP_POWER_CFG, data);

	data = ocp_reg_read(tp, OCP_DOWN_SPEED);
	data |= EN_10M_BGOFF;
	ocp_reg_write(tp, OCP_DOWN_SPEED, data);
	data = ocp_reg_read(tp, OCP_POWER_CFG);
	data |= EN_10M_PLLOFF;
	ocp_reg_write(tp, OCP_POWER_CFG, data);
	sram_write(tp, SRAM_IMPEDANCE, 0x0b13);

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_PHY_PWR);
	ocp_data |= PFM_PWM_SWITCH;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_PHY_PWR, ocp_data);

	/* Enable LPF corner auto tune */
	sram_write(tp, SRAM_LPF_CFG, 0xf70f);

	/* Adjust 10M Amplitude */
	sram_write(tp, SRAM_10M_AMP1, 0x00af);
	sram_write(tp, SRAM_10M_AMP2, 0x0208);
}

static u32 r8152_efuse_read(struct r8152 *tp, u8 addr)
{
	u32 ocp_data;

	ocp_write_word(tp, MCU_TYPE_PLA, PLA_EFUSE_CMD, EFUSE_READ_CMD | addr);
	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_EFUSE_CMD);
	ocp_data = (ocp_data & EFUSE_DATA_BIT16) << 9;	/* data of bit16 */
	ocp_data |= ocp_read_word(tp, MCU_TYPE_PLA, PLA_EFUSE_DATA);

	return ocp_data;
}

static void r8153_disable_aldps(struct r8152 *tp)
{
	u16 data;

	data = ocp_reg_read(tp, OCP_POWER_CFG);
	data &= ~EN_ALDPS;
	ocp_reg_write(tp, OCP_POWER_CFG, data);
	mdelay(20);
}

static void r8153_aldps_en(struct r8152 *tp, bool enable)
{
	if (enable) {
		ocp_reg_set_bits(tp, OCP_POWER_CFG, EN_ALDPS);
	} else {
		int i;

		ocp_reg_clr_bits(tp, OCP_POWER_CFG, EN_ALDPS);
		for (i = 0; i < 20; i++) {
			mdelay(1);
			if (ocp_read_word(tp, MCU_TYPE_PLA, 0xe000) & 0x0100)
				break;
		}
	}
}

static void r8153b_hw_phy_cfg(struct r8152 *tp)
{
	u32 ocp_data;
	u16 data;

	data = r8152_mdio_read(tp, MII_BMCR);
	if (data & BMCR_PDOWN) {
		data &= ~BMCR_PDOWN;
		r8152_mdio_write(tp, MII_BMCR, data);
	}

	/* U1/U2/L1 idle timer. 500 us */
	ocp_write_word(tp, MCU_TYPE_USB, USB_U1U2_TIMER, 500);

	if (tp->version == RTL_VER_14)
		r8153c_firmware(tp);
	else
		r8153b_firmware(tp);

	data = sram_read(tp, SRAM_GREEN_CFG);
	data |= R_TUNE_EN;
	sram_write(tp, SRAM_GREEN_CFG, data);
	data = ocp_reg_read(tp, OCP_NCTL_CFG);
	data |= PGA_RETURN_EN;
	ocp_reg_write(tp, OCP_NCTL_CFG, data);

	/* ADC Bias Calibration:
	 * read efuse offset 0x7d to get a 17-bit data. Remove the dummy/fake
	 * bit (bit3) to rebuild the real 16-bit data. Write the data to the
	 * ADC ioffset.
	 */
	ocp_data = r8152_efuse_read(tp, 0x7d);
	ocp_data = ((ocp_data & 0x1fff0) >> 1) | (ocp_data & 0x7);
	if (ocp_data != 0xffff)
		ocp_reg_write(tp, OCP_ADC_IOFFSET, ocp_data);

	/* ups mode tx-link-pulse timing adjustment:
	 * rg_saw_cnt = OCP reg 0xC426 Bit[13:0]
	 * swr_cnt_1ms_ini = 16000000 / rg_saw_cnt
	 */
	ocp_data = ocp_reg_read(tp, 0xc426);
	ocp_data &= 0x3fff;
	if (ocp_data) {
		u32 swr_cnt_1ms_ini;

		swr_cnt_1ms_ini = (16000000 / ocp_data) & SAW_CNT_1MS_MASK;
		ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_UPS_CFG);
		ocp_data = (ocp_data & ~SAW_CNT_1MS_MASK) | swr_cnt_1ms_ini;
		ocp_write_word(tp, MCU_TYPE_USB, USB_UPS_CFG, ocp_data);
	}

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_PHY_PWR);
	ocp_data |= PFM_PWM_SWITCH;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_PHY_PWR, ocp_data);
}

static void r8156_hw_phy_cfg(struct r8152 *tp)
{
	u32 ocp_data;
	u16 data;

	r8156_patch_code(tp);

	data = r8152_mdio_read(tp, MII_BMCR);
	if (data & BMCR_PDOWN) {
		data &= ~BMCR_PDOWN;
		r8152_mdio_write(tp, MII_BMCR, data);
	}

	/* U1/U2/L1 idle timer. 500 us */
	ocp_write_word(tp, MCU_TYPE_USB, USB_U1U2_TIMER, 500);

	data = r8153_phy_status(tp, 0);
	switch (data) {
	case PHY_STAT_EXT_INIT:
		r8156_ram_code(tp, true);
		ocp_reg_clr_bits(tp, 0xa468, BIT(3) | BIT(1));
		break;
	case PHY_STAT_LAN_ON:
	case PHY_STAT_PWRDN:
	default:
		r8156_ram_code(tp, false);
		break;
	}

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_PHY_PWR);
	ocp_data &= ~PFM_PWM_SWITCH;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_PHY_PWR, ocp_data);

	switch (tp->version) {
	case RTL_VER_10:
		ocp_reg_w0w1(tp, 0xad40, 0x3ff, BIT(7) | BIT(2));

		ocp_reg_set_bits(tp, 0xad4e, BIT(4));
		ocp_reg_w0w1(tp, 0xad16, 0x3ff, 0x6);
		ocp_reg_w0w1(tp, 0xad32, 0x3f, 6);
		ocp_reg_clr_bits(tp, 0xac08, BIT(12) | BIT(8));
		ocp_reg_w0w1(tp, 0xac8a, BIT(15), BIT(12) | BIT(13) | BIT(14));
		ocp_reg_set_bits(tp, 0xad18, BIT(10));
		ocp_reg_set_bits(tp, 0xad1a, 0x3ff);
		ocp_reg_set_bits(tp, 0xad1c, 0x3ff);

		sram_write_w0w1(tp, 0x80ea, 0xff00, 0xc400);
		sram_write_w0w1(tp, 0x80eb, 0x0700, 0x0300);
		sram_write_w0w1(tp, 0x80f8, 0xff00, 0x1c00);
		sram_write_w0w1(tp, 0x80f1, 0xff00, 0x3000);

		sram_write_w0w1(tp, 0x80fe, 0xff00, 0xa500);
		sram_write_w0w1(tp, 0x8102, 0xff00, 0x5000);
		sram_write_w0w1(tp, 0x8015, 0xff00, 0x3300);
		sram_write_w0w1(tp, 0x8100, 0xff00, 0x7000);
		sram_write_w0w1(tp, 0x8014, 0xff00, 0xf000);
		sram_write_w0w1(tp, 0x8016, 0xff00, 0x6500);
		sram_write_w0w1(tp, 0x80dc, 0xff00, 0xed00);
		sram_set_bits(tp, 0x80df, BIT(8));
		sram_clr_bits(tp, 0x80e1, BIT(8));

		ocp_reg_w0w1(tp, 0xbf06, 0x003f, 0x0038);

		sram_write(tp, 0x819f, 0xddb6);

		ocp_reg_write(tp, 0xbc34, 0x5555);
		ocp_reg_w0w1(tp, 0xbf0a, 0x0e00, 0x0a00);

		ocp_reg_clr_bits(tp, 0xbd2c, BIT(13));
		break;
	case RTL_VER_11:
		/* 2.5G INRX */
		ocp_reg_set_bits(tp, 0xad16, 0x3ff);
		ocp_reg_w0w1(tp, 0xad32, 0x3f, 6);
		ocp_reg_clr_bits(tp, 0xac08, BIT(12) | BIT(8));
		ocp_reg_w0w1(tp, 0xacc0, 0x3, BIT(1));
		ocp_reg_w0w1(tp, 0xad40, 0xe7, BIT(6) | BIT(2));
		ocp_reg_clr_bits(tp, 0xac14, BIT(7));
		ocp_reg_clr_bits(tp, 0xac80, BIT(8) | BIT(9));
		ocp_reg_w0w1(tp, 0xac5e, 0x7, BIT(1));
		ocp_reg_write(tp, 0xad4c, 0x00a8);
		ocp_reg_write(tp, 0xac5c, 0x01ff);
		ocp_reg_w0w1(tp, 0xac8a, 0xf0, BIT(4) | BIT(5));
		ocp_reg_write(tp, 0xb87c, 0x8157);
		ocp_reg_w0w1(tp, 0xb87e, 0xff00, 0x0500);
		ocp_reg_write(tp, 0xb87c, 0x8159);
		ocp_reg_w0w1(tp, 0xb87e, 0xff00, 0x0700);

		/* AAGC */
		ocp_reg_write(tp, 0xb87c, 0x80a2);
		ocp_reg_write(tp, 0xb87e, 0x0153);
		ocp_reg_write(tp, 0xb87c, 0x809c);
		ocp_reg_write(tp, 0xb87e, 0x0153);

		/* EEE parameter */
		ocp_write_word(tp, MCU_TYPE_PLA, PLA_EEE_TXTWSYS_2P5G, 0x0056);

		ocp_word_set_bits(tp, MCU_TYPE_PLA, PLA_USB_CFG,
				  EN_XG_LIP | EN_G_LIP);

		sram_write(tp, 0x8257, 0x020f); /*  XG PLL */
		sram_write(tp, 0x80ea, 0x7843); /* GIGA Master */

		if (rtl_phy_patch_request(tp, true, true))
			return;

		/* Advance EEE */
		ocp_word_clr_bits(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL4,
				  EEE_SPDWN_EN);

		ocp_reg_w0w1(tp, OCP_DOWN_SPEED, EN_EEE_100 | EN_EEE_1000,
			     EN_10M_CLKDIV);

		ocp_reg_clr_bits(tp, OCP_POWER_CFG, EEE_CLKDIV_EN);

		ocp_reg_write(tp, OCP_SYSCLK_CFG, 0);
		ocp_reg_write(tp, OCP_SYSCLK_CFG, sysclk_div_expo(5));

		rtl_phy_patch_request(tp, false, true);

		/* enable ADC Ibias Cal */
		ocp_reg_set_bits(tp, 0xd068, BIT(13));

		/* enable Thermal Sensor */
		sram_clr_bits(tp, 0x81a2, BIT(8));
		ocp_reg_w0w1(tp, 0xb54c, 0xff00, 0xdb00);

		/* Nway 2.5G Lite */
		ocp_reg_clr_bits(tp, 0xa454, BIT(0));

		/* CS DSP solution */
		ocp_reg_set_bits(tp, OCP_10GBT_CTRL, RTL_ADV2_5G_F_R);
		ocp_reg_clr_bits(tp, 0xad4e, BIT(4));
		ocp_reg_clr_bits(tp, 0xa86a, BIT(0));

		/* MDI SWAP */
		ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_UPS_CFG);
		data = ocp_reg_read(tp, 0xd068);
		if ((ocp_data & MID_REVERSE) && (data & BIT(1))) {
			u16 swap_a, swap_b;

			data = ocp_reg_read(tp, 0xd068);
			data &= ~0x1f;
			data |= 0x1; /* p0 */
			ocp_reg_write(tp, 0xd068, data);
			swap_a = ocp_reg_read(tp, 0xd06a);
			data &= ~0x18;
			data |= 0x18; /* p3 */
			ocp_reg_write(tp, 0xd068, data);
			swap_b = ocp_reg_read(tp, 0xd06a);
			data &= ~0x18; /* p0 */
			ocp_reg_write(tp, 0xd068, data);
			ocp_reg_write(tp, 0xd06a,
				      (swap_a & ~0x7ff) | (swap_b & 0x7ff));
			data |= 0x18; /* p3 */
			ocp_reg_write(tp, 0xd068, data);
			ocp_reg_write(tp, 0xd06a,
				      (swap_b & ~0x7ff) | (swap_a & 0x7ff));
			data &= ~0x18;
			data |= 0x08; /* p1 */
			ocp_reg_write(tp, 0xd068, data);
			swap_a = ocp_reg_read(tp, 0xd06a);
			data &= ~0x18;
			data |= 0x10; /* p2 */
			ocp_reg_write(tp, 0xd068, data);
			swap_b = ocp_reg_read(tp, 0xd06a);
			data &= ~0x18;
			data |= 0x08; /* p1 */
			ocp_reg_write(tp, 0xd068, data);
			ocp_reg_write(tp, 0xd06a,
				      (swap_a & ~0x7ff) | (swap_b & 0x7ff));
			data &= ~0x18;
			data |= 0x10; /* p2 */
			ocp_reg_write(tp, 0xd068, data);
			ocp_reg_write(tp, 0xd06a,
				      (swap_b & ~0x7ff) | (swap_a & 0x7ff));
			swap_a = ocp_reg_read(tp, 0xbd5a);
			swap_b = ocp_reg_read(tp, 0xbd5c);
			ocp_reg_write(tp, 0xbd5a, (swap_a & ~0x1f1f) |
				      ((swap_b & 0x1f) << 8) |
				      ((swap_b >> 8) & 0x1f));
			ocp_reg_write(tp, 0xbd5c, (swap_b & ~0x1f1f) |
				      ((swap_a & 0x1f) << 8) |
				      ((swap_a >> 8) & 0x1f));
			swap_a = ocp_reg_read(tp, 0xbc18);
			swap_b = ocp_reg_read(tp, 0xbc1a);
			ocp_reg_write(tp, 0xbc18, (swap_a & ~0x1f1f) |
				      ((swap_b & 0x1f) << 8) |
				      ((swap_b >> 8) & 0x1f));
			ocp_reg_write(tp, 0xbc1a, (swap_b & ~0x1f1f) |
				      ((swap_a & 0x1f) << 8) |
				      ((swap_a >> 8) & 0x1f));
		}

		/* Notify the MAC when the speed is changed to force mode. */
		ocp_reg_set_bits(tp, OCP_INTR_EN, INTR_SPEED_FORCE);
		break;
	default:
		break;
	}

	ocp_reg_clr_bits(tp, 0xa428, BIT(9));
	ocp_reg_clr_bits(tp, 0xa5ea, BIT(0));
}

static void r8156b_hw_phy_cfg(struct r8152 *tp)
{
	u16 data;

	r8156_patch_code(tp);

	switch (tp->version) {
	case RTL_VER_12:
		ocp_reg_write(tp, 0xbf86, 0x9000);
		ocp_reg_set_bits(tp, 0xc402, BIT(10));
		ocp_reg_clr_bits(tp, 0xc402, BIT(10));
		ocp_reg_write(tp, 0xbd86, 0x1010);
		ocp_reg_write(tp, 0xbd88, 0x1010);
		ocp_reg_w0w1(tp, 0xbd4e, BIT(10) | BIT(11), BIT(11));
		ocp_reg_w0w1(tp, 0xbf46, 0xf00, 0x700);
		break;
	case RTL_VER_13:
	case RTL_VER_15:
		r8156b_wait_loading_flash(tp);
		break;
	default:
		break;
	}

	ocp_word_test_and_clr_bits(tp, MCU_TYPE_USB, USB_MISC_0, PCUT_STATUS);

	data = r8153_phy_status(tp, 0);
	switch (data) {
	case PHY_STAT_EXT_INIT:
		r8156_ram_code(tp, true);
		ocp_reg_clr_bits(tp, 0xa466, BIT(0));
		ocp_reg_clr_bits(tp, 0xa468, BIT(3) | BIT(1));
		break;
	case PHY_STAT_LAN_ON:
	case PHY_STAT_PWRDN:
	default:
		r8156_ram_code(tp, false);
		break;
	}

	r8152_mdio_test_and_clr_bit(tp, MII_BMCR, BMCR_PDOWN);

	/* disable ALDPS before updating the PHY parameters */
	r8153_disable_aldps(tp);

	ocp_word_clr_bits(tp, MCU_TYPE_PLA, PLA_PHY_PWR, PFM_PWM_SWITCH);

	switch (tp->version) {
	case RTL_VER_12:
		ocp_reg_set_bits(tp, 0xbc08, BIT(3) | BIT(2));

		sram_write_w0w1(tp, 0x8fff, 0xff00, 0x0400);

		ocp_reg_set_bits(tp, 0xacda, 0xff00);
		ocp_reg_set_bits(tp, 0xacde, 0xf000);
		ocp_reg_write(tp, 0xac8c, 0x0ffc);
		ocp_reg_write(tp, 0xac46, 0xb7b4);
		ocp_reg_write(tp, 0xac50, 0x0fbc);
		ocp_reg_write(tp, 0xac3c, 0x9240);
		ocp_reg_write(tp, 0xac4e, 0x0db4);
		ocp_reg_write(tp, 0xacc6, 0x0707);
		ocp_reg_write(tp, 0xacc8, 0xa0d3);
		ocp_reg_write(tp, 0xad08, 0x0007);

		ocp_reg_write(tp, 0xb87c, 0x8560);
		ocp_reg_write(tp, 0xb87e, 0x19cc);
		ocp_reg_write(tp, 0xb87c, 0x8562);
		ocp_reg_write(tp, 0xb87e, 0x19cc);
		ocp_reg_write(tp, 0xb87c, 0x8564);
		ocp_reg_write(tp, 0xb87e, 0x19cc);
		ocp_reg_write(tp, 0xb87c, 0x8566);
		ocp_reg_write(tp, 0xb87e, 0x147d);
		ocp_reg_write(tp, 0xb87c, 0x8568);
		ocp_reg_write(tp, 0xb87e, 0x147d);
		ocp_reg_write(tp, 0xb87c, 0x856a);
		ocp_reg_write(tp, 0xb87e, 0x147d);
		ocp_reg_write(tp, 0xb87c, 0x8ffe);
		ocp_reg_write(tp, 0xb87e, 0x0907);
		ocp_reg_write(tp, 0xb87c, 0x80d6);
		ocp_reg_write(tp, 0xb87e, 0x2801);
		ocp_reg_write(tp, 0xb87c, 0x80f2);
		ocp_reg_write(tp, 0xb87e, 0x2801);
		ocp_reg_write(tp, 0xb87c, 0x80f4);
		ocp_reg_write(tp, 0xb87e, 0x6077);
		ocp_reg_write(tp, 0xb506, 0x01e7);

		ocp_reg_write(tp, 0xb87c, 0x8013);
		ocp_reg_write(tp, 0xb87e, 0x0700);
		ocp_reg_write(tp, 0xb87c, 0x8fb9);
		ocp_reg_write(tp, 0xb87e, 0x2801);
		ocp_reg_write(tp, 0xb87c, 0x8fba);
		ocp_reg_write(tp, 0xb87e, 0x0100);
		ocp_reg_write(tp, 0xb87c, 0x8fbc);
		ocp_reg_write(tp, 0xb87e, 0x1900);
		ocp_reg_write(tp, 0xb87c, 0x8fbe);
		ocp_reg_write(tp, 0xb87e, 0xe100);
		ocp_reg_write(tp, 0xb87c, 0x8fc0);
		ocp_reg_write(tp, 0xb87e, 0x0800);
		ocp_reg_write(tp, 0xb87c, 0x8fc2);
		ocp_reg_write(tp, 0xb87e, 0xe500);
		ocp_reg_write(tp, 0xb87c, 0x8fc4);
		ocp_reg_write(tp, 0xb87e, 0x0f00);
		ocp_reg_write(tp, 0xb87c, 0x8fc6);
		ocp_reg_write(tp, 0xb87e, 0xf100);
		ocp_reg_write(tp, 0xb87c, 0x8fc8);
		ocp_reg_write(tp, 0xb87e, 0x0400);
		ocp_reg_write(tp, 0xb87c, 0x8fca);
		ocp_reg_write(tp, 0xb87e, 0xf300);
		ocp_reg_write(tp, 0xb87c, 0x8fcc);
		ocp_reg_write(tp, 0xb87e, 0xfd00);
		ocp_reg_write(tp, 0xb87c, 0x8fce);
		ocp_reg_write(tp, 0xb87e, 0xff00);
		ocp_reg_write(tp, 0xb87c, 0x8fd0);
		ocp_reg_write(tp, 0xb87e, 0xfb00);
		ocp_reg_write(tp, 0xb87c, 0x8fd2);
		ocp_reg_write(tp, 0xb87e, 0x0100);
		ocp_reg_write(tp, 0xb87c, 0x8fd4);
		ocp_reg_write(tp, 0xb87e, 0xf400);
		ocp_reg_write(tp, 0xb87c, 0x8fd6);
		ocp_reg_write(tp, 0xb87e, 0xff00);
		ocp_reg_write(tp, 0xb87c, 0x8fd8);
		ocp_reg_write(tp, 0xb87e, 0xf600);

		ocp_byte_set_bits(tp, MCU_TYPE_PLA, PLA_USB_CFG,
				  EN_XG_LIP | EN_G_LIP);
		ocp_reg_write(tp, 0xb87c, 0x813d);
		ocp_reg_write(tp, 0xb87e, 0x390e);
		ocp_reg_write(tp, 0xb87c, 0x814f);
		ocp_reg_write(tp, 0xb87e, 0x790e);
		ocp_reg_write(tp, 0xb87c, 0x80b0);
		ocp_reg_write(tp, 0xb87e, 0x0f31);
		ocp_reg_set_bits(tp, 0xbf4c, BIT(1));
		ocp_reg_set_bits(tp, 0xbcca, BIT(9) | BIT(8));
		ocp_reg_write(tp, 0xb87c, 0x8141);
		ocp_reg_write(tp, 0xb87e, 0x320e);
		ocp_reg_write(tp, 0xb87c, 0x8153);
		ocp_reg_write(tp, 0xb87e, 0x720e);
		ocp_reg_write(tp, 0xb87c, 0x8529);
		ocp_reg_write(tp, 0xb87e, 0x050e);
		ocp_reg_clr_bits(tp, OCP_EEE_CFG, CTAP_SHORT_EN);

		sram_write(tp, 0x816c, 0xc4a0);
		sram_write(tp, 0x8170, 0xc4a0);
		sram_write(tp, 0x8174, 0x04a0);
		sram_write(tp, 0x8178, 0x04a0);
		sram_write(tp, 0x817c, 0x0719);
		sram_write(tp, 0x8ff4, 0x0400);
		sram_write(tp, 0x8ff1, 0x0404);

		ocp_reg_write(tp, 0xbf4a, 0x001b);
		ocp_reg_write(tp, 0xb87c, 0x8033);
		ocp_reg_write(tp, 0xb87e, 0x7c13);
		ocp_reg_write(tp, 0xb87c, 0x8037);
		ocp_reg_write(tp, 0xb87e, 0x7c13);
		ocp_reg_write(tp, 0xb87c, 0x803b);
		ocp_reg_write(tp, 0xb87e, 0xfc32);
		ocp_reg_write(tp, 0xb87c, 0x803f);
		ocp_reg_write(tp, 0xb87e, 0x7c13);
		ocp_reg_write(tp, 0xb87c, 0x8043);
		ocp_reg_write(tp, 0xb87e, 0x7c13);
		ocp_reg_write(tp, 0xb87c, 0x8047);
		ocp_reg_write(tp, 0xb87e, 0x7c13);

		ocp_reg_write(tp, 0xb87c, 0x8145);
		ocp_reg_write(tp, 0xb87e, 0x370e);
		ocp_reg_write(tp, 0xb87c, 0x8157);
		ocp_reg_write(tp, 0xb87e, 0x770e);
		ocp_reg_write(tp, 0xb87c, 0x8169);
		ocp_reg_write(tp, 0xb87e, 0x0d0a);
		ocp_reg_write(tp, 0xb87c, 0x817b);
		ocp_reg_write(tp, 0xb87e, 0x1d0a);

		sram_write_w0w1(tp, 0x8217, 0xff00, 0x5000);
		sram_write_w0w1(tp, 0x821a, 0xff00, 0x5000);
		sram_write(tp, 0x80da, 0x0403);
		sram_write_w0w1(tp, 0x80dc, 0xff00, 0x1000);
		sram_write(tp, 0x80b3, 0x0384);
		sram_write(tp, 0x80b7, 0x2007);
		sram_write_w0w1(tp, 0x80ba, 0xff00, 0x6c00);
		sram_write(tp, 0x80b5, 0xf009);
		sram_write_w0w1(tp, 0x80bd, 0xff00, 0x9f00);
		sram_write(tp, 0x80c7, 0xf083);
		sram_write(tp, 0x80dd, 0x03f0);
		sram_write_w0w1(tp, 0x80df, 0xff00, 0x1000);
		sram_write(tp, 0x80cb, 0x2007);
		sram_write_w0w1(tp, 0x80ce, 0xff00, 0x6c00);
		sram_write(tp, 0x80c9, 0x8009);
		sram_write_w0w1(tp, 0x80d1, 0xff00, 0x8000);
		sram_write(tp, 0x80a3, 0x200a);
		sram_write(tp, 0x80a5, 0xf0ad);
		sram_write(tp, 0x809f, 0x6073);
		sram_write(tp, 0x80a1, 0x000b);
		sram_write_w0w1(tp, 0x80a9, 0xff00, 0xc000);

		if (rtl_phy_patch_request(tp, true, true))
			return;

		ocp_reg_clr_bits(tp, 0xb896, BIT(0));
		ocp_reg_clr_bits(tp, 0xb892, 0xff00);
		ocp_reg_write(tp, 0xb88e, 0xc23e);
		ocp_reg_write(tp, 0xb890, 0x0000);
		ocp_reg_write(tp, 0xb88e, 0xc240);
		ocp_reg_write(tp, 0xb890, 0x0103);
		ocp_reg_write(tp, 0xb88e, 0xc242);
		ocp_reg_write(tp, 0xb890, 0x0507);
		ocp_reg_write(tp, 0xb88e, 0xc244);
		ocp_reg_write(tp, 0xb890, 0x090b);
		ocp_reg_write(tp, 0xb88e, 0xc246);
		ocp_reg_write(tp, 0xb890, 0x0c0e);
		ocp_reg_write(tp, 0xb88e, 0xc248);
		ocp_reg_write(tp, 0xb890, 0x1012);
		ocp_reg_write(tp, 0xb88e, 0xc24a);
		ocp_reg_write(tp, 0xb890, 0x1416);
		ocp_reg_set_bits(tp, 0xb896, BIT(0));

		rtl_phy_patch_request(tp, false, true);

		ocp_reg_set_bits(tp, 0xa86a, BIT(0));
		ocp_reg_set_bits(tp, 0xa6f0, BIT(0));

		ocp_reg_write(tp, 0xbfa0, 0xd70d);
		ocp_reg_write(tp, 0xbfa2, 0x4100);
		ocp_reg_write(tp, 0xbfa4, 0xe868);
		ocp_reg_write(tp, 0xbfa6, 0xdc59);
		ocp_reg_write(tp, 0xb54c, 0x3c18);
		ocp_reg_clr_bits(tp, 0xbfa4, BIT(5));
		sram_set_bits(tp, 0x817d, BIT(12));
		break;
	case RTL_VER_13:
		/* 2.5G INRX */
		ocp_reg_w0w1(tp, 0xac46, 0x00f0, 0x0090);
		ocp_reg_w0w1(tp, 0xad30, 0x0003, 0x0001);
		fallthrough;
	case RTL_VER_15:
		/* EEE parameter */
		ocp_reg_write(tp, 0xb87c, 0x80f5);
		ocp_reg_write(tp, 0xb87e, 0x760e);
		ocp_reg_write(tp, 0xb87c, 0x8107);
		ocp_reg_write(tp, 0xb87e, 0x360e);
		ocp_reg_write(tp, 0xb87c, 0x8551);
		ocp_reg_w0w1(tp, 0xb87e, 0xff00, 0x0800);

		/* ADC_PGA parameter */
		ocp_reg_w0w1(tp, 0xbf00, 0xe000, 0xa000);
		ocp_reg_w0w1(tp, 0xbf46, 0x0f00, 0x0300);

		/* Green Table-PGA, 1G full viterbi */
		sram_write(tp, 0x8044, 0x2417);
		sram_write(tp, 0x804a, 0x2417);
		sram_write(tp, 0x8050, 0x2417);
		sram_write(tp, 0x8056, 0x2417);
		sram_write(tp, 0x805c, 0x2417);
		sram_write(tp, 0x8062, 0x2417);
		sram_write(tp, 0x8068, 0x2417);
		sram_write(tp, 0x806e, 0x2417);
		sram_write(tp, 0x8074, 0x2417);
		sram_write(tp, 0x807a, 0x2417);

		/* Nway DACONB parameter */
		ocp_reg_w0w1(tp, 0xa4ca, 0x6000, 0x0040);

		/* XG PLL */
		ocp_reg_w0w1(tp, 0xbf84, 0xe000, 0xa000);
		break;
	default:
		break;
	}

	/* Notify the MAC when the speed is changed to force mode. */
	ocp_reg_set_bits(tp, OCP_INTR_EN, INTR_SPEED_FORCE);

	if (rtl_phy_patch_request(tp, true, true))
		return;

	ocp_word_clr_bits(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL4, EEE_SPDWN_EN);

	ocp_reg_w0w1(tp, OCP_DOWN_SPEED, EN_EEE_100 | EN_EEE_1000,
		     EN_10M_CLKDIV);

	ocp_reg_clr_bits(tp, OCP_POWER_CFG, EEE_CLKDIV_EN);

	rtl_phy_patch_request(tp, false, true);

	ocp_reg_clr_bits(tp, 0xa428, BIT(9));
	ocp_reg_clr_bits(tp, 0xa5ea, BIT(0));
}

static void r8153_first_init(struct r8152 *tp)
{
	u32 ocp_data;

	rxdy_gated_en(tp, true);

	ocp_data = ocp_read_dword(tp, MCU_TYPE_PLA, PLA_RCR);
	ocp_data &= ~RCR_ACPT_ALL;
	ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RCR, ocp_data);

	r8153_hw_phy_cfg(tp);

	rtl8152_nic_reset(tp);
	rtl_reset_bmu(tp);

	ocp_data = ocp_read_byte(tp, MCU_TYPE_PLA, PLA_OOB_CTRL);
	ocp_data &= ~NOW_IS_OOB;
	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_OOB_CTRL, ocp_data);

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_SFF_STS_7);
	ocp_data &= ~MCU_BORW_EN;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_SFF_STS_7, ocp_data);

	rtl8152_reinit_ll(tp);

	rtl_rx_vlan_en(tp, false);

	ocp_data = RTL8153_RMS;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_RMS, ocp_data);
	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_MTPS, MTPS_JUMBO);

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_TCR0);
	ocp_data |= TCR0_AUTO_FIFO;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_TCR0, ocp_data);

	rtl8152_nic_reset(tp);

	/* rx share fifo credit full threshold */
	if (tp->version == RTL_VER_14) {
		ocp_write_byte(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL0, 0x02);
		ocp_write_byte(tp, MCU_TYPE_PLA, PLA_RXFIFO_FULL, 0x08);
		ocp_write_word(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL1, RXFIFO_THR2_NORMAL);
		ocp_write_word(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL2, RXFIFO_THR3_NORMAL);
		/* TX share fifo free credit full threshold */
		ocp_write_word(tp, MCU_TYPE_PLA, PLA_TXFIFO_CTRL, 512 / 64);
		ocp_write_word(tp, MCU_TYPE_PLA, PLA_TXFIFO_FULL, 2048 / 8);
	} else {
		ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL0, RXFIFO_THR1_NORMAL);
		ocp_write_word(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL1, RXFIFO_THR2_NORMAL);
		ocp_write_word(tp, MCU_TYPE_PLA, PLA_RXFIFO_CTRL2, RXFIFO_THR3_NORMAL);
		/* TX share fifo free credit full threshold */
		ocp_write_dword(tp, MCU_TYPE_PLA, PLA_TXFIFO_CTRL, TXFIFO_THR_NORMAL2);
	}

	/* rx aggregation */
	ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_USB_CTRL);

	ocp_data &= ~(RX_AGG_DISABLE | RX_ZERO_EN);
	ocp_write_word(tp, MCU_TYPE_USB, USB_USB_CTRL, ocp_data);
}

static void r8153_enter_oob(struct r8152 *tp)
{
	u32 ocp_data;

	ocp_data = ocp_read_byte(tp, MCU_TYPE_PLA, PLA_OOB_CTRL);
	ocp_data &= ~NOW_IS_OOB;
	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_OOB_CTRL, ocp_data);

	rtl_disable(tp);
	rtl_reset_bmu(tp);

	rtl8152_reinit_ll(tp);

	ocp_data = RTL8153_RMS;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_RMS, ocp_data);

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_TEREDO_CFG);
	ocp_data &= ~TEREDO_WAKE_MASK;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_TEREDO_CFG, ocp_data);

	rtl_rx_vlan_en(tp, false);

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_BDC_CR);
	ocp_data |= ALDPS_PROXY_MODE;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_BDC_CR, ocp_data);

	ocp_data = ocp_read_byte(tp, MCU_TYPE_PLA, PLA_OOB_CTRL);
	ocp_data |= NOW_IS_OOB | DIS_MCU_CLROOB;
	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_OOB_CTRL, ocp_data);

	rxdy_gated_en(tp, false);

	ocp_data = ocp_read_dword(tp, MCU_TYPE_PLA, PLA_RCR);
	ocp_data |= RCR_APM | RCR_AM | RCR_AB;
	ocp_write_dword(tp, MCU_TYPE_PLA, PLA_RCR, ocp_data);
}

/*
 * Ported from Linux drivers/net/usb/r8152.c rtl8156_change_mtu().  PLA_RMS is
 * the MAC's receive maximum size; without it the chip keeps whatever the reset
 * default is and silently drops full-size frames, which looks like a link that
 * comes up and passes nothing.
 */
static void rtl8156_change_mtu(struct r8152 *tp)
{
	u32 rx_max_size = RTL8156_RMS;

	ocp_write_word(tp, MCU_TYPE_PLA, PLA_RMS, rx_max_size);
	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_MTPS, MTPS_JUMBO);
	r8156_fc_parameter(tp);

	/* TX share fifo free credit full threshold */
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_TXFIFO_CTRL, 512 / 64);
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_TXFIFO_FULL,
		       ALIGN(rx_max_size + sizeof(struct tx_desc), 1024) / 16);
}

static void r8156_exit_oob(struct r8152 *tp)
{
	rxdy_gated_en(tp, true);
	r8153_teredo_off(tp);

	ocp_dword_clr_bits(tp, MCU_TYPE_PLA, PLA_RCR, RCR_ACPT_ALL);

	rtl8152_nic_reset(tp);
	rtl_reset_bmu(tp);

	ocp_byte_clr_bits(tp, MCU_TYPE_PLA, PLA_OOB_CTRL, NOW_IS_OOB);

	ocp_word_clr_bits(tp, MCU_TYPE_PLA, PLA_SFF_STS_7, MCU_BORW_EN);

	rtl_rx_vlan_en(tp, false);

	rtl8156_change_mtu(tp);

	switch (tp->version) {
	case RTL_TEST_01:
	case RTL_VER_10:
	case RTL_VER_11:
		ocp_word_set_bits(tp, MCU_TYPE_USB, USB_BMU_CONFIG,
				  ACT_ODMA);
		break;
	default:
		break;
	}

	/* share FIFO settings */
	ocp_word_w0w1(tp, MCU_TYPE_PLA, PLA_RXFIFO_FULL, RXFIFO_FULL_MASK,
		      0x08);

	r8153b_mcu_spdown_en(tp, false);

	ocp_word_clr_bits(tp, MCU_TYPE_USB, USB_SPEED_OPTION,
			RG_PWRDN_EN | ALL_SPEED_OFF);

	ocp_write_dword(tp, MCU_TYPE_USB, USB_RX_BUF_TH, 0x00600400);

	ocp_word_clr_bits(tp, MCU_TYPE_USB, USB_USB_CTRL,
			  RX_AGG_DISABLE | RX_ZERO_EN);
}

static void r8156_enter_oob(struct r8152 *tp)
{
	ocp_byte_clr_bits(tp, MCU_TYPE_PLA, PLA_OOB_CTRL, NOW_IS_OOB);

	/* RX FIFO settings for OOB */
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_RXFIFO_FULL, 64 / 16);
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_RX_FIFO_FULL, 1024 / 16);
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_RX_FIFO_EMPTY, 4096 / 16);

	rtl_disable(tp);
	rtl_reset_bmu(tp);

	ocp_word_set_bits(tp, MCU_TYPE_PLA, PLA_BDC_CR, ALDPS_PROXY_MODE);

	ocp_byte_set_bits(tp, MCU_TYPE_PLA, PLA_OOB_CTRL,
			NOW_IS_OOB | DIS_MCU_CLROOB);

	ocp_word_set_bits(tp, MCU_TYPE_PLA, PLA_SFF_STS_7, MCU_BORW_EN);

	rtl_rx_vlan_en(tp, false);

	rxdy_gated_en(tp, false);

	ocp_dword_set_bits(tp, MCU_TYPE_PLA, PLA_RCR,
			   RCR_APM | RCR_AM | RCR_AB);
}

static void rtl8153_disable(struct r8152 *tp)
{
	r8153_disable_aldps(tp);
	rtl_disable(tp);
	rtl_reset_bmu(tp);
}

static void rtl8156_disable(struct r8152 *tp)
{
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_RX_FIFO_FULL, 0);
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_RX_FIFO_EMPTY, 0);

	rtl8153_disable(tp);
}

static u16 rtl8152_get_support_speed(struct r8152 *tp)
{
	switch (tp->version) {
	case RTL_TEST_01:
	case RTL_VER_10:
	case RTL_VER_11:
	case RTL_VER_12:
	case RTL_VER_13:
		return SPEED_2500;
	default:
		if (tp->supports_gmii)
			return SPEED_1000;
		else
			return SPEED_100;
	}
}

static int rtl8152_set_speed(struct r8152 *tp, u8 autoneg, u16 speed, u8 duplex)
{
	u16 bmcr, anar, gbcr, gbcr2;

	anar = r8152_mdio_read(tp, MII_ADVERTISE);
	anar &= ~(ADVERTISE_10HALF | ADVERTISE_10FULL |
		  ADVERTISE_100HALF | ADVERTISE_100FULL);
	if (tp->supports_gmii) {
		gbcr = r8152_mdio_read(tp, MII_CTRL1000);
		gbcr &= ~(ADVERTISE_1000FULL | ADVERTISE_1000HALF);
	} else {
		gbcr = 0;
	}

	if (tp->supports_xgmii) {
		gbcr2 = ocp_reg_read(tp, OCP_10GBT_CTRL);
		gbcr2 &= ~(MDIO_AN_10GBT_CTRL_ADV2_5G |
			   MDIO_AN_10GBT_CTRL_ADV5G);
	} else {
		gbcr2 = 0;
	}

	if (autoneg == AUTONEG_DISABLE) {
		if (speed == SPEED_10) {
			bmcr = 0;
			anar |= ADVERTISE_10HALF | ADVERTISE_10FULL;
		} else if (speed == SPEED_100) {
			bmcr = BMCR_SPEED100;
			anar |= ADVERTISE_100HALF | ADVERTISE_100FULL;
		} else if (speed == SPEED_1000 &&
			   rtl8152_get_support_speed(tp) >= SPEED_1000) {
			bmcr = BMCR_SPEED1000;
			gbcr |= ADVERTISE_1000FULL | ADVERTISE_1000HALF;
		} else {
			return -EINVAL;
		}

		if (duplex == DUPLEX_FULL)
			bmcr |= BMCR_FULLDPLX;
	} else {
		if (speed == SPEED_10) {
			if (duplex == DUPLEX_FULL)
				anar |= ADVERTISE_10HALF | ADVERTISE_10FULL;
			else
				anar |= ADVERTISE_10HALF;
		} else if (speed == SPEED_100) {
			if (duplex == DUPLEX_FULL) {
				anar |= ADVERTISE_10HALF | ADVERTISE_10FULL;
				anar |= ADVERTISE_100HALF | ADVERTISE_100FULL;
			} else {
				anar |= ADVERTISE_10HALF;
				anar |= ADVERTISE_100HALF;
			}
		} else if (speed == SPEED_1000 &&
			   rtl8152_get_support_speed(tp) >= SPEED_1000) {
			if (duplex == DUPLEX_FULL) {
				anar |= ADVERTISE_10HALF | ADVERTISE_10FULL;
				anar |= ADVERTISE_100HALF | ADVERTISE_100FULL;
				gbcr |= ADVERTISE_1000FULL | ADVERTISE_1000HALF;
			} else {
				anar |= ADVERTISE_10HALF;
				anar |= ADVERTISE_100HALF;
				gbcr |= ADVERTISE_1000HALF;
			}
		} else if (speed == SPEED_2500 &&
			   rtl8152_get_support_speed(tp) >= SPEED_2500) {
			if (duplex == DUPLEX_FULL) {
				anar |= ADVERTISE_10HALF | ADVERTISE_10FULL;
				anar |= ADVERTISE_100HALF | ADVERTISE_100FULL;
				gbcr |= ADVERTISE_1000FULL | ADVERTISE_1000HALF;
				gbcr2 |= MDIO_AN_10GBT_CTRL_ADV2_5G;
			} else {
				anar |= ADVERTISE_10HALF;
				anar |= ADVERTISE_100HALF;
				gbcr |= ADVERTISE_1000HALF;
			}
		} else {
			return -EINVAL;
		}

		bmcr = BMCR_ANENABLE | BMCR_ANRESTART | BMCR_RESET;
	}

	if (tp->supports_gmii)
		r8152_mdio_write(tp, MII_CTRL1000, gbcr);

	if (tp->supports_xgmii)
		ocp_reg_write(tp, OCP_10GBT_CTRL, gbcr2);

	r8152_mdio_write(tp, MII_ADVERTISE, anar);
	r8152_mdio_write(tp, MII_BMCR, bmcr);

	return 0;
}

static void rtl8152_up(struct r8152 *tp)
{
	r8152b_disable_aldps(tp);
	r8152b_exit_oob(tp);
	r8152b_enable_aldps(tp);
}

static void rtl8152_down(struct r8152 *tp)
{
	r8152_power_cut_en(tp, false);
	r8152b_disable_aldps(tp);
	r8152b_enter_oob(tp);
	r8152b_enable_aldps(tp);
}

static void rtl8153_up(struct r8152 *tp)
{
	r8153_u1u2en(tp, false);
	r8153_disable_aldps(tp);
	r8153_first_init(tp);
	r8153_u2p3en(tp, false);
}

static void rtl8153_down(struct r8152 *tp)
{
	r8153_u1u2en(tp, false);
	r8153_u2p3en(tp, false);
	r8153_power_cut_en(tp, false);
	r8153_disable_aldps(tp);
	r8153_enter_oob(tp);
}

static void rtl8153b_up(struct r8152 *tp)
{
	r8153_first_init(tp);
}

static void rtl8153b_down(struct r8152 *tp)
{
	r8153_enter_oob(tp);
}

static void rtl8153c_up(struct r8152 *tp)
{
	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_CONFIG);
	ocp_word_set_bits(tp, MCU_TYPE_PLA, PLA_CONFIG34, BIT(8));
	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_NORAML);

	rtl8153b_up(tp);
}

static void rtl8156_up(struct r8152 *tp)
{
	r8153b_u1u2en(tp, false);
	r8153_u2p3en(tp, false);
	r8153_aldps_en(tp, false);

	r8156_exit_oob(tp);

	r8153_aldps_en(tp, true);
	r8153_u2p3en(tp, true);
	if (tp->udev->speed >= USB_SPEED_SUPER)
		r8153b_u1u2en(tp, true);
}

static void rtl8156_down(struct r8152 *tp)
{
	r8156_enter_oob(tp);
}

static void r8156_mdio_force_mode(struct r8152 *tp)
{
	/* Select force mode through 0xa5b4 bit 15
	 * 0: MDIO force mode
	 * 1: MMD force mode
	 */
	ocp_reg_clr_bits(tp, 0xa5b4, BIT(15));
}

static void r8152b_get_version(struct r8152 *tp)
{
	u32 ocp_data;
	u16 tcr;
	int i;

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_TCR1);
	tcr = (u16)(ocp_data & VERSION_MASK);

	for (i = 0; i < ARRAY_SIZE(r8152_versions); i++) {
		if (tcr == r8152_versions[i].tcr) {
			/* Found a supported version */
			tp->version = r8152_versions[i].version;
			tp->supports_gmii = r8152_versions[i].supports_gmii;
			tp->supports_xgmii = r8152_versions[i].supports_xgmii;
			break;
		}
	}

	if (tp->version == RTL_VER_UNKNOWN)
		printf("r8152: unknown chip, TCR version 0x%04x\n", tcr);
}

static void r8152b_enable_fc(struct r8152 *tp)
{
	u16 anar;
	anar = r8152_mdio_read(tp, MII_ADVERTISE);
	anar |= ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM;
	r8152_mdio_write(tp, MII_ADVERTISE, anar);
}

static void rtl_tally_reset(struct r8152 *tp)
{
	u32 ocp_data;

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_RSTTALLY);
	ocp_data |= TALLY_RESET;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_RSTTALLY, ocp_data);
}

static void r8152b_init(struct r8152 *tp)
{
	u32 ocp_data;

	r8152b_disable_aldps(tp);

	if (tp->version == RTL_VER_01) {
		ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_LED_FEATURE);
		ocp_data &= ~LED_MODE_MASK;
		ocp_write_word(tp, MCU_TYPE_PLA, PLA_LED_FEATURE, ocp_data);
	}

	r8152_power_cut_en(tp, false);

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_PHY_PWR);
	ocp_data |= TX_10M_IDLE_EN | PFM_PWM_SWITCH;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_PHY_PWR, ocp_data);
	ocp_data = ocp_read_dword(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL);
	ocp_data &= ~MCU_CLK_RATIO_MASK;
	ocp_data |= MCU_CLK_RATIO | D3_CLK_GATED_EN;
	ocp_write_dword(tp, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL, ocp_data);
	ocp_data = GPHY_STS_MSK | SPEED_DOWN_MSK |
		   SPDWN_RXDV_MSK | SPDWN_LINKCHG_MSK;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_GPHY_INTR_IMR, ocp_data);

	ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_USB_TIMER);
	ocp_data |= BIT(15);
	ocp_write_word(tp, MCU_TYPE_USB, USB_USB_TIMER, ocp_data);
	ocp_write_word(tp, MCU_TYPE_USB, 0xcbfc, 0x03e8);
	ocp_data &= ~BIT(15);
	ocp_write_word(tp, MCU_TYPE_USB, USB_USB_TIMER, ocp_data);

	r8152b_enable_fc(tp);
	rtl_tally_reset(tp);

	/* enable rx aggregation */
	ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_USB_CTRL);

	ocp_data &= ~(RX_AGG_DISABLE | RX_ZERO_EN);
	ocp_write_word(tp, MCU_TYPE_USB, USB_USB_CTRL, ocp_data);
}

static void r8153_init(struct r8152 *tp)
{
	int i;
	u32 ocp_data;

	r8153_disable_aldps(tp);
	r8153_u1u2en(tp, false);

	r8152_wait_for_bit(tp, 0, MCU_TYPE_PLA, PLA_BOOT_CTRL,
			   AUTOLOAD_DONE, 1, R8152_WAIT_TIMEOUT);

	for (i = 0; i < R8152_WAIT_TIMEOUT; i++) {
		ocp_data = ocp_reg_read(tp, OCP_PHY_STATUS) & PHY_STAT_MASK;
		if (ocp_data == PHY_STAT_LAN_ON || ocp_data == PHY_STAT_PWRDN)
			break;

		mdelay(1);
	}

	r8153_u2p3en(tp, false);

	if (tp->version == RTL_VER_04) {
		ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_SSPHYLINK2);
		ocp_data &= ~pwd_dn_scale_mask;
		ocp_data |= pwd_dn_scale(96);
		ocp_write_word(tp, MCU_TYPE_USB, USB_SSPHYLINK2, ocp_data);

		ocp_data = ocp_read_byte(tp, MCU_TYPE_USB, USB_USB2PHY);
		ocp_data |= USB2PHY_L1 | USB2PHY_SUSPEND;
		ocp_write_byte(tp, MCU_TYPE_USB, USB_USB2PHY, ocp_data);
	} else if (tp->version == RTL_VER_05) {
		ocp_data = ocp_read_byte(tp, MCU_TYPE_PLA, PLA_DMY_REG0);
		ocp_data &= ~ECM_ALDPS;
		ocp_write_byte(tp, MCU_TYPE_PLA, PLA_DMY_REG0, ocp_data);

		ocp_data = ocp_read_byte(tp, MCU_TYPE_USB, USB_CSR_DUMMY1);
		if (ocp_read_word(tp, MCU_TYPE_USB, USB_BURST_SIZE) == 0)
			ocp_data &= ~DYNAMIC_BURST;
		else
			ocp_data |= DYNAMIC_BURST;
		ocp_write_byte(tp, MCU_TYPE_USB, USB_CSR_DUMMY1, ocp_data);
	} else if (tp->version == RTL_VER_06) {
		ocp_data = ocp_read_byte(tp, MCU_TYPE_USB, USB_CSR_DUMMY1);
		if (ocp_read_word(tp, MCU_TYPE_USB, USB_BURST_SIZE) == 0)
			ocp_data &= ~DYNAMIC_BURST;
		else
			ocp_data |= DYNAMIC_BURST;
		ocp_write_byte(tp, MCU_TYPE_USB, USB_CSR_DUMMY1, ocp_data);
	}

	ocp_data = ocp_read_byte(tp, MCU_TYPE_USB, USB_CSR_DUMMY2);
	ocp_data |= EP4_FULL_FC;
	ocp_write_byte(tp, MCU_TYPE_USB, USB_CSR_DUMMY2, ocp_data);

	ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_WDT11_CTRL);
	ocp_data &= ~TIMER11_EN;
	ocp_write_word(tp, MCU_TYPE_USB, USB_WDT11_CTRL, ocp_data);

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_LED_FEATURE);
	ocp_data &= ~LED_MODE_MASK;
	ocp_write_word(tp, MCU_TYPE_PLA, PLA_LED_FEATURE, ocp_data);

	ocp_data = FIFO_EMPTY_1FB | ROK_EXIT_LPM;
	if (tp->version == RTL_VER_04 && tp->udev->speed != USB_SPEED_SUPER)
		ocp_data |= LPM_TIMER_500MS;
	else
		ocp_data |= LPM_TIMER_500US;
	ocp_write_byte(tp, MCU_TYPE_USB, USB_LPM_CTRL, ocp_data);

	ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_AFE_CTRL2);
	ocp_data &= ~SEN_VAL_MASK;
	ocp_data |= SEN_VAL_NORMAL | SEL_RXIDLE;
	ocp_write_word(tp, MCU_TYPE_USB, USB_AFE_CTRL2, ocp_data);

	ocp_write_word(tp, MCU_TYPE_USB, USB_CONNECT_TIMER, 0x0001);

	r8153_power_cut_en(tp, false);

	r8152b_enable_fc(tp);
	rtl_tally_reset(tp);
}

static void r8153b_init(struct r8152 *tp)
{
	u32 ocp_data;
	int i;

	r8153_disable_aldps(tp);
	r8153b_u1u2en(tp, false);

	r8152_wait_for_bit(tp, 0, MCU_TYPE_PLA, PLA_BOOT_CTRL,
			   AUTOLOAD_DONE, 1, R8152_WAIT_TIMEOUT);

	for (i = 0; i < R8152_WAIT_TIMEOUT; i++) {
		ocp_data = ocp_reg_read(tp, OCP_PHY_STATUS) & PHY_STAT_MASK;
		if (ocp_data == PHY_STAT_LAN_ON || ocp_data == PHY_STAT_PWRDN)
			break;

		mdelay(1);
	}

	r8153_u2p3en(tp, false);

	/* MSC timer = 0xfff * 8ms = 32760 ms */
	ocp_write_word(tp, MCU_TYPE_USB, USB_MSC_TIMER, 0x0fff);

	r8153_power_cut_en(tp, false);

	/* MAC clock speed down */
	r8153_mac_clk_speed_down(tp, false);

	r8153b_mcu_spdown_en(tp, false);

	if (tp->version == RTL_VER_09) {
		/* Disable Test IO for 32QFN */
		if (ocp_read_byte(tp, MCU_TYPE_PLA, 0xdc00) & BIT(5)) {
			ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_PHY_PWR);
			ocp_data |= TEST_IO_OFF;
			ocp_write_word(tp, MCU_TYPE_PLA, PLA_PHY_PWR, ocp_data);
		}
	}

	/* enable rx aggregation */
	ocp_data = ocp_read_word(tp, MCU_TYPE_USB, USB_USB_CTRL);
	ocp_data &= ~(RX_AGG_DISABLE | RX_ZERO_EN);
	ocp_write_word(tp, MCU_TYPE_USB, USB_USB_CTRL, ocp_data);

	rtl_tally_reset(tp);
	r8153b_hw_phy_cfg(tp);
	r8152b_enable_fc(tp);
}

static void r8153c_init(struct r8152 *tp)
{
	ocp_byte_clr_bits(tp, MCU_TYPE_USB, USB_MISC_2, BIT(7));

	r8153b_init(tp);
}

static void r8156_init(struct r8152 *tp)
{
	u32 ocp_data;
	int i;

	ocp_byte_clr_bits(tp, MCU_TYPE_USB, USB_ECM_OP, EN_ALL_SPEED);
	ocp_write_word(tp, MCU_TYPE_USB, USB_SPEED_OPTION, 0);
	ocp_word_set_bits(tp, MCU_TYPE_USB, USB_ECM_OPTION, BYPASS_MAC_RESET);

	r8153_disable_aldps(tp);
	r8153b_u1u2en(tp, false);

	r8152_wait_for_bit(tp, 0, MCU_TYPE_PLA, PLA_BOOT_CTRL,
			   AUTOLOAD_DONE, 1, R8152_WAIT_TIMEOUT);

	for (i = 0; i < R8152_WAIT_TIMEOUT; i++) {
		ocp_data = ocp_reg_read(tp, OCP_PHY_STATUS) & PHY_STAT_MASK;
		if (ocp_data == PHY_STAT_LAN_ON || ocp_data == PHY_STAT_PWRDN)
			break;

		mdelay(1);
	}

	r8153_u2p3en(tp, false);

	/* MSC timer = 0xfff * 8ms = 32760 ms */
	ocp_write_word(tp, MCU_TYPE_USB, USB_MSC_TIMER, 0x0fff);

	r8153b_power_cut_en(tp, false);

	/* MAC clock speed down */
	r8156_mac_clk_spd(tp, false);

	r8153b_mcu_spdown_en(tp, false);

	/* enable rx aggregation */
	ocp_word_clr_bits(tp, MCU_TYPE_USB, USB_USB_CTRL,
			  RX_AGG_DISABLE | RX_ZERO_EN);

	rtl_tally_reset(tp);
	r8156_hw_phy_cfg(tp);
	r8152b_enable_fc(tp);

	/* RX aggregation timer, as Linux sets it for this family */
	tp->coalesce = 15000;
}

static void r8156b_init(struct r8152 *tp)
{
	u32 ocp_data;
	u16 data;

	ocp_byte_clr_bits(tp, MCU_TYPE_USB, USB_ECM_OP, EN_ALL_SPEED);
	ocp_write_word(tp, MCU_TYPE_USB, USB_SPEED_OPTION, 0);
	ocp_word_set_bits(tp, MCU_TYPE_USB, USB_ECM_OPTION,
			  BYPASS_MAC_RESET);
	ocp_word_set_bits(tp, MCU_TYPE_USB, USB_U2P3_CTRL, RX_DETECT8);

	r8153_disable_aldps(tp);
	r8153b_u1u2en(tp, false);

	switch (tp->version) {
	case RTL_VER_13:
	case RTL_VER_15:
		r8156b_wait_loading_flash(tp);
		break;
	default:
		break;
	}

	r8152_wait_for_bit(tp, 0, MCU_TYPE_PLA, PLA_BOOT_CTRL,
			   AUTOLOAD_DONE, 1, R8152_WAIT_TIMEOUT);

	data = r8153_phy_status(tp, 0);
	if (data == PHY_STAT_EXT_INIT) {
		ocp_reg_clr_bits(tp, 0xa468, BIT(3) | BIT(1));
		ocp_reg_clr_bits(tp, 0xa466, BIT(0));
	}

	r8152_mdio_test_and_clr_bit(tp, MII_BMCR, BMCR_PDOWN);

	r8153_phy_status(tp, PHY_STAT_LAN_ON);

	r8153_u2p3en(tp, false);

	/* MSC timer = 0xfff * 8ms = 32760 ms */
	ocp_write_word(tp, MCU_TYPE_USB, USB_MSC_TIMER, 0x0fff);

	r8153b_power_cut_en(tp, false);

	ocp_word_clr_bits(tp, MCU_TYPE_PLA, PLA_RCR, SLOT_EN);

	ocp_word_set_bits(tp, MCU_TYPE_PLA, PLA_CPCR, FLOW_CTRL_EN);

	/* enable fc timer and set timer to 600 ms. */
	ocp_write_word(tp, MCU_TYPE_USB, USB_FC_TIMER,
		       CTRL_TIMER_EN | (600 / 8));

	ocp_data = ocp_read_word(tp, MCU_TYPE_PLA, PLA_POL_GPIO_CTRL);
	if (!(ocp_data & DACK_DET_EN))
		ocp_word_w0w1(tp, MCU_TYPE_USB, USB_FW_CTRL, AUTO_SPEEDUP,
			      FLOW_CTRL_PATCH_2);
	else
		ocp_word_clr_bits(tp, MCU_TYPE_USB, USB_FW_CTRL,
				  AUTO_SPEEDUP);

	ocp_word_set_bits(tp, MCU_TYPE_USB, USB_FW_TASK, FC_PATCH_TASK);

	r8156_mac_clk_spd(tp, false);

	r8153b_mcu_spdown_en(tp, false);

	/* rx aggregation */
	ocp_word_clr_bits(tp, MCU_TYPE_USB, USB_USB_CTRL,
			RX_AGG_DISABLE | RX_ZERO_EN);

	r8156_mdio_force_mode(tp);

	rtl_tally_reset(tp);
	r8156b_hw_phy_cfg(tp);
	r8152b_enable_fc(tp);

	/* RX aggregation timer, as Linux sets it for this family */
	tp->coalesce = 15000;
}

static void rtl8152_unload(struct r8152 *tp)
{
	if (tp->version != RTL_VER_01)
		r8152_power_cut_en(tp, true);
}

static void rtl8153_unload(struct r8152 *tp)
{
	r8153_power_cut_en(tp, false);
}

static int rtl_ops_init(struct r8152 *tp)
{
	struct rtl_ops *ops = &tp->rtl_ops;
	int ret = 0;

	switch (tp->version) {
	case RTL_VER_01:
	case RTL_VER_02:
	case RTL_VER_07:
		ops->init		= r8152b_init;
		ops->enable		= rtl8152_enable;
		ops->disable		= rtl8152_disable;
		ops->up			= rtl8152_up;
		ops->down		= rtl8152_down;
		ops->unload		= rtl8152_unload;
		break;

	case RTL_VER_03:
	case RTL_VER_04:
	case RTL_VER_05:
	case RTL_VER_06:
		ops->init		= r8153_init;
		ops->enable		= rtl8153_enable;
		ops->disable		= rtl8153_disable;
		ops->up			= rtl8153_up;
		ops->down		= rtl8153_down;
		ops->unload		= rtl8153_unload;
		break;

	case RTL_VER_08:
	case RTL_VER_09:
		ops->init		= r8153b_init;
		ops->enable		= rtl8153_enable;
		ops->disable		= rtl8153_disable;
		ops->up			= rtl8153b_up;
		ops->down		= rtl8153b_down;
		break;

	case RTL_TEST_01:
	case RTL_VER_10:
	case RTL_VER_11:
		ops->init		= r8156_init;
		ops->enable		= rtl8156_enable;
		ops->disable		= rtl8156_disable;
		ops->up			= rtl8156_up;
		ops->down		= rtl8156_down;
		break;

	case RTL_VER_12:
	case RTL_VER_13:
	case RTL_VER_15:
		ops->init		= r8156b_init;
		ops->enable		= rtl8156_enable;
		ops->disable		= rtl8156_disable;
		ops->up			= rtl8156_up;
		ops->down		= rtl8156_down;
		break;

	case RTL_VER_14:
		ops->init		= r8153c_init;
		ops->enable		= rtl8153_enable;
		ops->disable		= rtl8153_disable;
		ops->up			= rtl8153c_up;
		ops->down		= rtl8153b_down;
		break;

	default:
		ret = -ENODEV;
		printf("r8152: unsupported chip version %d\n", tp->version);
		break;
	}

	return ret;
}

static const char *rtl8152_speed_name(u16 speed)
{
	if (speed & _2500bps)
		return "2.5 Gbps";
	else if (speed & _1000bps)
		return "1 Gbps";
	else if (speed & _500bps)
		return "500 Mbps";
	else if (speed & _100bps)
		return "100 Mbps";
	else if (speed & _10bps)
		return "10 Mbps";
	else
		return "unknown speed";
}

static int r8152_init_common(struct r8152 *tp)
{
	u16 speed;
	int timeout = 0;
	int link_detected;

	debug("** %s()\n", __func__);

	do {
		speed = rtl8152_get_speed(tp);

		link_detected = speed & LINK_STATUS;
		if (!link_detected) {
			if (timeout == 0)
				printf("Waiting for Ethernet connection... ");
			mdelay(TIMEOUT_RESOLUTION);
			timeout += TIMEOUT_RESOLUTION;
		}
	} while (!link_detected && timeout < PHY_CONNECT_TIMEOUT);
	if (link_detected) {
		tp->rtl_ops.enable(tp);

		if (timeout != 0)
			printf("done.\n");

		printf("r8152: link up, %s %s duplex\n",
		       rtl8152_speed_name(speed),
		       (speed & FULL_DUP) ? "full" : "half");
	} else {
		printf("unable to connect.\n");
	}

	return 0;
}

static int r8152_send_common(struct ueth_data *ueth, void *packet, int length)
{
	struct usb_device *udev = ueth->pusb_dev;
	u32 opts1, opts2 = 0;
	int err;
	int actual_len;
	ALLOC_CACHE_ALIGN_BUFFER(uint8_t, msg,
				 PKTSIZE + sizeof(struct tx_desc));
	struct tx_desc *tx_desc = (struct tx_desc *)msg;

	debug("** %s(), len %d\n", __func__, length);

	opts1 = length | TX_FS | TX_LS;

	tx_desc->opts2 = cpu_to_le32(opts2);
	tx_desc->opts1 = cpu_to_le32(opts1);

	memcpy(msg + sizeof(struct tx_desc), (void *)packet, length);

	err = usb_bulk_msg(udev, usb_sndbulkpipe(udev, ueth->ep_out),
			   (void *)msg, length + sizeof(struct tx_desc),
			   &actual_len, USB_BULK_SEND_TIMEOUT);
	debug("Tx: len = %zu, actual = %u, err = %d\n",
	      length + sizeof(struct tx_desc), actual_len, err);

	return err;
}

static int r8152_eth_start(struct udevice *dev)
{
	struct r8152 *tp = dev_get_priv(dev);

	debug("** %s (%d)\n", __func__, __LINE__);

	return r8152_init_common(tp);
}

void r8152_eth_stop(struct udevice *dev)
{
	struct r8152 *tp = dev_get_priv(dev);

	debug("** %s (%d)\n", __func__, __LINE__);

	tp->rtl_ops.disable(tp);
}

int r8152_eth_send(struct udevice *dev, void *packet, int length)
{
	struct r8152 *tp = dev_get_priv(dev);

	return r8152_send_common(&tp->ueth, packet, length);
}

int r8152_eth_recv(struct udevice *dev, int flags, uchar **packetp)
{
	struct r8152 *tp = dev_get_priv(dev);
	struct ueth_data *ueth = &tp->ueth;
	uint8_t *ptr;
	int ret, len;
	struct rx_desc *rx_desc;
	u16 packet_len;

	len = usb_ether_get_rx_bytes(ueth, &ptr);
	debug("%s: first try, len=%d\n", __func__, len);
	if (!len) {
		if (!(flags & ETH_RECV_CHECK_DEVICE))
			return -EAGAIN;
		ret = usb_ether_receive(ueth, RTL8152_AGG_BUF_SZ);
		if (ret)
			return ret;

		len = usb_ether_get_rx_bytes(ueth, &ptr);
		debug("%s: second try, len=%d\n", __func__, len);
	}

	rx_desc = (struct rx_desc *)ptr;
	packet_len = le32_to_cpu(rx_desc->opts1) & RX_LEN_MASK;
	packet_len -= CRC_SIZE;

	if (packet_len > len - (sizeof(struct rx_desc) + CRC_SIZE)) {
		debug("Rx: too large packet: %d\n", packet_len);
		goto err;
	}

	*packetp = ptr + sizeof(struct rx_desc);
	return packet_len;

err:
	usb_ether_advance_rxbuf(ueth, -1);
	return -ENOSPC;
}

static int r8152_free_pkt(struct udevice *dev, uchar *packet, int packet_len)
{
	struct r8152 *tp = dev_get_priv(dev);

	packet_len += sizeof(struct rx_desc) + CRC_SIZE;
	packet_len = ALIGN(packet_len, 8);
	usb_ether_advance_rxbuf(&tp->ueth, packet_len);

	return 0;
}

static int r8152_write_hwaddr(struct udevice *dev)
{
	struct eth_pdata *pdata = dev_get_plat(dev);
	struct r8152 *tp = dev_get_priv(dev);

	unsigned char enetaddr[8] = { 0 };

	debug("** %s (%d)\n", __func__, __LINE__);
	memcpy(enetaddr, pdata->enetaddr, ETH_ALEN);

	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_CONFIG);
	pla_ocp_write(tp, PLA_IDR, BYTE_EN_SIX_BYTES, 8, enetaddr);
	ocp_write_byte(tp, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_NORAML);

	debug("MAC %pM\n", pdata->enetaddr);
	return 0;
}

int r8152_read_rom_hwaddr(struct udevice *dev)
{
	struct eth_pdata *pdata = dev_get_plat(dev);
	struct r8152 *tp = dev_get_priv(dev);

	debug("** %s (%d)\n", __func__, __LINE__);
	r8152_read_mac(tp, pdata->enetaddr);
	return 0;
}

static int r8152_eth_probe(struct udevice *dev)
{
	struct usb_device *udev = dev_get_parent_priv(dev);
	struct eth_pdata *pdata = dev_get_plat(dev);
	struct r8152 *tp = dev_get_priv(dev);
	struct ueth_data *ueth = &tp->ueth;
	int ret;

	tp->udev = udev;
	r8152_read_mac(tp, pdata->enetaddr);

	r8152b_get_version(tp);

	ret = rtl_ops_init(tp);
	if (ret)
		return ret;

	tp->rtl_ops.init(tp);
	tp->rtl_ops.up(tp);

	rtl8152_set_speed(tp, AUTONEG_ENABLE,
			  rtl8152_get_support_speed(tp),
			  DUPLEX_FULL);

	return usb_ether_register(dev, ueth, RTL8152_AGG_BUF_SZ);
}

static const struct eth_ops r8152_eth_ops = {
	.start	= r8152_eth_start,
	.send	= r8152_eth_send,
	.recv	= r8152_eth_recv,
	.free_pkt = r8152_free_pkt,
	.stop	= r8152_eth_stop,
	.write_hwaddr = r8152_write_hwaddr,
	.read_rom_hwaddr = r8152_read_rom_hwaddr,
};

U_BOOT_DRIVER(r8152_eth) = {
	.name	= "r8152_eth",
	.id	= UCLASS_ETH,
	.probe = r8152_eth_probe,
	.ops	= &r8152_eth_ops,
	.priv_auto	= sizeof(struct r8152),
	.plat_auto	= sizeof(struct eth_pdata),
};

static const struct usb_device_id r8152_eth_id_table[] = {
	/* Realtek */
	{ USB_DEVICE(0x0bda, 0x8050) },
	{ USB_DEVICE(0x0bda, 0x8152) },
	{ USB_DEVICE(0x0bda, 0x8153) },
	{ USB_DEVICE(0x0bda, 0x8155) },
	{ USB_DEVICE(0x0bda, 0x8156) },

	/* Samsung */
	{ USB_DEVICE(0x04e8, 0xa101) },

	/* Lenovo */
	{ USB_DEVICE(0x17ef, 0x304f) },
	{ USB_DEVICE(0x17ef, 0x3052) },
	{ USB_DEVICE(0x17ef, 0x3054) },
	{ USB_DEVICE(0x17ef, 0x3057) },
	{ USB_DEVICE(0x17ef, 0x7205) },
	{ USB_DEVICE(0x17ef, 0x720a) },
	{ USB_DEVICE(0x17ef, 0x720b) },
	{ USB_DEVICE(0x17ef, 0x720c) },

	/* TP-LINK */
	{ USB_DEVICE(0x2357, 0x0601) },
	{ USB_DEVICE(0x2357, 0x0602) },

	/* Nvidia */
	{ USB_DEVICE(0x0955, 0x09ff) },

	{ }		/* Terminating entry */
};

U_BOOT_USB_DEVICE(r8152_eth, r8152_eth_id_table);
