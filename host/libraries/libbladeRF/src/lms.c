/*
 * This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2013-2014 Nuand LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * If you're diving into this file, have the following documentation handy.
 *
 * As most registers don't have a clearly defined names, or are not grouped by
 * a specific set of functionality, there's little value in providing named
 * macro definitions, hence the hard-coded addresses and bitmasks.
 *
 * LMS6002D Project page:
 *  http://www.limemicro.com/products/LMS6002D.php?sector=default
 *
 * LMS6002D Datasheet:
 *  http://www.limemicro.com/download/LMS6002Dr2-DataSheet-1.2r0.pdf
 *
 * LMS6002D Programming and Calibration Guide:
 *  http://www.limemicro.com/download/LMS6002Dr2-Programming_and_Calibration_Guide-1.1r1.pdf
 *
 * LMS6002D FAQ:
 *  http://www.limemicro.com/download/FAQ_v1.0r10.pdf
 *
 */
#include <libbladeRF.h>
#include "lms.h"
#include "bladerf_priv.h"
#include "log.h"
#include "rel_assert.h"

#define kHz(x) (x * 1000)
#define MHz(x) (x * 1000000)
#define GHz(x) (x * 1000000000)

struct dc_cal_state {
    uint8_t clk_en;                 /* Backup of clock enables */

    uint8_t reg0x71;                /* Backup of registers */
    uint8_t reg0x7c;

    bladerf_lna_gain lna_gain;      /* Backup of gain values */
    int rxvga1_gain;
    int rxvga2_gain;

    uint8_t base_addr;              /* Base address of DC cal regs */
    unsigned int num_submodules;    /* # of DC cal submodules to operate on */

    int rxvga1_curr_gain;           /* Current gains used in retry loops */
    int rxvga2_curr_gain;
};

/* LPF conversion table */
static const unsigned int uint_bandwidths[] = {
    MHz(28),
    MHz(20),
    MHz(14),
    MHz(12),
    MHz(10),
    kHz(8750),
    MHz(7),
    MHz(6),
    kHz(5500),
    MHz(5),
    kHz(3840),
    MHz(3),
    kHz(2750),
    kHz(2500),
    kHz(1750),
    kHz(1500)
};

#define FREQ_RANGE(low_, high_, value_) \
{ \
    FIELD_INIT(.low, low_), \
    FIELD_INIT(.high,  high_), \
    FIELD_INIT(.value, value_), \
}

/* Frequency Range table. Corresponds to the LMS FREQSEL table.
 * Per feedback from the LMS google group, the last entry, listed as 3.72G
 * in the programming manual, can be applied up to 3.8G */
static const struct freq_range {
    uint32_t    low;
    uint32_t    high;
    uint8_t     value;
} bands[] = {
    FREQ_RANGE(BLADERF_FREQUENCY_MIN,   285625000,              0x27),
    FREQ_RANGE(285625000,               336875000,              0x2f),
    FREQ_RANGE(336875000,               405000000,              0x37),
    FREQ_RANGE(405000000,               465000000,              0x3f),
    FREQ_RANGE(465000000,               571250000,              0x26),
    FREQ_RANGE(571250000,               673750000,              0x2e),
    FREQ_RANGE(673750000,               810000000,              0x36),
    FREQ_RANGE(810000000,               930000000,              0x3e),
    FREQ_RANGE(930000000,               1142500000,             0x25),
    FREQ_RANGE(1142500000,              1347500000,             0x2d),
    FREQ_RANGE(1347500000,              1620000000,             0x35),
    FREQ_RANGE(1620000000,              1860000000,             0x3d),
    FREQ_RANGE(1860000000u,             2285000000u,            0x24),
    FREQ_RANGE(2285000000u,             2695000000u,            0x2c),
    FREQ_RANGE(2695000000u,             3240000000u,            0x34),
    FREQ_RANGE(3240000000u,             BLADERF_FREQUENCY_MAX,  0x3c),
};

 /*
 * The LMS FAQ (Rev 1.0r10, Section 5.20) states that the RXVGA1 codes may be
 * converted to dB via:
 *      value_db = 20 * log10(127 / (127 - code))
 *
 * However, an offset of 5 appears to be required, yielding:
 *      value_db =  5 + 20 * log10(127 / (127 - code))
 *
 */
static const uint8_t rxvga1_lut_code2val[] = {
    5,  5,  5,  5,  5,  5,  5,  5,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,
    6,  6,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  7,  8,  8,  8,  8,  8,  8,
    8,  8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9,  9,  10, 10, 10, 10, 10,
    10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12, 13, 13,
    13, 13, 13, 13, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 16, 16, 16, 16, 17,
    17, 17, 18, 18, 18, 18, 19, 19, 19, 20, 20, 21, 21, 22, 22, 22, 23, 24, 24,
    25, 25, 26, 27, 28, 29, 30
};

/* The closest values from the above forumla have been selected.
 * indicides 0 - 4 are clamped to 5dB */
static const uint8_t rxvga1_lut_val2code[] = {
    2,  2,  2,  2,   2,   2,   14,  26,  37,  47,  56,  63,  70,  76,  82,  87,
    91, 95, 99, 102, 104, 107, 109, 111, 113, 114, 116, 117, 118, 119, 120,
};

static const uint8_t lms_reg_dumpset[] = {
    /* Top level configuration */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0E, 0x0F,

    /* TX PLL Configuration */
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
    0x1C, 0x1D, 0x1E, 0x1F,

    /* RX PLL Configuration */
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B,
    0x2C, 0x2D, 0x2E, 0x2F,

    /* TX LPF Modules Configuration */
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,

    /* TX RF Modules Configuration */
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B,
    0x4C, 0x4D, 0x4E, 0x4F,

    /* RX LPF, ADC, and DAC Modules Configuration */
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B,
    0x5C, 0x5D, 0x5E, 0x5F,

    /* RX VGA2 Configuration */
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,

    /* RX FE Modules Configuration */
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C
};

/* Register 0x08:  RF loopback config and additional BB config
 *
 * LBRFEN[3:0] @ [3:0]
 *  0000 - RF loopback disabled
 *  0001 - TXMIX output connected to LNA1 path
 *  0010 - TXMIX output connected to LNA2 path
 *  0011 - TXMIX output connected to LNA3 path
 *  else - Reserved
 *
 * LBEN_OPIN @ [4]
 *  0   - Disabled
 *  1   - TX BB loopback signal is connected to RX output pins
 *
 * LBEN_VGA2IN @ [5]
 *  0   - Disabled
 *  1   - TX BB loopback signal is connected to RXVGA2 input
 *
 * LBEN_LPFIN @ [6]
 *  0   - Disabled
 *  1   - TX BB loopback signal is connected to RXLPF input
 *
 */
#define LBEN_OPIN   (1 << 4)
#define LBEN_VGA2IN (1 << 5)
#define LBEN_LPFIN  (1 << 6)
#define LBEN_MASK   (LBEN_OPIN | LBEN_VGA2IN | LBEN_LPFIN)

#define LBRFEN_LNA1 1
#define LBRFEN_LNA2 2
#define LBRFEN_LNA3 3
#define LBRFEN_MASK 0xf     /* [3:2] are marked reserved */


/* Register 0x46: Baseband loopback config
 *
 * LOOPBBEN[1:0] @ [3:2]
 *  00 - All Baseband loops opened (default)
 *  01 - TX loopback path connected from TXLPF output
 *  10 - TX loopback path connected from TXVGA1 output
 *  11 - TX loopback path connected from Env/peak detect output
 */
#define LOOPBBEN_TXLPF  (1 << 2)
#define LOOPBBEN_TXVGA  (2 << 2)
#define LOOPBBEN_ENVPK  (3 << 2)
#define LOOBBBEN_MASK   (3 << 2)

static inline int is_loopback_enabled(struct bladerf *dev)
{
    bladerf_loopback loopback;
    int status;

    status = lms_get_loopback_mode(dev, &loopback);
    if (status != 0) {
        return status;
    }

    return loopback != BLADERF_LB_NONE;
}

static int write_pll_config(struct bladerf *dev, bladerf_module module,
                                     uint32_t frequency, uint8_t freqsel)
{
    int status;
    uint8_t regval;
    uint8_t selout;
    uint8_t addr;

    if (module == BLADERF_MODULE_TX) {
        addr = 0x15;
    } else {
        addr = 0x25;
    }

    status = LMS_READ(dev, addr, &regval);
    if (status != 0) {
        return status;
    }

    status = is_loopback_enabled(dev);
    if (status < 0) {
        return status;
    }

    if (status == 0) {
        /* Loopback not enabled - update the PLL output buffer. */
        selout = (frequency < BLADERF_BAND_HIGH ? 1 : 2);
        regval = (freqsel << 2) | selout;
    } else {
        /* Loopback is enabled - don't touch PLL output buffer. */
        regval = (regval & ~0xfc) | (freqsel << 2);
    }

    return LMS_WRITE(dev, addr, regval);
}


int lms_lpf_enable(struct bladerf *dev, bladerf_module mod, bool enable)
{
    int status;
    uint8_t data;
    const uint8_t reg = (mod == BLADERF_MODULE_RX) ? 0x54 : 0x34;

    status = LMS_READ(dev, reg, &data);
    if (status != 0) {
        return status;
    }

    if (enable) {
        data |= (1 << 1);
    } else {
        data &= ~(1 << 1);
    }

    status = LMS_WRITE(dev, reg, data);
    if (status != 0) {
        return status;
    }

    /* Check to see if we are bypassed */
    status = LMS_READ(dev, reg + 1, &data);
    if (status != 0) {
        return status;
    } else if (data & (1 << 6)) {
        /* Bypass is enabled; switch back to normal operation */
        data &= ~(1 << 6);
        status = LMS_WRITE(dev, reg + 1, data);
    }

    return status;
}

