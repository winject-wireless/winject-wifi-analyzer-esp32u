/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Raw I/Q capture on the original ESP32, adapted from ESP-SDR
 * (firmware/esp-sdr/main/targets/esp32/receiver.c, GPLv3). The register
 * sequence and private PHY calls are ESP-SDR's; they are valid because this
 * build links the same libphy.a/librtc.a as ESP-SDR's pinned IDF (checked
 * by md5 against IDF 25fe69f). Calibration helpers are compiled straight
 * from the submodule (rx_recalibration.c, rx_calibration_state.c).
 *
 * iq_radio_leave() restores the receiver to a real WiFi channel.
 */
#include "iq_radio.h"

#include <string.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "heap_memory_layout.h"
#include "rx_bandwidth.h"
#include "rx_lo.h"
#include "rx_recalibration.h"
#include "soc/dport_access.h"
#include "soc/dport_reg.h"
#include "soc/rtc.h"
#include "soc/soc.h"

/* Both SRAM banks incl. instruction aliases; mode 3 exposes all 64 KiB. */
SOC_RESERVE_MEMORY_REGION(0x3ffe8000, 0x3fff8000, esp32_rf_dump);

#define DUMP_CTRL   0x60033d90u
#define DUMP_STATUS 0x60033d94u
#define DUMP_BYTES  0x60033dc4u
#define RX_GAIN     0x3ff5c02cu
#define ADC_CTRL    0x3ff460a0u
#define ADC_ON_FORCE (BIT(9) | BIT(8))
#define CAPACITY    16384u
#define SENTINEL    0xa55aa55au

static uint32_t trigger_time_us;
uint32_t iq_diag_trigger_us(void) { return trigger_time_us; }

static uint32_t* const samples = (uint32_t*)0x3ffe8000;

extern unsigned rom_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void rom_chip_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(int);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_set_rxclk_en(int);
extern void set_chanfreq(unsigned mhz, unsigned mode);
extern void rom_set_rf_freq_offset(unsigned crystal, unsigned mhz, int offset);

static bool active;
static bool rx_ready;
static unsigned frequency_mhz = 2412;
static unsigned calibrated_mhz;
static bool hardware_agc = true;
static unsigned gain_code = 40;
static unsigned gain_max = 127;
static unsigned bandwidth_mhz;
static int rx_filter = -1;
static unsigned filter_saved[2];

static void filter_apply(void)
{
    for (unsigned j = 0; j < 2; j++)
    {
        filter_saved[j] = rom_chip_i2c_readReg(0x67, 1, 1 + j);
        if (rx_filter >= 0)
        {
            rom_chip_i2c_writeReg(0x67, 1, 1 + j,
                                  (filter_saved[j] & ~127u) | (unsigned)rx_filter);
        }
    }
    esp_rom_delay_us(10);
}

static void filter_restore(void)
{
    if (rx_filter >= 0)
    {
        for (unsigned j = 0; j < 2; j++)
        {
            rom_chip_i2c_writeReg(0x67, 1, 1 + j, filter_saved[j]);
        }
    }
}

static void apply_gain(void)
{
    uint32_t gain = REG_READ(RX_GAIN);
    if (hardware_agc || !active)
    {
        REG_CLR_BIT(RX_GAIN, BIT(23));
    }
    else
    {
        REG_WRITE(RX_GAIN, (gain & 0x7fffff) | (gain_code << 24) | BIT(23));
    }
}

static bool is_wifi_channel(unsigned mhz)
{
    return (mhz >= 2412 && mhz <= 2472 && (mhz - 2412) % 5 == 0) || mhz == 2484;
}

