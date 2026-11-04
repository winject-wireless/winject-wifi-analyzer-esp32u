/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * esp32-power-meter: on-chip burst power measurement for ESP-SDR (ESP32).
 * Included by targets/esp32/receiver.c (see firmware/patches); uses its
 * acquire_iq(), samples[], MAX_SAMPLES, reply(), hardware_agc and gain_code.
 *
 *   PWR <captures> <rate> <threshold_db> <min_burst_us>
 *
 * Captures <captures> snapshots of MAX_SAMPLES at rate code <rate> (0 = 80,
 * 1 = 40, 6 = 16 MS/s), finds bursts whose 1 us moving-average power rises
 * <threshold_db> above the capture's noise floor and last at least
 * <min_burst_us>, and replies with one line:
 *
 *   PWR caps=<n> bursts=<n> med=<dBFS> p10=<dBFS> p90=<dBFS> min=<dBFS>
 *       max=<dBFS> peak=<dBFS> noise=<dBFS> clip=<fraction> len_us=<median>
 *       gain=<MANUAL n|HARDWARE>
 *
 * 0 dBFS is a sample at the 10-bit converter's full scale (|I|,|Q| = 511 on a
 * circle of radius 511). Burst power is the mean sample power over the burst
 * with 10 % trimmed from each end (ramp-up/down and smoothing lag). With a
 * fixed (MANUAL) gain the readings are linear in input power; dBm needs one
 * calibration offset per gain index and frequency. No samples leave the chip.
 */
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PWR_MAX_BURSTS   1024u
#define PWR_MAX_CAPS     500u
#define PWR_HIST_LO_DB   (-90.0f)
#define PWR_HIST_STEP_DB 0.25f
#define PWR_HIST_BINS    400u            /* -90 .. +10 dBFS */
#define PWR_CLIP_LEVEL   501             /* |I| or |Q| at the rail (98 %) */
#define PWR_CLIP_FLAG    0x80000000u
#define PWR_FS2          (511.0f * 511.0f)

static float pwr_burst_db[PWR_MAX_BURSTS];
static float pwr_burst_len_us[PWR_MAX_BURSTS];
static float pwr_noise_db[PWR_MAX_CAPS];
static uint16_t pwr_hist[PWR_HIST_BINS];

static int pwr_cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static float pwr_quantile(float *v, unsigned n, float q) {
    if (!n) return NAN;
    qsort(v, n, sizeof(float), pwr_cmp_float);
    unsigned k = (unsigned)(q * (float)(n - 1) + 0.5f);
    return v[k < n ? k : n - 1];
}

static float pwr_db(float p) {
    return 10.0f * log10f(p > 1e-12f ? p : 1e-12f);
}

static int pwr_sext10(uint32_t v) {
    v &= 0x3ffu;
    return v & 0x200u ? (int)v - 1024 : (int)v;
}

/* Power and clip flag per sample, in place: bits 0-19 = I^2 + Q^2. */
static unsigned pwr_prepare(unsigned n) {
    unsigned clipped = 0;
    for (unsigned j = 0; j < n; j++) {
        uint32_t w = samples[j];
        int i = pwr_sext10(w), q = pwr_sext10(w >> 10);
        uint32_t p = (uint32_t)(i * i + q * q);
        if (abs(i) >= PWR_CLIP_LEVEL || abs(q) >= PWR_CLIP_LEVEL) {
            p |= PWR_CLIP_FLAG;
            clipped++;
        }
        samples[j] = p;
    }
    return clipped;
}

#define PWR_P(j) (samples[j] & ~PWR_CLIP_FLAG)

/* Noise floor: 20th percentile of the moving-average power (holds while bursts
 * fill most of the capture). Returns linear power relative to full scale. */
static float pwr_noise(unsigned n, unsigned win) {
    memset(pwr_hist, 0, sizeof(pwr_hist));
    uint32_t sum = 0;
    unsigned counted = 0;
    for (unsigned j = 0; j < n; j++) {
        sum += PWR_P(j);
        if (j >= win) sum -= PWR_P(j - win);
        if (j + 1 < win) continue;
        float db = pwr_db((float)sum / (float)win / PWR_FS2);
        int bin = (int)((db - PWR_HIST_LO_DB) / PWR_HIST_STEP_DB);
        if (bin < 0) bin = 0;
        if (bin >= (int)PWR_HIST_BINS) bin = PWR_HIST_BINS - 1;
        if (pwr_hist[bin] < UINT16_MAX) pwr_hist[bin]++;
        counted++;
    }
    unsigned target = counted / 5, acc = 0;
    for (unsigned b = 0; b < PWR_HIST_BINS; b++) {
        acc += pwr_hist[b];
        if (acc > target)
            return powf(10.0f, (PWR_HIST_LO_DB + (b + 0.5f) * PWR_HIST_STEP_DB) / 10.0f);
    }
    return 1.0f;
}