int lms_lpf_get_mode(struct bladerf *dev, bladerf_module mod,
                     bladerf_lpf_mode *mode)
{
    int status;
    const uint8_t reg = (mod == BLADERF_MODULE_RX) ? 0x54 : 0x34;
    uint8_t data_h, data_l;
    bool lpf_enabled, lpf_bypassed;

    status = LMS_READ(dev, reg, &data_l);
    if (status != 0) {
        return status;
    }

    status = LMS_READ(dev, reg + 1, &data_h);
    if (status != 0) {
        return status;
    }

    lpf_enabled  = (data_l & (1 << 1)) != 0;
    lpf_bypassed = (data_h & (1 << 6)) != 0;

    if (lpf_enabled && !lpf_bypassed) {
        *mode = BLADERF_LPF_NORMAL;
    } else if (!lpf_enabled && lpf_bypassed) {
        *mode = BLADERF_LPF_BYPASSED;
    } else if (!lpf_enabled && !lpf_bypassed) {
        *mode = BLADERF_LPF_DISABLED;
    } else {
        log_debug("Invalid LPF configuration: 0x%02x, 0x%02x\n",
                  data_l, data_h);
        status = BLADERF_ERR_INVAL;
    }

    return status;
}

int lms_lpf_set_mode(struct bladerf *dev, bladerf_module mod,
                     bladerf_lpf_mode mode)
{
    int status;
    const uint8_t reg = (mod == BLADERF_MODULE_RX) ? 0x54 : 0x34;
    uint8_t data_l, data_h;

    status = LMS_READ(dev, reg, &data_l);
    if (status != 0) {
        return status;
    }

    status = LMS_READ(dev, reg + 1, &data_h);
    if (status != 0) {
        return status;
    }

    switch (mode) {
        case BLADERF_LPF_NORMAL:
            data_l |= (1 << 1);     /* Enable LPF */
            data_h &= ~(1 << 6);    /* Disable LPF bypass */
            break;

        case BLADERF_LPF_BYPASSED:
            data_l &= ~(1 << 1);    /* Power down LPF */
            data_h |= (1 << 6);     /* Enable LPF bypass */
            break;

        case BLADERF_LPF_DISABLED:
            data_l &= ~(1 << 1);    /* Power down LPF */
            data_h &= ~(1 << 6);    /* Disable LPF bypass */
            break;

        default:
            log_debug("Invalid LPF mode: %d\n", mode);
            return BLADERF_ERR_INVAL;
    }

    status = LMS_WRITE(dev, reg, data_l);
    if (status != 0) {
        return status;
    }

    status = LMS_WRITE(dev, reg + 1, data_h);
    return status;
}

int lms_set_bandwidth(struct bladerf *dev, bladerf_module mod, lms_bw bw)
{
    int status;
    uint8_t data;
    const uint8_t reg = (mod == BLADERF_MODULE_RX) ? 0x54 : 0x34;

    status = LMS_READ(dev, reg, &data);
    if (status != 0) {
        return status;
    }

    data &= ~0x3c;      /* Clear out previous bandwidth setting */
    data |= (bw << 2);  /* Apply new bandwidth setting */

    return LMS_WRITE(dev, reg, data);

}


int lms_get_bandwidth(struct bladerf *dev, bladerf_module mod, lms_bw *bw)
{
    int status;
    uint8_t data;
    const uint8_t reg = (mod == BLADERF_MODULE_RX) ? 0x54 : 0x34;

    status = LMS_READ(dev, reg, &data);
    if (status != 0) {
        return status;
    }

    /* Fetch bandwidth table index from reg[5:2] */
    data >>= 2;
    data &= 0xf;

    assert(data < ARRAY_SIZE(uint_bandwidths));
    *bw = (lms_bw)data;
    return 0;
}

lms_bw lms_uint2bw(unsigned int req)
{
    lms_bw ret;

    if (     req <= kHz(1500)) ret = BW_1p5MHz;
    else if (req <= kHz(1750)) ret = BW_1p75MHz;
    else if (req <= kHz(2500)) ret = BW_2p5MHz;
    else if (req <= kHz(2750)) ret = BW_2p75MHz;
    else if (req <= MHz(3)  )  ret = BW_3MHz;
    else if (req <= kHz(3840)) ret = BW_3p84MHz;
    else if (req <= MHz(5)  )  ret = BW_5MHz;
    else if (req <= kHz(5500)) ret = BW_5p5MHz;
    else if (req <= MHz(6)  )  ret = BW_6MHz;
    else if (req <= MHz(7)  )  ret = BW_7MHz;
    else if (req <= kHz(8750)) ret = BW_8p75MHz;
    else if (req <= MHz(10) )  ret = BW_10MHz;
    else if (req <= MHz(12) )  ret = BW_12MHz;
    else if (req <= MHz(14) )  ret = BW_14MHz;
    else if (req <= MHz(20) )  ret = BW_20MHz;
    else                       ret = BW_28MHz;

    return ret;
}

/* Return the table entry */
unsigned int lms_bw2uint(lms_bw bw)
{
    unsigned int idx = bw & 0xf;
    assert(idx < ARRAY_SIZE(uint_bandwidths));
    return uint_bandwidths[idx];
}

/* Enable dithering on the module PLL */
int lms_dither_enable(struct bladerf *dev, bladerf_module mod,
                      uint8_t nbits, bool enable)
{
    int status;

    /* Select the base address based on which PLL we are configuring */
    const uint8_t reg = (mod == BLADERF_MODULE_RX) ? 0x24 : 0x14;
    uint8_t data;

    /* Valid range is 1 - 8 bits (inclusive) */
    if (nbits < 1 || nbits > 8) {
        return BLADERF_ERR_INVAL;
    }

    /* Read what we currently have in there */
    status = LMS_READ(dev, reg, &data);
    if (status != 0) {
        return status;
    }

    if (enable) {
        /* Enable dithering */
        data |= (1 << 7);

        /* Clear out the previous setting of the number of bits to dither */
        data &= ~(7 << 4);

        /* Update with the desired number of bits to dither */
        data |= (((nbits - 1) & 7) << 4);

    } else {
        /* Clear dithering enable bit */
        data &= ~(1 << 7);
    }

    /* Write it out */
    status = LMS_WRITE(dev, reg, data);
    return status;
}

/* Soft reset of the LMS */
int lms_soft_reset(struct bladerf *dev)
{

    int status = LMS_WRITE(dev, 0x05, 0x12);

    if (status == 0) {
        status = LMS_WRITE(dev, 0x05, 0x32);
    }

    return status;
}

/* Set the gain on the LNA */
int lms_lna_set_gain(struct bladerf *dev, bladerf_lna_gain gain)
{
    int status;
    uint8_t data;

    if (gain == BLADERF_LNA_GAIN_BYPASS || gain == BLADERF_LNA_GAIN_MID ||
        gain == BLADERF_LNA_GAIN_MAX) {

        status = LMS_READ(dev, 0x75, &data);
        if (status == 0) {
            data &= ~(3 << 6);          /* Clear out previous gain setting */
            data |= ((gain & 3) << 6);  /* Update gain value */
            status = LMS_WRITE(dev, 0x75, data);
        }

    } else {
        status = BLADERF_ERR_INVAL;
    }

    return status;
}

int lms_lna_get_gain(struct bladerf *dev, bladerf_lna_gain *gain)
{
    int status;
    uint8_t data;

    status = LMS_READ(dev, 0x75, &data);
    if (status == 0) {
        data >>= 6;
        data &= 3;
        *gain = (bladerf_lna_gain)data;

        if (*gain == BLADERF_LNA_GAIN_UNKNOWN) {
            status = BLADERF_ERR_INVAL;
        }
    }

    return status;
}

/* Select which LNA to enable */
int lms_select_lna(struct bladerf *dev, lms_lna lna)
{
    int status;
    uint8_t data;

    status = LMS_READ(dev, 0x75, &data);
    if (status != 0) {
        return status;
    }

    data &= ~(3 << 4);
    data |= ((lna & 3) << 4);

    return LMS_WRITE(dev, 0x75, data);
}

int lms_get_lna(struct bladerf *dev, lms_lna *lna)
{
    int status;
    uint8_t data;

    status = LMS_READ(dev, 0x75, &data);
    if (status != 0) {
        *lna = LNA_NONE;
        return status;
    } else {
        *lna = (lms_lna) (data >> 4) & 0x3;
        return 0;
    }
}

/* Enable bit is in reserved register documented in this thread:
 *  https://groups.google.com/forum/#!topic/limemicro-opensource/8iTannzlfzg
 */
int lms_rxvga1_enable(struct bladerf *dev, bool enable)
{
    int status;
    uint8_t data;

    status = LMS_READ(dev, 0x7d, &data);
    if (status != 0) {
        return status;
    }

    if (enable) {
        data &= ~(1 << 3);
    } else {
        data |= (1 << 3);
    }

    return LMS_WRITE(dev, 0x7d, data);
}

/* Set the RFB_TIA_RXFE mixer gain */
int lms_rxvga1_set_gain(struct bladerf *dev, int gain)
{
    if (gain > BLADERF_RXVGA1_GAIN_MAX) {
        gain = BLADERF_RXVGA1_GAIN_MAX;
        log_info("Clamping RXVGA1 gain to %ddB\n", gain);
    } else if (gain < BLADERF_RXVGA1_GAIN_MIN) {
        gain = BLADERF_RXVGA1_GAIN_MIN;
        log_info("Clamping RXVGA1 gain to %ddB\n", gain);
    }

    return LMS_WRITE(dev, 0x76, rxvga1_lut_val2code[gain]);
}

/* Get the RFB_TIA_RXFE mixer gain */
int lms_rxvga1_get_gain(struct bladerf *dev, int *gain)
{
    uint8_t data;
    int status = LMS_READ(dev, 0x76, &data);

    if (status == 0) {
        data &= 0x7f;
        if (data > 120) {
            data = 120;
        }

        *gain = rxvga1_lut_code2val[data];
    }

    return status;
}