static void tune_rx(unsigned mhz)
{
    if (calibrated_mhz != mhz || rx_recalibration_stale())
    {
        rx_recalibrate(mhz);
        calibrated_mhz = mhz;
    }
    rx_lo_plan_t plan = rx_lo_plan(mhz);
    rx_lo_select(false);
    const bool channel = is_wifi_channel(mhz);
    /* Calibrate on a real channel before bypassing the channel mapping; the
     * PHY entry point also covers a channel the WiFi API thinks is set. */
    set_chanfreq(channel ? mhz : 2412, 0);
    if (!channel)
    {
        unsigned xtal = rtc_clk_xtal_freq_get();
        /* Release the pinned capacitor code or the PLL can stay unlocked. */
        unsigned cap = ram_chip_i2c_readReg(0x62, 1, 0);
        ram_chip_i2c_writeReg(0x62, 1, 0, cap & ~0x80u);
        int offset = (plan.offset_khz * 1024 + 500) / 1000;
        rom_set_rf_freq_offset(xtal == 26 ? 1 : xtal == 24 ? 2 : 0, plan.mhz, offset);
    }
}

static void prepare_rx(void)
{
    if (!rx_ready || rx_recalibration_stale())
    {
        tune_rx(frequency_mhz);
        rx_ready = true;
    }
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    /* PBUS work-mode setup releases a forced gain; apply it afterwards. */
    apply_gain();
    if (rx_lo_select(rx_lo_plan(frequency_mhz).alternate))
    {
        esp_rom_delay_us(3000);
    }
}

bool iq_radio_rate_code(uint32_t rate_hz, unsigned* code)
{
    switch (rate_hz)
    {
    case 80000000:
        *code = 0;
        return true;
    case 40000000:
        *code = 1;
        return true;
    case 16000000:
        *code = 6;
        return true;
    default:
        return false;
    }
}

uint32_t iq_radio_rate_hz(unsigned code)
{
    return code == 0 ? 80000000u : code == 1 ? 40000000u : code == 6 ? 16000000u : 0u;
}

bool iq_radio_enter(unsigned mhz)
{
    if (mhz < IQ_RADIO_FREQ_MIN_MHZ || mhz > IQ_RADIO_FREQ_MAX_MHZ)
    {
        return false;
    }
    active = true;
    frequency_mhz = mhz;
    rx_ready = false;
    prepare_rx();
    gain_max = (REG_READ(RX_GAIN) >> 8) & 127;
    if (gain_code > gain_max)
    {
        gain_code = gain_max;
    }
    apply_gain();
    return true;
}

bool iq_radio_tune(unsigned mhz)
{
    if (!active || mhz < IQ_RADIO_FREQ_MIN_MHZ || mhz > IQ_RADIO_FREQ_MAX_MHZ)
    {
        return false;
    }
    if (mhz != frequency_mhz)
    {
        frequency_mhz = mhz;
        rx_ready = false;
    }
    prepare_rx();
    return true;
}

void iq_radio_leave(unsigned channel_mhz)
{
    if (!active)
    {
        return;
    }
    active = false;
    REG_WRITE(DUMP_CTRL, 0);
    rx_lo_select(false);
    apply_gain(); /* releases forced gain: WiFi AGC */
    /* Rebuild the RX DC/gain tables at the WiFi channel, then retune
     * through the PHY so the MAC's channel and the PLL agree again. */
    if (is_wifi_channel(channel_mhz))
    {
        rx_recalibrate(channel_mhz);
        calibrated_mhz = channel_mhz;
        set_chanfreq(channel_mhz, 0);
    }
    rx_ready = false;
}

bool iq_radio_active(void)
{
    return active;
}

unsigned iq_radio_frequency(void)
{
    return frequency_mhz;
}

void iq_radio_set_gain(bool hw, unsigned index)
{
    hardware_agc = hw;
    if (!hw)
    {
        gain_code = index > gain_max ? gain_max : index;
    }
    apply_gain();
}

bool iq_radio_hardware_agc(void)
{
    return hardware_agc;
}

unsigned iq_radio_gain_index(void)
{
    return gain_code;
}

unsigned iq_radio_gain_max(void)
{
    return gain_max;
}

void iq_radio_set_bandwidth(unsigned mhz)
{
    bandwidth_mhz = mhz;
    rx_filter = mhz == 0 ? -1 : (int)rx_bandwidth_dcap(mhz);
}

