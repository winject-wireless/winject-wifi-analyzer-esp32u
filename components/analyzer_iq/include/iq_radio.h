/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WINJECT_ANALYZER_IQ_RADIO_H_
#define WINJECT_ANALYZER_IQ_RADIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ESP-SDR RF dump on the original ESP32 (firmware/esp-sdr, targets/esp32):
// the WiFi MAC keeps running in promiscuous mode while the dump engine
// copies baseband I/Q into the reserved 64 KiB bank at 0x3ffe8000.
//
// Words are IQ10: I in bits 0..9, raw Q in bits 10..19 (conventional
// Q = -raw Q), gain index in bits 20..27. All calls from one task.

#define IQ_RADIO_MAX_SAMPLES 16380u
#define IQ_RADIO_FREQ_MIN_MHZ 100u
#define IQ_RADIO_FREQ_MAX_MHZ 6000u

// Rate codes as in ESP-SDR: 0 = 80, 1 = 40, 6 = 16 MS/s.
bool iq_radio_rate_code(uint32_t rate_hz, unsigned* code);
uint32_t iq_radio_rate_hz(unsigned code);

// Leaves WiFi reception, powers the receiver for capture and tunes to mhz.
bool iq_radio_enter(unsigned mhz);
// Retunes (fresh RX DC calibration at mhz). Only between captures.
bool iq_radio_tune(unsigned mhz);
// Restores WiFi reception on channel_mhz (a WiFi channel center).
void iq_radio_leave(unsigned channel_mhz);
bool iq_radio_active(void);
unsigned iq_radio_frequency(void);

void iq_radio_set_gain(bool hardware_agc, unsigned index);
bool iq_radio_hardware_agc(void);
unsigned iq_radio_gain_index(void);
unsigned iq_radio_gain_max(void);
// Analog low-pass: 0 = PHY default, else approx. full -3 dB width in MHz
// (12..67), applied around each capture only.
void iq_radio_set_bandwidth(unsigned mhz);
unsigned iq_radio_bandwidth(void);

// Captures n samples (256..IQ_RADIO_MAX_SAMPLES). *words stays valid until
// the next capture. false: *error names the failure.
bool iq_radio_capture(unsigned n, unsigned rate_code, const uint32_t** words,
                      unsigned* elapsed_us, const char** error);

// CPU-owned scratch in the dump bank after a capture of n samples: the words
// past n are free until the next capture (which re-fills them). Returns the
// usable bytes at *out (8-byte aligned).
size_t iq_radio_scratch(unsigned n, void** out);

static inline int iq_radio_i(uint32_t w)
{
    int v = (int)(w & 0x3ffu);
    return v & 0x200 ? v - 1024 : v;
}

// Conventional Q (raw field negated).
static inline int iq_radio_q(uint32_t w)
{
    int v = (int)((w >> 10) & 0x3ffu);
    return -(v & 0x200 ? v - 1024 : v);
}

#ifdef __cplusplus
}
#endif

#endif  // WINJECT_ANALYZER_IQ_RADIO_H_