/* Enable RXVGA2 */
int lms_rxvga2_enable(struct bladerf *dev, bool enable)
{
    int status;
    uint8_t data;

    status = LMS_READ(dev, 0x64, &data);
    if (status != 0) {
        return status;
    }

    if (enable) {
        data |= (1 << 1);
    } else {
        data &= ~(1 << 1);
    }

    return LMS_WRITE(dev, 0x64, data);
}


/* Set the gain on RXVGA2 */
int lms_rxvga2_set_gain(struct bladerf *dev, int gain)
{
    if (gain > BLADERF_RXVGA2_GAIN_MAX) {
        gain = BLADERF_RXVGA2_GAIN_MAX;
        log_info("Clamping RXVGA2 gain to %ddB\n", gain);
    } else if (gain < BLADERF_RXVGA2_GAIN_MIN) {
        gain = BLADERF_RXVGA2_GAIN_MIN;
        log_info("Clamping RXVGA2 gain to %ddB\n", gain);
    }

    /* 3 dB per register code */
    return LMS_WRITE(dev, 0x65, gain / 3);
}

int lms_rxvga2_get_gain(struct bladerf *dev, int *gain)
{

    uint8_t data;
    const int status = LMS_READ(dev, 0x65, &data);

    if (status == 0) {
        /* 3 dB per code */
        data *= 3;
        *gain = data;
    }

    return status;
}

int lms_select_pa(struct bladerf *dev, lms_pa pa)
{
    int status;
    uint8_t data;

    status = LMS_READ(dev, 0x44, &data);

    /* Disable PA1, PA2, and AUX PA - we'll enable as requested below. */
    data &= ~0x1C;

    /* AUX PA powered down */
    data |= (1 << 1);

    switch (pa) {
        case PA_AUX:
            data &= ~(1 << 1);  /* Power up the AUX PA */
            break;

        case PA_1:
            data |= (2 << 2);   /* PA_EN[2:0] = 010 - Enable PA1 */
            break;

        case PA_2:
            data |= (4 << 2);   /* PA_EN[2:0] = 100 - Enable PA2 */
            break;

        case PA_NONE:
            break;

        default:
            assert(!"Invalid PA selection");
            status = BLADERF_ERR_INVAL;
    }

    if (status == 0) {
        status = LMS_WRITE(dev, 0x44, data);
    }

    return status;

};

int lms_peakdetect_enable(struct bladerf *dev, bool enable)
{
    int status;
    uint8_t data;

    status = LMS_READ(dev, 0x44, &data);

    if (status == 0) {
        if (enable) {
            data &= ~(1 << 0);
        } else {
            data |= (1 << 0);
        }
        status = LMS_WRITE(dev, 0x44, data);
    }

    return status;
}

int lms_enable_rffe(struct bladerf *dev, bladerf_module module, bool enable)
{
    int status;
    uint8_t data;
    uint8_t addr = (module == BLADERF_MODULE_TX ? 0x40 : 0x70);

    status = LMS_READ(dev, addr, &data);
    if (status == 0) {

        if (module == BLADERF_MODULE_TX) {
            if (enable) {
                data |= (1 << 1);
            } else {
                data &= ~(1 << 1);
            }
        } else {
            if (enable) {
                data |= (1 << 0);
            } else {
                data &= ~(1 << 0);
            }
        }

        status = LMS_WRITE(dev, addr, data);
    }

    return status;
}

int lms_txvga2_set_gain(struct bladerf *dev, int gain_int)
{
    int status;
    uint8_t data;
    int8_t gain;

    if (gain_int > BLADERF_TXVGA2_GAIN_MAX) {
        gain = BLADERF_TXVGA2_GAIN_MAX;
        log_info("Clamping TXVGA2 gain to %ddB\n", gain);
    } else if (gain_int < BLADERF_TXVGA2_GAIN_MIN) {
        gain = 0;
        log_info("Clamping TXVGA2 gain to %ddB\n", gain);
    } else {
        gain = gain_int;
    }

    status = LMS_READ(dev, 0x45, &data);
    if (status == 0) {
        data &= ~(0x1f << 3);
        data |= ((gain & 0x1f) << 3);
        status = LMS_WRITE(dev, 0x45, data);
    }

    return status;
}

int lms_txvga2_get_gain(struct bladerf *dev, int *gain)
{
    int status;
    uint8_t data;

    status = LMS_READ(dev, 0x45, &data);

    if (status == 0) {
        *gain = (data >> 3) & 0x1f;

        /* Register values of 25-31 all correspond to 25 dB */
        if (*gain > 25) {
            *gain = 25;
        }
    }

    return status;
}

int lms_txvga1_set_gain(struct bladerf *dev, int gain_int)
{
    int8_t gain;

    if (gain_int < BLADERF_TXVGA1_GAIN_MIN) {
        gain = BLADERF_TXVGA1_GAIN_MIN;
        log_info("Clamping TXVGA1 gain to %ddB\n", gain);
    } else if (gain_int > BLADERF_TXVGA1_GAIN_MAX) {
        gain = BLADERF_TXVGA1_GAIN_MAX;
        log_info("Clamping TXVGA1 gain to %ddB\n", gain);
    } else {
        gain = gain_int;
    }

    /* Apply offset to convert gain to register table index */
    gain = (gain + 35);

    /* Since 0x41 is only VGA1GAIN, we don't need to RMW */
    return LMS_WRITE(dev, 0x41, gain);
}

int lms_txvga1_get_gain(struct bladerf *dev, int *gain)
{
    int status;
    uint8_t data;

    status = LMS_READ(dev, 0x41, &data);
    if (status == 0) {
        /* Clamp to max value */
        data = data & 0x1f;

        /* Convert table index to value */
        *gain = data - 35;
    }

    return status;
}

static inline int enable_lna_power(struct bladerf *dev, bool enable)
{
    int status;
    uint8_t regval;

    /* Magic test register to power down LNAs */
    status = LMS_READ(dev, 0x7d, &regval);
    if (status != 0) {
        return status;
    }

    if (enable) {
        regval &= ~(1 << 0);
    } else {
        regval |= (1 << 0);
    }

    status = LMS_WRITE(dev, 0x7d, regval);
    if (status != 0) {
        return status;
    }

    /* Decode test registers */
    status = LMS_READ(dev, 0x70, &regval);
    if (status != 0) {
        return status;
    }

    if (enable) {
        regval &= ~(1 << 1);
    } else {
        regval |= (1 << 1);
    }

    return LMS_WRITE(dev, 0x70, regval);
}

/* Power up/down RF loopback switch */
static inline int enable_rf_loopback_switch(struct bladerf *dev, bool enable)
{
    int status;
    uint8_t regval;

    status = LMS_READ(dev, 0x0b, &regval);
    if (status != 0) {
        return status;
    }

    if (enable) {
        regval |= (1 << 0);
    } else {
        regval &= ~(1 << 0);
    }

    return LMS_WRITE(dev, 0x0b, regval);
}


/* Configure TX-side of loopback */
static int loopback_tx(struct bladerf *dev, bladerf_loopback mode)
{
    int status = 0;

    switch(mode) {
        case BLADERF_LB_BB_TXLPF_RXVGA2:
        case BLADERF_LB_BB_TXLPF_RXLPF:
        case BLADERF_LB_BB_TXVGA1_RXVGA2:
        case BLADERF_LB_BB_TXVGA1_RXLPF:
            break;

        case BLADERF_LB_RF_LNA1:
        case BLADERF_LB_RF_LNA2:
        case BLADERF_LB_RF_LNA3:
            status = lms_select_pa(dev, PA_AUX);
            break;

        case BLADERF_LB_NONE:
        {
            struct lms_freq f;

            /* Restore proper settings (PA) for this frequency */
            status = lms_get_frequency(dev, BLADERF_MODULE_TX, &f);
            if (status != 0) {
                return status;
            }

            status = lms_set_frequency(dev, BLADERF_MODULE_TX,
                                       lms_frequency_to_hz(&f));
            if (status != 0) {
                return status;
            }

            status = lms_select_band(dev, BLADERF_MODULE_TX,
                                     lms_frequency_to_hz(&f));
            break;
        }

        default:
            assert(!"Invalid loopback mode encountered");
            status = BLADERF_ERR_INVAL;
    }

    return status;
}