unsigned iq_radio_bandwidth(void)
{
    return bandwidth_mhz;
}

bool iq_radio_capture(unsigned n, unsigned rate_code, const uint32_t** words,
                      unsigned* elapsed_us, const char** error)
{
    *error = NULL;
    if (!active)
    {
        *error = "receiver not in IQ mode";
        return false;
    }
    if (n < 256 || n > IQ_RADIO_MAX_SAMPLES ||
        (rate_code != 0 && rate_code != 1 && rate_code != 6))
    {
        *error = "samples 256..16380, rate 80/40/16 MS/s";
        return false;
    }
    /* Clock selectors: 0 = 16 MS/s, 1 = 80 MS/s, 2 = 40 MS/s. */
    const unsigned clock = rate_code == 0 ? 1 : rate_code == 1 ? 2 : 0;
    if (rx_recalibration_stale())
    {
        prepare_rx();
    }
    REG_WRITE(DUMP_CTRL, 0);
    for (unsigned j = 0; j < CAPACITY; j++)
    {
        samples[j] = SENTINEL;
    }
    filter_apply();
    /* Force the ADC on during the dump to avoid fixed I/Q runs from PHY
     * gating (ESPARGOS/esp-sdr#22). */
    uint32_t adc_ctrl = DPORT_REG_READ(ADC_CTRL);
    DPORT_REG_WRITE(ADC_CTRL, adc_ctrl | ADC_ON_FORCE);
    esp_rom_delay_us(2);
    uint32_t owner = DPORT_REG_READ(DPORT_IRAM_DRAM_AHB_SEL_REG);
    uint32_t byte_select = REG_READ(DUMP_BYTES);
    DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG,
                    (owner & ~DPORT_MAC_DUMP_MODE_M) | (3 << DPORT_MAC_DUMP_MODE_S));
    REG_WRITE(DUMP_BYTES, 0x03020100);
    uint32_t ctrl = n | ((clock == 0) << 16) | ((clock == 2) << 15);
    REG_WRITE(DUMP_CTRL, ctrl | BIT(31));
    int64_t start = esp_timer_get_time();
    trigger_time_us = (uint32_t)esp_timer_get_time();
    REG_SET_BIT(DUMP_CTRL, BIT(19));
    REG_CLR_BIT(DUMP_CTRL, BIT(19));
    while (!(REG_READ(DUMP_CTRL) & BIT(18)) && esp_timer_get_time() - start < 20000)
    {
    }
    uint32_t result = REG_READ(DUMP_CTRL);
    uint32_t status = REG_READ(DUMP_STATUS);
    unsigned elapsed = (unsigned)(esp_timer_get_time() - start);
    REG_WRITE(DUMP_CTRL, 0);
    esp_rom_delay_us(10);
    DPORT_REG_WRITE(DPORT_IRAM_DRAM_AHB_SEL_REG, owner);
    REG_WRITE(DUMP_BYTES, byte_select);
    DPORT_REG_WRITE(ADC_CTRL, (DPORT_REG_READ(ADC_CTRL) & ~ADC_ON_FORCE) |
                                 (adc_ctrl & ADC_ON_FORCE));
    filter_restore();
    if (!(result & BIT(18)) || (status & 0x7fff) != n)
    {
        *error = "capture timeout";
        return false;
    }
    for (unsigned j = 0; j < CAPACITY; j++)
    {
        if ((j < n && samples[j] == SENTINEL) || (j >= n && samples[j] != SENTINEL))
        {
            *error = "capture memory check failed";
            return false;
        }
    }
    *words = samples;
    *elapsed_us = elapsed;
    return true;
}

size_t iq_radio_scratch(unsigned n, void** out)
{
    unsigned first = (n + 1u) & ~1u; /* 8-byte aligned word index */
    if (first >= CAPACITY)
    {
        *out = NULL;
        return 0;
    }
    *out = samples + first;
    return (CAPACITY - first) * sizeof(uint32_t);
}