typedef struct {
    unsigned bursts;
    float peak_db;
} pwr_capture_result_t;

/* Find bursts in one prepared capture; append their power and length. */
static pwr_capture_result_t pwr_bursts(unsigned n, unsigned win, float noise, float threshold_db,
                                       unsigned min_len, float samples_per_us, unsigned *stored) {
    pwr_capture_result_t r = {0, -200.0f};
    const float level = noise * powf(10.0f, threshold_db / 10.0f) * PWR_FS2 * (float)win;
    uint32_t sum = 0;
    unsigned start = 0;
    bool in_burst = false;
    for (unsigned j = 0; j <= n; j++) {
        bool above = false;
        if (j < n) {
            sum += PWR_P(j);
            if (j >= win) sum -= PWR_P(j - win);
            above = j + 1 >= win && (float)sum > level;
        }
        if (above && !in_burst) {
            in_burst = true;
            /* The trailing window lags the rise by up to win samples. */
            start = j + 1 >= win ? j + 1 - win : 0;
        } else if (!above && in_burst) {
            in_burst = false;
            unsigned end = j;                   /* exclusive; smoothing tail */
            if (start == 0 || j == n) continue;   /* cut by the capture edge */
            unsigned len = end - start;
            if (len < min_len) continue;
            unsigned cut = len / 10;
            double acc = 0;
            uint32_t peak = 0;
            for (unsigned k = start + cut; k < end - cut; k++) {
                uint32_t p = PWR_P(k);
                acc += p;
                if (p > peak) peak = p;
            }
            float db = pwr_db((float)(acc / (double)(len - 2 * cut)) / PWR_FS2);
            float pk = pwr_db((float)peak / PWR_FS2);
            if (pk > r.peak_db) r.peak_db = pk;
            r.bursts++;
            if (*stored < PWR_MAX_BURSTS) {
                pwr_burst_db[*stored] = db;
                pwr_burst_len_us[*stored] = (float)len / samples_per_us;
                (*stored)++;
            }
        }
    }
    return r;
}

static void pwr_fmt_db(char *out, size_t size, float v) {
    if (isnan(v)) {
        snprintf(out, size, "nan");
        return;
    }
    int c = (int)lroundf(v * 100.0f);
    snprintf(out, size, "%s%d.%02d", c < 0 ? "-" : "", abs(c) / 100, abs(c) % 100);
}

/* PWRC <captures> <rate>: mean power of whole snapshots, for continuous
 * signals (a CW reference source fills every capture, so PWR would reject it
 * as edge-cut). Replies
 *   PWRC caps=<n> med=<dBFS> p10=<dBFS> p90=<dBFS> clip_ppm=<n> gain=<...>
 * Tune off the tone (e.g. 3 MHz): DC correction and LO leakage sit at 0 Hz. */
static bool pwrc_command(char *line) {
    unsigned caps, rate;
    char extra;
    if (sscanf(line, "PWRC %u %u %c", &caps, &rate, &extra) != 2 || caps == 0 || caps > PWR_MAX_CAPS ||
        (rate != 0 && rate != 1 && rate != 6)) {
        reply("ERR args\n");
        return true;
    }
    const unsigned clock = rate == 0 ? 1 : rate == 1 ? 2 : 0;
    const unsigned n = MAX_SAMPLES;
    uint64_t clipped = 0;
    for (unsigned c = 0; c < caps; c++) {
        unsigned elapsed;
        if (!acquire_iq(n, 0, clock, &elapsed)) return true;   /* ERR already sent */
        clipped += pwr_prepare(n);
        double acc = 0;
        for (unsigned j = 0; j < n; j++) acc += PWR_P(j);
        pwr_noise_db[c] = pwr_db((float)(acc / (double)n) / PWR_FS2);
        vTaskDelay(1);
    }
    float med = pwr_quantile(pwr_noise_db, caps, 0.5f);   /* sorts */
    float p10 = pwr_noise_db[(unsigned)(0.1f * (caps - 1) + 0.5f)];
    float p90 = pwr_noise_db[(unsigned)(0.9f * (caps - 1) + 0.5f)];
    char s_med[16], s_p10[16], s_p90[16], gain[24];
    pwr_fmt_db(s_med, sizeof s_med, med);
    pwr_fmt_db(s_p10, sizeof s_p10, p10);
    pwr_fmt_db(s_p90, sizeof s_p90, p90);
    if (hardware_agc)
        snprintf(gain, sizeof gain, "HARDWARE");
    else
        snprintf(gain, sizeof gain, "MANUAL_%u", gain_code);
    reply("PWRC caps=%u med=%s p10=%s p90=%s clip_ppm=%u gain=%s\n", caps, s_med, s_p10, s_p90,
          (unsigned)((clipped * 1000000ull) / ((uint64_t)caps * n)), gain);
    return true;
}