/* Configure RX-side of loopback */
static int loopback_rx(struct bladerf *dev, bladerf_loopback mode)
{
    int status;
    bladerf_lpf_mode lpf_mode;
    uint8_t lna;
    uint8_t regval;

    status = lms_lpf_get_mode(dev, BLADERF_MODULE_RX, &lpf_mode);
    if (status != 0) {
        return status;
    }

    switch (mode) {
        case BLADERF_LB_BB_TXLPF_RXVGA2:
        case BLADERF_LB_BB_TXVGA1_RXVGA2:

            /* Ensure RXVGA2 is enabled */
            status = lms_rxvga2_enable(dev, true);
            if (status != 0) {
                return status;
            }

            /* RXLPF must be disabled */
            status = lms_lpf_set_mode(dev, BLADERF_MODULE_RX,
                                      BLADERF_LPF_DISABLED);
            if (status != 0) {
                return status;
            }
            break;

        case BLADERF_LB_BB_TXLPF_RXLPF:
        case BLADERF_LB_BB_TXVGA1_RXLPF:

            /* RXVGA1 must be disabled */
            status = lms_rxvga1_enable(dev, false);
            if (status != 0) {
                return status;
            }

            /* Enable the RXLPF if needed */
            if (lpf_mode == BLADERF_LPF_DISABLED) {
                status = lms_lpf_set_mode(dev, BLADERF_MODULE_RX,
                        BLADERF_LPF_NORMAL);
                if (status != 0) {
                    return status;
                }
            }

            /* Ensure RXVGA2 is enabled */
            status = lms_rxvga2_enable(dev, true);
            if (status != 0) {
                return status;
            }

            break;

        case BLADERF_LB_RF_LNA1:
        case BLADERF_LB_RF_LNA2:
        case BLADERF_LB_RF_LNA3:
            lna = mode - BLADERF_LB_RF_LNA1 + 1;
            assert(lna >= 1 && lna <= 3);

            /* Power down LNAs */
            status = enable_lna_power(dev, false);
            if (status != 0) {
                return status;
            }

            /* Ensure RXVGA1 is enabled */
            status = lms_rxvga1_enable(dev, true);
            if (status != 0) {
                return status;
            }

            /* Enable the RXLPF if needed */
            if (lpf_mode == BLADERF_LPF_DISABLED) {
                status = lms_lpf_set_mode(dev, BLADERF_MODULE_RX,
                        BLADERF_LPF_NORMAL);
                if (status != 0) {
                    return status;
                }
            }

            /* Ensure RXVGA2 is enabled */
            status = lms_rxvga2_enable(dev, true);
            if (status != 0) {
                return status;
            }

            /* Select output buffer in RX PLL and select the desired LNA */
            status = LMS_READ(dev, 0x25, &regval);
            if (status != 0) {
                return status;
            }

            regval &= ~0x03;
            regval |= lna;

            status = LMS_WRITE(dev, 0x25, regval);
            if (status != 0) {
                return status;
            }

            status = lms_select_lna(dev, lna);
            if (status != 0) {
                return status;
            }

            /* Enable RF loopback switch */
            status = enable_rf_loopback_switch(dev, true);
            if (status != 0) {
                return status;
            }

            break;

        case BLADERF_LB_NONE:
        {
            struct lms_freq f;

            /* Ensure all RX blocks are enabled */
            status = lms_rxvga1_enable(dev, true);
            if (status != 0) {
                return status;
            }

            if (lpf_mode == BLADERF_LPF_DISABLED) {
                status = lms_lpf_set_mode(dev, BLADERF_MODULE_RX,
                        BLADERF_LPF_NORMAL);
                if (status != 0) {
                    return status;
                }
            }

            status = lms_rxvga2_enable(dev, true);
            if (status != 0) {
                return status;
            }

            /* Disable RF loopback switch */
            status = enable_rf_loopback_switch(dev, false);
            if (status != 0) {
                return status;
            }

            /* Power up LNAs */
            status = enable_lna_power(dev, true);
            if (status != 0) {
                return status;
            }

            /* Restore proper settings (LNA, RX PLL) for this frequency */
            status = lms_get_frequency(dev, BLADERF_MODULE_RX, &f);
            if (status != 0) {
                return status;
            }

            status = lms_set_frequency(dev, BLADERF_MODULE_RX,
                                       lms_frequency_to_hz(&f));
            if (status != 0) {
                return status;
            }


            status = lms_select_band(dev, BLADERF_MODULE_RX,
                                     lms_frequency_to_hz(&f));
            break;
        }

        default:
            assert(!"Invalid loopback mode encountered");
            status = BLADERF_ERR_INVAL;
    }

    return status;
}

/* Configure "switches" in loopback path */
static int loopback_path(struct bladerf *dev, bladerf_loopback mode)
{
    int status;
    uint8_t loopbben, lben_lbrf;

    status = LMS_READ(dev, 0x46, &loopbben);
    if (status != 0) {
        return status;
    }

    status = LMS_READ(dev, 0x08, &lben_lbrf);
    if (status != 0) {
        return status;
    }

    /* Default to baseband loopback being disabled  */
    loopbben &= ~LOOBBBEN_MASK;

    /* Default to RF and BB loopback options being disabled */
    lben_lbrf &= ~(LBRFEN_MASK | LBEN_MASK);

    switch(mode) {
        case BLADERF_LB_BB_TXLPF_RXVGA2:
            loopbben |= LOOPBBEN_TXLPF;
            lben_lbrf |= LBEN_VGA2IN;
            break;

        case BLADERF_LB_BB_TXLPF_RXLPF:
            loopbben |= LOOPBBEN_TXLPF;
            lben_lbrf |= LBEN_LPFIN;
            break;

        case BLADERF_LB_BB_TXVGA1_RXVGA2:
            loopbben |= LOOPBBEN_TXVGA;
            lben_lbrf |= LBEN_VGA2IN;
            break;

        case BLADERF_LB_BB_TXVGA1_RXLPF:
            loopbben |= LOOPBBEN_TXVGA;
            lben_lbrf |= LBEN_LPFIN;
            break;

        case BLADERF_LB_RF_LNA1:
            lben_lbrf |= LBRFEN_LNA1;
            break;

        case BLADERF_LB_RF_LNA2:
            lben_lbrf |= LBRFEN_LNA2;
            break;

        case BLADERF_LB_RF_LNA3:
            lben_lbrf |= LBRFEN_LNA3;
            break;

        case BLADERF_LB_NONE:
            break;

        default:
            return BLADERF_ERR_INVAL;
    }

    status = LMS_WRITE(dev, 0x46, loopbben);
    if (status == 0) {
        status = LMS_WRITE(dev, 0x08, lben_lbrf);
    }

    return status;
}


int lms_set_loopback_mode(struct bladerf *dev, bladerf_loopback mode)
{
    int status;

    /* Verify a valid mode is provided before shutting anything down */
    switch (mode) {
        case BLADERF_LB_BB_TXLPF_RXVGA2:
        case BLADERF_LB_BB_TXLPF_RXLPF:
        case BLADERF_LB_BB_TXVGA1_RXVGA2:
        case BLADERF_LB_BB_TXVGA1_RXLPF:
        case BLADERF_LB_RF_LNA1:
        case BLADERF_LB_RF_LNA2:
        case BLADERF_LB_RF_LNA3:
        case BLADERF_LB_NONE:
            break;

        default:
            return BLADERF_ERR_INVAL;
    }

    /* Disable all PA/LNAs while entering loopback mode or making changes */
    status = lms_select_pa(dev, PA_NONE);
    if (status != 0) {
        return status;
    }

    status = lms_select_lna(dev, LNA_NONE);
    if (status != 0) {
        return status;
    }

    /* Disconnect loopback paths while we re-configure blocks */
    status = loopback_path(dev, BLADERF_LB_NONE);
    if (status != 0) {
        return status;
    }

    /* Configure the RX side of the loopback path */
    status = loopback_rx(dev, mode);
    if (status != 0) {
        return status;
    }

    /* Configure the TX side of the path */
    status = loopback_tx(dev, mode);
    if (status != 0) {
        return status;
    }

    /* Configure "switches" along the loopback path */
    status = loopback_path(dev, mode);
    if (status != 0) {
        return status;
    }

    return 0;
}

int lms_get_loopback_mode(struct bladerf *dev, bladerf_loopback *loopback)
{
    int status;
    uint8_t lben_lbrfen, loopbben;


    status = LMS_READ(dev, 0x08, &lben_lbrfen);
    if (status != 0) {
        return status;
    }

    status = LMS_READ(dev, 0x46, &loopbben);
    if (status != 0) {
        return status;
    }

    switch (lben_lbrfen & 0x7) {
        case LBRFEN_LNA1:
            *loopback = BLADERF_LB_RF_LNA1;
            return 0;

        case LBRFEN_LNA2:
            *loopback = BLADERF_LB_RF_LNA2;
            return 0;

        case LBRFEN_LNA3:
            *loopback = BLADERF_LB_RF_LNA3;
            return 0;

        default:
            break;
    }

    switch (lben_lbrfen & LBEN_MASK) {
        case LBEN_VGA2IN:
            if (loopbben & LOOPBBEN_TXLPF) {
                *loopback = BLADERF_LB_BB_TXLPF_RXVGA2;
                return 0;
            } else if (loopbben & LOOPBBEN_TXVGA) {
                *loopback = BLADERF_LB_BB_TXVGA1_RXVGA2;
                return 0;
            }
            break;

        case LBEN_LPFIN:
            if (loopbben & LOOPBBEN_TXLPF) {
                *loopback = BLADERF_LB_BB_TXLPF_RXLPF;
                return 0;
            } else if (loopbben & LOOPBBEN_TXVGA) {
                *loopback = BLADERF_LB_BB_TXVGA1_RXLPF;
                return 0;
            }
            break;

        default:
            break;
    }

    *loopback = BLADERF_LB_NONE;
    return 0;
}

/* Top level power down of the LMS */
int lms_power_down(struct bladerf *dev)
{
    int status;
    uint8_t data;

    status = LMS_READ(dev, 0x05, &data);
    if (status == 0) {
        data &= ~(1 << 4);
        status = LMS_WRITE(dev, 0x05, data);
    }

    return status;
}

/* Enable the PLL of a module */
int lms_pll_enable(struct bladerf *dev, bladerf_module mod, bool enable)
{
    int status;
    const uint8_t reg = (mod == BLADERF_MODULE_RX) ? 0x24 : 0x14;
    uint8_t data;

    status = LMS_READ(dev, reg, &data);
    if (status == 0) {
        if (enable) {
            data |= (1 << 3);
        } else {
            data &= ~(1 << 3);
        }
        status = LMS_WRITE(dev, reg, data);
    }

    return status;
}

/* Enable the RX subsystem */
int lms_rx_enable(struct bladerf *dev, bool enable)
{
    int status;
    uint8_t data;

    status = LMS_READ(dev, 0x05, &data);
    if (status == 0) {
        if (enable) {
            data |= (1 << 2);
        } else {
            data &= ~(1 << 2);
        }
        status = LMS_WRITE(dev, 0x05, data);
    }

    return status;
}

/* Enable the TX subsystem */
int lms_tx_enable(struct bladerf *dev, bool enable)
{
    int status;
    uint8_t data;

    status = LMS_READ(dev, 0x05, &data);

    if (status == 0) {
        if (enable) {
            data |= (1 << 3);
        } else {
            data &= ~(1 << 3);
        }
        status = LMS_WRITE(dev, 0x05, data);
    }

    return status;
}

