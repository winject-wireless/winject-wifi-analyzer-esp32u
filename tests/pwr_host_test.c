/* Host test for firmware/power_meter/pwr_command.h.
 * Stubs the receiver pieces the header uses and feeds synthetic captures in the
 * ESP32 dump format (I in bits 0-9, -Q in bits 10-19, signed 10-bit).
 *   cc -O2 -o pwr_host_test tests/pwr_host_test.c -lm && ./pwr_host_test */
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAMPLES 16380u
static uint32_t sample_store[16384];
static uint32_t *const samples = sample_store;
static bool hardware_agc = false;
static unsigned gain_code = 40;
static char last_reply[512];
static void vTaskDelay(int t) { (void)t; }
static void reply(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_reply, sizeof last_reply, fmt, ap);
    va_end(ap);
}

/* Synthetic scene, set per test. */
static double scene_noise_dbfs = -50.0, scene_burst_dbfs = -20.0;
static unsigned scene_burst_start = 3000, scene_burst_len = 9600;   /* samples */
static unsigned scene_bursts = 1, scene_gap = 0;
static bool scene_clip = false;

static double gauss(void) {
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

static uint32_t word(int i, int q) {
    if (i > 511) i = 511;
    if (i < -512) i = -512;
    if (q > 511) q = 511;
    if (q < -512) q = -512;
    int nq = -q;
    if (nq > 511) nq = 511;
    return ((uint32_t)i & 0x3ffu) | (((uint32_t)nq & 0x3ffu) << 10);
}

static bool acquire_iq(unsigned n, unsigned source, unsigned clock, unsigned *capture_us) {
    (void)source;
    (void)clock;
    double an = 511.0 * sqrt(pow(10.0, scene_noise_dbfs / 10.0) / 2.0);
    double ab = 511.0 * sqrt(pow(10.0, scene_burst_dbfs / 10.0) / 2.0);
    for (unsigned j = 0; j < n; j++) {
        double i = an * gauss(), q = an * gauss();
        for (unsigned b = 0; b < scene_bursts; b++) {
            unsigned s = scene_burst_start + b * (scene_burst_len + scene_gap);
            if (j >= s && j < s + scene_burst_len) {
                i += ab * gauss();
                q += ab * gauss();
            }
        }
        if (scene_clip && j == scene_burst_start + 100) i = 600;
        samples[j] = word((int)lround(i), (int)lround(q));
    }
    *capture_us = 1000;
    return true;
}

#include "../firmware/power_meter/pwr_command.h"

static double field(const char *key) {
    const char *p = strstr(last_reply, key);
    assert(p);
    return atof(p + strlen(key));
}

static void run(const char *cmd) {
    char line[64];
    snprintf(line, sizeof line, "%s", cmd);
    assert(pwr_command(line));
    printf("%s -> %s", cmd, last_reply);
}

int main(void) {
    srand(1);
    /* One 120 us burst at -20 dBFS over -50 dBFS noise, 80 MS/s. */
    run("PWR 10 0 10 20");
    assert(field("bursts=") == 10);
    assert(fabs(field(" med=") - (-20.0)) < 0.3);
    assert(fabs(field("noise=") - (-50.0)) < 1.5);
    assert(fabs(field("len_us=") - 120.0) < 3.0);
    assert(field("clip_ppm=") == 0);

    /* Linearity: 6 dB more input reads 6 dB more. */
    double a = field(" med=");
    scene_burst_dbfs = -14.0;
    run("PWR 10 0 10 20");
    assert(fabs(field(" med=") - a - 6.0) < 0.3);

    /* Two bursts per capture; 16 MS/s rate; length check in us. */
    scene_burst_dbfs = -25.0;
    scene_bursts = 2;
    scene_burst_len = 1600;   /* 100 us at 16 MS/s */
    scene_gap = 800;
    scene_burst_start = 1000;
    run("PWR 5 6 10 20");
    assert(field("bursts=") == 10);
    assert(fabs(field(" med=") - (-25.0)) < 0.3);
    assert(fabs(field("len_us=") - 100.0) < 4.0);

    /* Bursts shorter than min_burst_us are ignored. */
    run("PWR 5 6 10 200");
    assert(field("bursts=") == 0);

    /* No signal at all. */
    scene_bursts = 0;
    run("PWR 3 0 10 20");
    assert(field("bursts=") == 0);

    /* A burst touching the capture start is partial and skipped. */
    scene_bursts = 1;
    scene_burst_start = 0;
    scene_burst_len = 5000;
    run("PWR 3 0 10 20");
    assert(field("bursts=") == 0);

    /* Clipping is counted. */
    scene_burst_start = 3000;
    scene_clip = true;
    run("PWR 2 0 10 20");
    assert(field("clip_ppm=") > 0);

    /* PWRC: a carrier filling the whole capture (PWR rejects it as edge-cut). */
    scene_clip = false;
    scene_noise_dbfs = -50.0;
    scene_burst_dbfs = -22.0;
    scene_bursts = 1;
    scene_burst_start = 0;
    scene_burst_len = 16384;
    run("PWR 3 0 10 20");
    assert(field("bursts=") == 0);
    run("PWRC 10 6");
    assert(!strncmp(last_reply, "PWRC ", 5));
    assert(fabs(field(" med=") - (-22.0)) < 0.2);   /* noise adds ~0.004 dB */
    scene_burst_dbfs = -32.0;
    run("PWRC 10 1");
    assert(fabs(field(" med=") - (-32.0)) < 0.2);
    char badc[] = "PWRC 0 6";
    assert(pwr_command(badc) && !strncmp(last_reply, "ERR args", 8));

    /* Bad arguments. */
    char bad[] = "PWR 0 0 10 20";
    assert(pwr_command(bad) && !strncmp(last_reply, "ERR args", 8));
    char other[] = "CAPS";
    assert(!pwr_command(other));

    puts("pwr_host_test: OK");
    return 0;
}