static bool pwr_command(char *line) {
    unsigned caps, rate, threshold, min_us;
    char extra;
    if (!strncmp(line, "PWRC ", 5)) return pwrc_command(line);
    if (strncmp(line, "PWR ", 4)) return false;
    if (sscanf(line, "PWR %u %u %u %u %c", &caps, &rate, &threshold, &min_us, &extra) != 4 ||
        caps == 0 || caps > PWR_MAX_CAPS || (rate != 0 && rate != 1 && rate != 6) ||
        threshold == 0 || threshold > 60 || min_us > 5000) {
        reply("ERR args\n");
        return true;
    }
    const unsigned clock = rate == 0 ? 1 : rate == 1 ? 2 : 0;
    const float samples_per_us = rate == 0 ? 80.0f : rate == 1 ? 40.0f : 16.0f;
    const unsigned win = (unsigned)samples_per_us;        /* 1 us */
    const unsigned n = MAX_SAMPLES;
    const unsigned min_len = (unsigned)(min_us * samples_per_us);
    unsigned stored = 0, total_bursts = 0;
    uint64_t clipped = 0;
    float peak_db = -200.0f;
    for (unsigned c = 0; c < caps; c++) {
        unsigned elapsed;
        if (!acquire_iq(n, 0, clock, &elapsed)) return true;   /* ERR already sent */
        clipped += pwr_prepare(n);
        float noise = pwr_noise(n, win);
        pwr_noise_db[c] = pwr_db(noise);
        pwr_capture_result_t r = pwr_bursts(n, win, noise, (float)threshold, min_len,
                                            samples_per_us, &stored);
        total_bursts += r.bursts;
        if (r.peak_db > peak_db) peak_db = r.peak_db;
        vTaskDelay(1);
    }
    float noise_med = pwr_quantile(pwr_noise_db, caps, 0.5f);
    float len_med = pwr_quantile(pwr_burst_len_us, stored, 0.5f);
    float med = pwr_quantile(pwr_burst_db, stored, 0.5f);   /* sorts pwr_burst_db */
    float p10 = stored ? pwr_burst_db[(unsigned)(0.1f * (stored - 1) + 0.5f)] : NAN;
    float p90 = stored ? pwr_burst_db[(unsigned)(0.9f * (stored - 1) + 0.5f)] : NAN;
    float mn = stored ? pwr_burst_db[0] : NAN, mx = stored ? pwr_burst_db[stored - 1] : NAN;
    char s_med[16], s_p10[16], s_p90[16], s_mn[16], s_mx[16], s_pk[16], s_noise[16], s_len[16];
    pwr_fmt_db(s_med, sizeof s_med, med);
    pwr_fmt_db(s_p10, sizeof s_p10, p10);
    pwr_fmt_db(s_p90, sizeof s_p90, p90);
    pwr_fmt_db(s_mn, sizeof s_mn, mn);
    pwr_fmt_db(s_mx, sizeof s_mx, mx);
    pwr_fmt_db(s_pk, sizeof s_pk, stored ? peak_db : NAN);
    pwr_fmt_db(s_noise, sizeof s_noise, noise_med);
    pwr_fmt_db(s_len, sizeof s_len, len_med);
    unsigned clip_ppm = (unsigned)((clipped * 1000000ull) / ((uint64_t)caps * n));
    char gain[24];
    if (hardware_agc)
        snprintf(gain, sizeof gain, "HARDWARE");
    else
        snprintf(gain, sizeof gain, "MANUAL_%u", gain_code);
    reply("PWR caps=%u bursts=%u med=%s p10=%s p90=%s min=%s max=%s peak=%s noise=%s clip_ppm=%u "
          "len_us=%s gain=%s\n",
          caps, total_bursts, s_med, s_p10, s_p90, s_mn, s_mx, s_pk, s_noise, clip_ppm, s_len, gain);
    return true;
}