/* Converts frequency structure to Hz */
uint32_t lms_frequency_to_hz(struct lms_freq *f)
{
    uint64_t pll_coeff;
    uint32_t div;

    pll_coeff = (((uint64_t)f->nint) << 23) + f->nfrac;
    div = (f->x << 23);

    return (uint32_t)(((f->reference * pll_coeff) + (div >> 1)) / div);
}

/* Print a frequency structure */
void lms_print_frequency(struct lms_freq *f)
{
    log_verbose("---- Frequency ----\n");
    log_verbose("  x        : %d\n", f->x);
    log_verbose("  nint     : %d\n", f->nint);
    log_verbose("  nfrac    : %u\n", f->nfrac);
    log_verbose("  freqsel  : 0x%02x\n", f->freqsel);
    log_verbose("  reference: %u\n", f->reference);
    log_verbose("  freq     : %u\n", lms_frequency_to_hz(f));
}

/* Get the frequency structure */
int lms_get_frequency(struct bladerf *dev, bladerf_module mod,
                      struct lms_freq *f)
{
    const uint8_t base = (mod == BLADERF_MODULE_RX) ? 0x20 : 0x10;
    int status;
    uint8_t data;

    status = LMS_READ(dev, base + 0, &data);
    if (status != 0) {
        return status;
    }

    f->nint = ((uint16_t)data) << 1;

    status = LMS_READ(dev, base + 1, &data);
    if (status != 0) {
        return status;
    }

    f->nint |= (data & 0x80) >> 7;
    f->nfrac = ((uint32_t)data & 0x7f) << 16;

    status = LMS_READ(dev, base + 2, &data);
    if (status != 0) {
        return status;
    }

    f->nfrac |= ((uint32_t)data)<<8;

    status = LMS_READ(dev, base + 3, &data);
    if (status != 0) {
        return status;
    }

    f->nfrac |= data;

    status = LMS_READ(dev, base + 5, &data);
    if (status != 0) {
        return status;
    }

    f->freqsel = (data>>2);
    f->x = 1 << ((f->freqsel & 7) - 3);
    f->reference = 38400000;

    return status;
}

#define VCO_HIGH 0x02
#define VCO_NORM 0x00
#define VCO_LOW 0x01
static inline int tune_vcocap(struct bladerf *dev, uint8_t base, uint8_t data)
{
    int start_i = -1, stop_i = -1;
    int i;
    uint8_t vcocap = 32;
    uint8_t step = vcocap >> 1;
    uint8_t vtune;
    int status;

    status = LMS_READ(dev, base + 9, &data);
    if (status != 0) {
        return status;
    }

    data &= ~(0x3f);
    for (i = 0; i < 6; i++) {
        status = LMS_WRITE(dev, base + 9, vcocap | data);
        if (status != 0) {
            return status;
        }

        status = LMS_READ(dev, base + 10, &vtune);
        if (status != 0) {
            return status;
        }

        vtune >>= 6;

        if (vtune == VCO_NORM) {
            log_verbose( "Found normal at VCOCAP: %d\n", vcocap );
            break;
        } else if (vtune == VCO_HIGH) {
            log_verbose( "Too high: %d -> %d\n", vcocap, vcocap + step );
            vcocap += step ;
        } else if (vtune == VCO_LOW) {
            log_verbose( "Too low: %d -> %d\n", vcocap, vcocap - step );
            vcocap -= step ;
        } else {
            log_error( "Invalid VTUNE value encountered: 0x%02x\n", vtune );
            return BLADERF_ERR_UNEXPECTED;
        }

        step >>= 1;
    }

    if (vtune != VCO_NORM) {
        log_debug( "VTUNE is not locked at the end of initial loop\n" );
        return BLADERF_ERR_UNEXPECTED;
    }

    start_i = stop_i = vcocap;
    while (start_i > 0 && vtune != VCO_HIGH) {
        start_i -= 1;

        status = LMS_WRITE(dev, base + 9, start_i | data);
        if (status != 0) {
            return status;
        }

        status = LMS_READ(dev, base + 10, &vtune);
        if (status != 0) {
            return status;
        }

        vtune >>= 6;
    }

    start_i += 1;
    log_verbose( "Found lower limit VCOCAP: %d\n", start_i );

    status = LMS_WRITE(dev, base + 9, vcocap | data );
    if (status != 0) {
        return status;
    }

    status = LMS_READ(dev, base + 10, &vtune);
    if (status != 0) {
        return status;
    }

    vtune >>= 6;

    while (stop_i < 64 && vtune != VCO_LOW) {
        stop_i += 1;

        status = LMS_WRITE(dev, base + 9, stop_i | data);
        if (status != 0) {
            return status;
        }

        status = LMS_READ(dev, base + 10, &vtune);
        if (status != 0) {
            return status;
        }

        vtune >>= 6;
    }

    stop_i -= 1;
    log_verbose( "Found upper limit VCOCAP: %d\n", stop_i );

    vcocap = (start_i + stop_i) >> 1;

    log_verbose( "Goldilocks VCOCAP: %d\n", vcocap );

    status = LMS_WRITE(dev, base + 9, vcocap | data );
    if (status != 0) {
        return status;
    }

    status = LMS_READ(dev, base + 10, &vtune);
    if (status != 0) {
        return status;
    }

    vtune >>= 6;
    log_verbose( "VTUNE: %d\n", vtune );
    if (vtune != VCO_NORM) {
        status = BLADERF_ERR_UNEXPECTED;
        log_warning("VCOCAP could not converge and VTUNE is not locked - %d\n",
                    vtune);
    }

    return status;
}

/* Set the frequency of a module */
int lms_set_frequency(struct bladerf *dev, bladerf_module mod, uint32_t freq)
{
    /* Select the base address based on which PLL we are configuring */
    const uint8_t base = (mod == BLADERF_MODULE_RX) ? 0x20 : 0x10;
    const uint64_t ref_clock = 38400000;
    uint8_t freqsel = bands[0].value;
    uint16_t nint;
    uint32_t nfrac;
    struct lms_freq f;
    uint8_t data;
    uint64_t vco_x;
    uint64_t temp;
    int status, dsm_status;
    uint8_t i = 0;

    /* Clamp out of range values */
    if (freq < BLADERF_FREQUENCY_MIN) {
        freq = BLADERF_FREQUENCY_MIN;
        log_info("Clamping frequency to %uHz\n", freq);
    } else if (freq > BLADERF_FREQUENCY_MAX) {
        freq = BLADERF_FREQUENCY_MAX;
        log_info("Clamping frequency to %uHz\n", freq);
    }

    /* Figure out freqsel */

    while(i < 16) {
        if ((freq >= bands[i].low) && (freq <= bands[i].high)) {
            freqsel = bands[i].value;
            break;
        }
        i++;
    }

    /* Calculate integer portion of the frequency value */
    vco_x = ((uint64_t)1) << ((freqsel & 7) - 3);
    temp = (vco_x * freq) / ref_clock;
    assert(temp <= UINT16_MAX);
    nint = (uint16_t)temp;

    /* Calculate the fractional portion, rounding to the nearest value */
    temp = (1 << 23) * (vco_x * freq - nint * ref_clock);
    temp = (temp + ref_clock / 2) / ref_clock;
    assert(temp <= UINT32_MAX);
    nfrac = (uint32_t)temp;

    assert(vco_x <= UINT8_MAX);
    f.x = (uint8_t)vco_x;
    f.nint = nint;
    f.nfrac = nfrac;
    f.freqsel = freqsel;
    assert(ref_clock <= UINT32_MAX);
    f.reference = (uint32_t)ref_clock;
    lms_print_frequency(&f);

    /* Turn on the DSMs */
    status = LMS_READ(dev, 0x09, &data);
    if (status == 0) {
        data |= 0x05;
        status = LMS_WRITE(dev, 0x09, data);
    }

    if (status != 0) {
        log_debug("Failed to turn on DSMs\n");
        return status;
    }

    status = write_pll_config(dev, mod, freq, freqsel);
    if (status != 0) {
        goto lms_set_frequency_error;
    }

    data = nint >> 1;
    status = LMS_WRITE(dev, base + 0, data);
    if (status != 0) {
        goto lms_set_frequency_error;
    }

    data = ((nint & 1) << 7) | ((nfrac >> 16) & 0x7f);
    status = LMS_WRITE(dev, base + 1, data);
    if (status != 0) {
        goto lms_set_frequency_error;
    }

    data = ((nfrac >> 8) & 0xff);
    status = LMS_WRITE(dev, base + 2, data);
    if (status != 0) {
        goto lms_set_frequency_error;
    }

    data = (nfrac & 0xff);
    status = LMS_WRITE(dev, base + 3, data);
    if (status != 0) {
        goto lms_set_frequency_error;
    }

    /* Set the PLL Ichp, Iup and Idn currents */
    status = LMS_READ(dev, base + 6, &data);
    if (status != 0) {
        goto lms_set_frequency_error;
    }

    data &= ~(0x1f);
    data |= 0x0c;

    status = LMS_WRITE(dev, base + 6, data);
    if (status != 0) {
        goto lms_set_frequency_error;
    }

    status = LMS_READ(dev, base + 7, &data);
    if (status != 0) {
        goto lms_set_frequency_error;
    }

    data &= ~(0x1f);
    // data |= 3;

    status = LMS_WRITE(dev, base + 7, data);
    if (status != 0) {
        goto lms_set_frequency_error;
    }

    status = LMS_READ(dev, base + 8, &data);
    if (status != 0) {
        goto lms_set_frequency_error;
    }

    data &= ~(0x1f);
    // data |= 3;
    status = LMS_WRITE(dev, base + 8, data);
    if (status != 0) {
        goto lms_set_frequency_error;
    }

    /* Loop through the VCOCAP to figure out optimal values */
    status = tune_vcocap(dev, base, data);

lms_set_frequency_error:
    /* Turn off the DSMs */
    dsm_status = LMS_READ(dev, 0x09, &data);
    if (dsm_status == 0) {
        data &= ~(0x05);
        dsm_status = LMS_WRITE(dev, 0x09, data);
    }

    return (status == 0) ? dsm_status : status;
}

int lms_dump_registers(struct bladerf *dev)
{
    int status = 0;
    uint8_t data,i;
    const uint16_t num_reg = sizeof(lms_reg_dumpset);

    for (i = 0; i < num_reg; i++) {
        status = LMS_READ(dev, lms_reg_dumpset[i], &data);
        if (status != 0) {
            log_debug("Failed to read LMS @ 0x%02x\n", lms_reg_dumpset[i]);
            return status;
        } else {
            log_debug("LMS[0x%02x] = 0x%02x\n", lms_reg_dumpset[i], data);
        }
    }

    return status;
}

/* Reference LMS6002D calibration guide, section 4.1 flow chart */
static int lms_dc_cal_loop(struct bladerf *dev, uint8_t base,
                           uint8_t cal_address, uint8_t dc_cntval,
                           uint8_t *dc_regval)
{
    int status;
    uint8_t i, val;
    bool done = false;
    const unsigned int max_cal_count = 25;

    log_debug("Calibrating module %2.2x:%2.2x\n", base, cal_address);

    /* Set the calibration address for the block, and start it up */
    status = LMS_READ(dev, base + 0x03, &val);
    if (status != 0) {
        return status;
    }

    val &= ~(0x07);
    val |= cal_address&0x07;

    status = LMS_WRITE(dev, base + 0x03, val);
    if (status != 0) {
        return status;
    }

    /* Set and latch the DC_CNTVAL  */
    status = LMS_WRITE(dev, base + 0x02, dc_cntval);
    if (status != 0) {
        return status;
    }

    val |= (1 << 4);
    status = LMS_WRITE(dev, base + 0x03, val);
    if (status != 0) {
        return status;
    }

    val &= ~(1 << 4);
    status = LMS_WRITE(dev, base + 0x03, val);
    if (status != 0) {
        return status;
    }


    /* Start the calibration by toggling DC_START_CLBR */
    val |= (1 << 5);
    status = LMS_WRITE(dev, base + 0x03, val);
    if (status != 0) {
        return status;
    }

    val &= ~(1 << 5);
    status = LMS_WRITE(dev, base + 0x03, val);
    if (status != 0) {
        return status;
    }

    /* Main loop checking the calibration */
    for (i = 0 ; i < max_cal_count && !done; i++) {
        /* Read active low DC_CLBR_DONE */
        status = LMS_READ(dev, base + 0x01, &val);
        if (status != 0) {
            return status;
        }

        /* Check if calibration is done */
        if (((val >> 1) & 1) == 0) {
            done = true;
            /* Per LMS FAQ item 4.7, we should check DC_REG_VAL, as
             * DC_LOCK is not a reliable indicator */
            status = LMS_READ(dev, base, dc_regval);
            if (status == 0) {
                *dc_regval &= 0x3f;
            }
        }
    }

    if (done == false) {
        log_warning("DC calibration loop did not converge.\n");
        status = BLADERF_ERR_UNEXPECTED;
    } else {
        log_debug( "DC_REGVAL: %d\n", *dc_regval );
    }

    return status;
}

static inline int dc_cal_backup(struct bladerf *dev,
                                bladerf_cal_module module,
                                struct dc_cal_state *state)
{
    int status;

    memset(state, 0, sizeof(state[0]));

    status = LMS_READ(dev, 0x09, &state->clk_en);
    if (status != 0) {
        return status;
    }

    if (module == BLADERF_DC_CAL_RX_LPF || module == BLADERF_DC_CAL_RXVGA2) {
        status = LMS_READ(dev, 0x71, &state->reg0x71);
        if (status != 0) {
            return status;
        }

        status = LMS_READ(dev, 0x7c, &state->reg0x7c);
        if (status != 0) {
            return status;
        }

        status = lms_lna_get_gain(dev, &state->lna_gain);
        if (status != 0) {
            return status;
        }

        status = lms_rxvga1_get_gain(dev, &state->rxvga1_gain);
        if (status != 0) {
            return status;
        }

        status = lms_rxvga2_get_gain(dev, &state->rxvga2_gain);
        if (status != 0) {
            return status;
        }
    }

    return 0;
}

static inline int dc_cal_module_init(struct bladerf *dev,
                                     bladerf_cal_module module,
                                     struct dc_cal_state *state)
{
    int status;
    uint8_t cal_clock;
    uint8_t val;

    switch (module) {
        case BLADERF_DC_CAL_LPF_TUNING:
            cal_clock = (1 << 5);  /* CLK_EN[5] - LPF CAL Clock */
            state->base_addr = 0x00;
            state->num_submodules = 1;
            break;

        case BLADERF_DC_CAL_TX_LPF:
            cal_clock = (1 << 1);  /* CLK_EN[1] - TX LPF DCCAL Clock */
            state->base_addr = 0x30;
            state->num_submodules = 2;
            break;

        case BLADERF_DC_CAL_RX_LPF:
            cal_clock = (1 << 3);  /* CLK_EN[3] - RX LPF DCCAL Clock */
            state->base_addr = 0x50;
            state->num_submodules = 2;
            break;

        case BLADERF_DC_CAL_RXVGA2:
            cal_clock = (1 << 4);  /* CLK_EN[4] - RX VGA2 DCCAL Clock */
            state->base_addr = 0x60;
            state->num_submodules = 5;
            break;

        default:
            return BLADERF_ERR_INVAL;
    }

    /* Enable the appropriate clock based on the module */
    status = LMS_WRITE(dev, 0x09, state->clk_en | cal_clock);
    if (status != 0) {
        return status;
    }

    switch (module) {

        case BLADERF_DC_CAL_LPF_TUNING:
            /* Nothing special to do */
            break;

        case BLADERF_DC_CAL_RX_LPF:
        case BLADERF_DC_CAL_RXVGA2:
            /* FAQ 5.26 (rev 1.0r10) notes that the DC comparators should be
             * powered up when performing DC calibration, and then powered down
             * afterwards to improve receiver linearity */
            if (module == BLADERF_DC_CAL_RXVGA2) {
                status = lms_clear(dev, 0x6e, (3 << 6));
                if (status != 0) {
                    return status;
                }
            } else {
                /* Power up RX LPF DC calibration comparator */
                status = lms_clear(dev, 0x5f, (1 << 7));
                if (status != 0) {
                    return status;
                }
            }

            /* Connect LNA to the external pads and internally terminate */
            val = state->reg0x71 & ~(1 << 7);
            status = LMS_WRITE(dev, 0x71, val);
            if (status != 0) {
                return status;
            }

            val = state->reg0x7c | (1 << 2);
            status = LMS_WRITE(dev, 0x7c, val);
            if (status != 0) {
                return status;
            }

            /* Attempt to calibrate at max gain. */
            status = lms_lna_set_gain(dev, BLADERF_LNA_GAIN_MAX);
            if (status != 0) {
                return status;
            }

            state->rxvga1_curr_gain = BLADERF_RXVGA1_GAIN_MAX;
            status = lms_rxvga1_set_gain(dev, state->rxvga1_curr_gain);
            if (status != 0) {
                return status;
            }

            state->rxvga2_curr_gain = BLADERF_RXVGA2_GAIN_MAX;
            status = lms_rxvga2_set_gain(dev, state->rxvga2_curr_gain);
            if (status != 0) {
                return status;
            }

            break;


        case BLADERF_DC_CAL_TX_LPF:
            /* FAQ item 4.1 notes that the DAC should be turned off or set
             * to generate minimum DC */
            status = lms_set(dev, 0x36, (1 << 7));
            if (status != 0) {
                return status;
            }

            /* Ensure TX LPF DC calibration comparator is powered up */
            status = lms_clear(dev, 0x3f, (1 << 7));
            if (status != 0) {
                return status;
            }
            break;

        default:
            assert(!"Invalid module");
            status = BLADERF_ERR_INVAL;
    }

    return status;
}

/* The RXVGA2 items here are based upon Lime Microsystems' recommendations
 * in their "Improving RxVGA2 DC Offset Calibration Stability" Document:
 *  https://groups.google.com/group/limemicro-opensource/attach/19b675d099a22b89/Improving%20RxVGA2%20DC%20Offset%20Calibration%20Stability_v1.pdf?part=0.1&authuser=0
 *
 * This function assumes that the submodules are preformed in a consecutive
 * and increasing order, as outlined in the above document.
 */
static inline int dc_cal_submodule(struct bladerf *dev,
                                   bladerf_cal_module module,
                                   unsigned int submodule,
                                   struct dc_cal_state *state,
                                   bool *converged)
{
    int status;
    uint8_t val, dc_regval;

    *converged = false;

    if (module == BLADERF_DC_CAL_RXVGA2) {
        switch (submodule) {
            case 0:
                /* Reset VGA2GAINA and VGA2GAINB to the default power-on values,
                 * in case we're retrying this calibration due to one of the
                 * later submodules failing. For the same reason, RXVGA2 decode
                 * is disabled; it is not used for the RC reference module (0)
                 */

                /* Disable RXVGA2 DECODE */
                status = lms_clear(dev, 0x64, (1 << 0));
                if (status != 0) {
                    return status;
                }

                /* VGA2GAINA = 0, VGA2GAINB = 0 */
                status = LMS_WRITE(dev, 0x68, 0x01);
                if (status != 0) {
                    return status;
                }
                break;

            case 1:
                /* Setup for Stage 1 I and Q channels (submodules 1 and 2) */

                /* Set to direct control signals: RXVGA2 Decode = 1 */
                status = lms_set(dev, 0x64, (1 << 0));
                if (status != 0) {
                    return status;
                }

                /* VGA2GAINA = 0110, VGA2GAINB = 0 */
                val = 0x06;
                status = LMS_WRITE(dev, 0x68, val);
                if (status != 0) {
                    return status;
                }
                break;

            case 2:
                /* No additional changes needed - covered by previous execution
                 * of submodule == 1. */
                break;

            case 3:
                /* Setup for Stage 2 I and Q channels (submodules 3 and 4) */

                /* VGA2GAINA = 0, VGA2GAINB = 0110 */
                val = 0x60;
                status = LMS_WRITE(dev, 0x68, val);
                if (status != 0) {
                    return status;
                }
                break;

            case 4:
                /* No additional changes needed - covered by execution
                 * of submodule == 3 */
                break;

            default:
                assert(!"Invalid submodule");
                return BLADERF_ERR_UNEXPECTED;
        }
    }

    status = lms_dc_cal_loop(dev, state->base_addr, submodule, 31, &dc_regval);
    if (status != 0) {
        return status;
    }

    if (dc_regval == 31) {
        log_debug("DC_REGVAL suboptimal value - retrying DC cal loop.\n");

        /* FAQ item 4.7 indcates that can retry with DC_CNTVAL reset */
        status = lms_dc_cal_loop(dev, state->base_addr, submodule, 0, &dc_regval);
        if (status != 0) {
            return status;
        } else if (dc_regval == 0) {
            log_debug("Bad DC_REGVAL detected. DC cal failed.\n");
            return 0;
        }
    }

    if (module == BLADERF_DC_CAL_LPF_TUNING) {
        /* Special case for LPF tuning module where results are
         * written to TX/RX LPF DCCAL */

        /* Set the DC level to RX and TX DCCAL modules */
        status = LMS_READ(dev, 0x35, &val);
        if (status == 0) {
            val &= ~(0x3f);
            val |= dc_regval;
            status = LMS_WRITE(dev, 0x35, val);
        }

        if (status != 0) {
            return status;
        }

        status = LMS_READ(dev, 0x55, &val);
        if (status == 0) {
            val &= ~(0x3f);
            val |= dc_regval;
            status = LMS_WRITE(dev, 0x55, val);
        }

        if (status != 0) {
            return status;
        }
    }

    *converged = true;
    return 0;
}

static inline int dc_cal_retry_adjustment(struct bladerf *dev,
                                          bladerf_cal_module module,
                                          struct dc_cal_state *state,
                                          bool *limit_reached)
{
    int status = 0;

    switch (module) {
        case BLADERF_DC_CAL_LPF_TUNING:
        case BLADERF_DC_CAL_TX_LPF:
            /* Nothing to adjust here */
            *limit_reached = true;
            break;

        case BLADERF_DC_CAL_RX_LPF:
            if (state->rxvga1_curr_gain > BLADERF_RXVGA1_GAIN_MIN) {
                state->rxvga1_curr_gain -= 1;
                log_debug("Retrying DC cal with RXVGA1=%d\n",
                          state->rxvga1_curr_gain);
                status = lms_rxvga1_set_gain(dev, state->rxvga1_curr_gain);
            } else {
                *limit_reached = true;
            }
            break;

        case BLADERF_DC_CAL_RXVGA2:
            if (state->rxvga1_curr_gain > BLADERF_RXVGA1_GAIN_MIN) {
                state->rxvga1_curr_gain -= 1;
                log_debug("Retrying DC cal with RXVGA1=%d\n",
                          state->rxvga1_curr_gain);
                status = lms_rxvga1_set_gain(dev, state->rxvga1_curr_gain);
            } else if (state->rxvga2_curr_gain > BLADERF_RXVGA2_GAIN_MIN) {
                state->rxvga2_curr_gain -= 3;
                log_debug("Retrying DC cal with RXVGA2=%d\n",
                          state->rxvga2_curr_gain);
                status = lms_rxvga2_set_gain(dev, state->rxvga2_curr_gain);
            } else {
                *limit_reached = true;
            }
            break;

        default:
            *limit_reached = true;
            assert(!"Invalid module");
            status = BLADERF_ERR_UNEXPECTED;
    }

    if (*limit_reached) {
        log_debug("DC Cal retry limit reached\n");
    }
    return status;
}

static inline int dc_cal_module_deinit(struct bladerf *dev,
                                       bladerf_cal_module module,
                                       struct dc_cal_state *state)
{
    int status = 0;

    switch (module) {
        case BLADERF_DC_CAL_LPF_TUNING:
            /* Nothing special to do here */
            break;

        case BLADERF_DC_CAL_RX_LPF:
            /* Power down RX LPF calibration comparator */
            status = lms_set(dev, 0x5f, (1 << 7));
            if (status != 0) {
                return status;
            }
            break;

        case BLADERF_DC_CAL_RXVGA2:
            /* Restore defaults: VGA2GAINA = 1, VGA2GAINB = 0 */
            status = LMS_WRITE(dev, 0x68, 0x01);
            if (status != 0) {
                return status;
            }

            /* Disable decode control signals: RXVGA2 Decode = 0 */
            status = lms_clear(dev, 0x64, (1 << 0));
            if (status != 0) {
                return status;
            }

            /* Power DC comparitors down, per FAQ 5.26 (rev 1.0r10) */
            status = lms_set(dev, 0x6e, (3 << 6));
            if (status != 0) {
                return status;
            }
            break;

        case BLADERF_DC_CAL_TX_LPF:
            /* Power down TX LPF DC calibration comparator */
            status = lms_set(dev, 0x3f, (1 << 7));
            if (status != 0) {
                return status;
            }

            /* Re-enable the DACs */
            status = lms_clear(dev, 0x36, (1 << 7));
            if (status != 0) {
                return status;
            }
            break;

        default:
            assert(!"Invalid module");
            status = BLADERF_ERR_INVAL;
    }

    return status;
}

static inline int dc_cal_restore(struct bladerf *dev,
                                 bladerf_cal_module module,
                                 struct dc_cal_state *state)
{
    int status, ret;
    ret = 0;

    status = LMS_WRITE(dev, 0x09, state->clk_en);
    if (status != 0) {
        ret = status;
    }

    if (module == BLADERF_DC_CAL_RX_LPF || module == BLADERF_DC_CAL_RXVGA2) {
        status = LMS_WRITE(dev, 0x71, state->reg0x71);
        if (status != 0 && ret == 0) {
            ret = status;
        }

        status = LMS_WRITE(dev, 0x7c, state->reg0x7c);
        if (status != 0 && ret == 0) {
            ret = status;
        }

        status = lms_lna_set_gain(dev, state->lna_gain);
        if (status != 0 && ret == 0) {
            ret = status;
        }

        status = lms_rxvga1_set_gain(dev, state->rxvga1_gain);
        if (status != 0 && ret == 0) {
            ret = status;
        }

        status = lms_rxvga2_set_gain(dev, state->rxvga2_gain);
        if (status != 0) {
            ret = status;
        }
    }

    return ret;
}

static inline int dc_cal_module(struct bladerf *dev,
                                bladerf_cal_module module,
                                struct dc_cal_state *state,
                                bool *converged)
{
    unsigned int i;
    int status = 0;

    *converged = true;

    for (i = 0; i < state->num_submodules && *converged && status == 0; i++) {
        status = dc_cal_submodule(dev, module, i, state, converged);
    }

    return status;
}

int lms_calibrate_dc(struct bladerf *dev, bladerf_cal_module module)
{
    int status, tmp_status;
    struct dc_cal_state state;
    bool converged, limit_reached;

    status = dc_cal_backup(dev, module, &state);
    if (status != 0) {
        return status;
    }

    status = dc_cal_module_init(dev, module, &state);
    if (status != 0) {
        goto error;
    }

    converged = false;
    limit_reached = false;

    while (!converged && !limit_reached && status == 0) {
        status = dc_cal_module(dev, module, &state, &converged);

        if (status == 0 && !converged) {
            status = dc_cal_retry_adjustment(dev, module, &state,
                                             &limit_reached);
        }
    }

    if (!converged && status == 0) {
        log_warning("DC Calibration (module=%d) failed to converge.\n", module);
        status = BLADERF_ERR_UNEXPECTED;
    }

error:
    tmp_status = dc_cal_module_deinit(dev, module, &state);
    status = (status != 0) ? status : tmp_status;

    tmp_status = dc_cal_restore(dev, module, &state);
    status = (status != 0) ? status : tmp_status;

    return status;
}

static inline int enable_lpf_cal_clock(struct bladerf *dev, bool enable)
{
    const uint8_t mask = (1 << 5);

    if (enable) {
        return lms_set(dev, 0x09, mask);
    } else {
        return lms_clear(dev, 0x09, mask);
    }
}

static inline int enable_rxvga2_dccal_clock(struct bladerf *dev, bool enable)
{
    const uint8_t mask = (1 << 4);

    if (enable) {
        return lms_set(dev, 0x09, mask);
    } else {
        return lms_clear(dev, 0x09, mask);
    }
}

static inline int enable_rxlpf_dccal_clock(struct bladerf *dev, bool enable)
{
    const uint8_t mask = (1 << 3);

    if (enable) {
        return lms_set(dev, 0x09, mask);
    } else {
        return lms_clear(dev, 0x09, mask);
    }
}

static inline int enable_txlpf_dccal_clock(struct bladerf *dev, bool enable)
{
    const uint8_t mask = (1 << 1);

    if (enable) {
        return lms_set(dev, 0x09, mask);
    } else {
        return lms_clear(dev, 0x09, mask);
    }
}

static int set_dc_cal_value(struct bladerf *dev, uint8_t base,
                             uint8_t dc_addr, int16_t value)
{
    int status;
    const uint8_t new_value = (uint8_t)value;
    uint8_t regval = (0x08 | dc_addr);

    /* Keep reset inactive, cal disable, load addr */
    status = LMS_WRITE(dev, base + 3, regval);
    if (status != 0) {
        return status;
    }

    /* Update DC_CNTVAL */
    status = LMS_WRITE(dev, base + 2, new_value);
    if (status != 0) {
        return status;
    }

    /* Strobe DC_LOAD */
    regval |= (1 << 4);
    status = LMS_WRITE(dev, base + 3, regval);
    if (status != 0) {
        return status;
    }

    regval &= ~(1 << 4);
    status = LMS_WRITE(dev, base + 3, regval);
    if (status != 0) {
        return status;
    }

    status = LMS_READ(dev, base, &regval);
    if (status != 0) {
        return status;
    }

    return 0;
}

static int get_dc_cal_value(struct bladerf *dev, uint8_t base,
                             uint8_t dc_addr, int16_t *value)
{
    int status;
    uint8_t regval;

    /* Keep reset inactive, cal disable, load addr */
    status = LMS_WRITE(dev, base + 3, (0x08 | dc_addr));
    if (status != 0) {
        return status;
    }

    /* Fetch value from DC_REGVAL */
    status = LMS_READ(dev, base, &regval);
    if (status != 0) {
        *value = -1;
        return status;
    }

    *value = regval;
    return 0;
}

int lms_set_dc_cals(struct bladerf *dev,
                     const struct bladerf_lms_dc_cals *dc_cals)
{
    int status;
    const bool cal_tx_lpf =
        (dc_cals->tx_lpf_i >= 0) || (dc_cals->tx_lpf_q >= 0);

    const bool cal_rx_lpf =
        (dc_cals->rx_lpf_i >= 0) || (dc_cals->rx_lpf_q >= 0);

    const bool cal_rxvga2 =
        (dc_cals->dc_ref >= 0) ||
        (dc_cals->rxvga2a_i >= 0) || (dc_cals->rxvga2a_q >= 0) ||
        (dc_cals->rxvga2b_i >= 0) || (dc_cals->rxvga2b_q >= 0);

    if (dc_cals->lpf_tuning >= 0) {
        status = enable_lpf_cal_clock(dev, true);
        if (status != 0) {
            return status;
        }

        status = set_dc_cal_value(dev, 0x00, 0, dc_cals->lpf_tuning);
        if (status != 0) {
            return status;
        }

        status = enable_lpf_cal_clock(dev, false);
        if (status != 0) {
            return status;
        }
    }

    if (cal_tx_lpf) {
        status = enable_txlpf_dccal_clock(dev, true);
        if (status != 0) {
            return status;
        }

        if (dc_cals->tx_lpf_i >= 0) {
            status = set_dc_cal_value(dev, 0x30, 0, dc_cals->tx_lpf_i);
            if (status != 0) {
                return status;
            }
        }

        if (dc_cals->tx_lpf_q >= 0) {
            status = set_dc_cal_value(dev, 0x30, 1, dc_cals->tx_lpf_q);
            if (status != 0) {
                return status;
            }
        }

        status = enable_txlpf_dccal_clock(dev, false);
        if (status != 0) {
            return status;
        }
    }

    if (cal_rx_lpf) {
        status = enable_rxlpf_dccal_clock(dev, true);
        if (status != 0) {
            return status;
        }

        if (dc_cals->rx_lpf_i >= 0) {
            status = set_dc_cal_value(dev, 0x50, 0, dc_cals->rx_lpf_i);
            if (status != 0) {
                return status;
            }
        }

        if (dc_cals->rx_lpf_q >= 0) {
            status = set_dc_cal_value(dev, 0x50, 1, dc_cals->rx_lpf_q);
            if (status != 0) {
                return status;
            }
        }

        status = enable_rxlpf_dccal_clock(dev, false);
        if (status != 0) {
            return status;
        }
    }

    if (cal_rxvga2) {
        status = enable_rxvga2_dccal_clock(dev, true);
        if (status != 0) {
            return status;
        }

        if (dc_cals->dc_ref >= 0) {
            status = set_dc_cal_value(dev, 0x60, 0, dc_cals->dc_ref);
            if (status != 0) {
                return status;
            }
        }

        if (dc_cals->rxvga2a_i >= 0) {
            status = set_dc_cal_value(dev, 0x60, 1, dc_cals->rxvga2a_i);
            if (status != 0) {
                return status;
            }
        }

        if (dc_cals->rxvga2a_q >= 0) {
            status = set_dc_cal_value(dev, 0x60, 2, dc_cals->rxvga2a_q);
            if (status != 0) {
                return status;
            }
        }

        if (dc_cals->rxvga2b_i >= 0) {
            status = set_dc_cal_value(dev, 0x60, 3, dc_cals->rxvga2b_i);
            if (status != 0) {
                return status;
            }
        }

        if (dc_cals->rxvga2b_q >= 0) {
            status = set_dc_cal_value(dev, 0x60, 4, dc_cals->rxvga2b_q);
            if (status != 0) {
                return status;
            }
        }

        status = enable_rxvga2_dccal_clock(dev, false);
        if (status != 0) {
            return status;
        }
    }

    return 0;
}

int lms_get_dc_cals(struct bladerf *dev, struct bladerf_lms_dc_cals *dc_cals)
{
    int status;

    status = get_dc_cal_value(dev, 0x00, 0, &dc_cals->lpf_tuning);
    if (status != 0) {
        return status;
    }

    status = get_dc_cal_value(dev, 0x30, 0, &dc_cals->tx_lpf_i);
    if (status != 0) {
        return status;
    }

    status = get_dc_cal_value(dev, 0x30, 1, &dc_cals->tx_lpf_q);
    if (status != 0) {
        return status;
    }

    status = get_dc_cal_value(dev, 0x50, 0, &dc_cals->rx_lpf_i);
    if (status != 0) {
        return status;
    }

    status = get_dc_cal_value(dev, 0x50, 1, &dc_cals->rx_lpf_q);
    if (status != 0) {
        return status;
    }

    status = get_dc_cal_value(dev, 0x60, 0, &dc_cals->dc_ref);
    if (status != 0) {
        return status;
    }

    status = get_dc_cal_value(dev, 0x60, 1, &dc_cals->rxvga2a_i);
    if (status != 0) {
        return status;
    }

    status = get_dc_cal_value(dev, 0x60, 2, &dc_cals->rxvga2a_q);
    if (status != 0) {
        return status;
    }

    status = get_dc_cal_value(dev, 0x60, 3, &dc_cals->rxvga2b_i);
    if (status != 0) {
        return status;
    }

    status = get_dc_cal_value(dev, 0x60, 4, &dc_cals->rxvga2b_q);
    if (status != 0) {
        return status;
    }

    return 0;
}

int lms_select_band(struct bladerf *dev, bladerf_module module,
                    unsigned int freq)
{
    int status;
    lms_lna lna;
    lms_pa pa;

    /* If loopback mode disabled, avoid changing the PA or LNA selection,
     * as these need to remain are powered down or disabled */
    status = is_loopback_enabled(dev);
    if (status < 0) {
        return status;
    } else if (status > 0) {
        return 0;
    }

    lna = (freq >= BLADERF_BAND_HIGH) ? LNA_2 : LNA_1;
    pa  = (freq >= BLADERF_BAND_HIGH) ? PA_2 : PA_1;

    if (module == BLADERF_MODULE_TX) {
        status = lms_select_pa(dev, pa);
    } else {
        status = lms_select_lna(dev, lna);
    }

    return status;
}

int lms_select_sampling(struct bladerf *dev, bladerf_sampling sampling)
{
    uint8_t val;
    int status = 0;

    if (sampling == BLADERF_SAMPLING_INTERNAL) {
        /* Disconnect the ADC input from the outside world */
        status = LMS_READ( dev, 0x09, &val );
        if (status) {
            log_warning( "Could not read LMS to connect ADC to external pins\n" );
            goto out;
        }

        val &= ~(1<<7);
        status = LMS_WRITE( dev, 0x09, val );
        if (status) {
            log_warning( "Could not write LMS to connect ADC to external pins\n" );
            goto out;
        }

        /* Turn on RXVGA2 */
        status = LMS_READ( dev, 0x64, &val );
        if (status) {
            log_warning( "Could not read LMS to enable RXVGA2\n" );
            goto out;
        }

        val |= (1<<1);
        status = LMS_WRITE( dev, 0x64, val );
        if (status) {
            log_warning( "Could not write LMS to enable RXVGA2\n" );
            goto out;
        }
    } else if (sampling == BLADERF_SAMPLING_EXTERNAL) {
        /* Turn off RXVGA2 */
        status = LMS_READ( dev, 0x64, &val );
        if (status) {
            log_warning( "Could not read the LMS to disable RXVGA2\n" );
            goto out;
        }

        val &= ~(1<<1);
        status = LMS_WRITE( dev, 0x64, val );
        if (status) {
            log_warning( "Could not write the LMS to disable RXVGA2\n" );
            goto out;
        }

        /* Connect the external ADC pins to the internal ADC input */
        status = LMS_READ( dev, 0x09, &val );
        if (status) {
            log_warning( "Could not read the LMS to connect ADC to internal pins\n" );
            goto out;
        }

        val |= (1<<7);
        status = LMS_WRITE( dev, 0x09, val );
        if (status) {
            log_warning( "Could not write the LMS to connect ADC to internal pins\n" );
        }
    } else {
        status = BLADERF_ERR_INVAL;
    }

out:
    return status;
}

int lms_get_sampling(struct bladerf *dev, bladerf_sampling *sampling)
{
    int status = 0, external = 0;
    uint8_t val = 0;

    status = LMS_READ(dev, 0x09, &val);
    if (status != 0) {
        log_warning("Could not read state of ADC pin connectivity\n");
        goto out;
    }
    external = (val & (1 << 7)) ? 1 : 0;

    status = LMS_READ(dev, 0x64, &val);
    if (status != 0) {
        log_warning( "Could not read RXVGA2 state\n" );
        goto out;
    }
    external |= (val & (1 << 1)) ? 0 : 2;

    switch (external) {
        case 0:
            *sampling = BLADERF_SAMPLING_INTERNAL;
            break;

        case 3:
            *sampling = BLADERF_SAMPLING_EXTERNAL;
            break;

        default:
            *sampling = BLADERF_SAMPLING_UNKNOWN;
            break;
    }

out:
    return status;
}
