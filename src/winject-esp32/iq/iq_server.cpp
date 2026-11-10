// SPDX-License-Identifier: GPL-3.0-or-later
#include "iq_server.h"

#include "config.h"
#include "iq_radio.h"
#include "mplane_args.h"
#include "mplane_udp.h"
#include "net_proto.h"
#include "netstack.h"

#include <atomic>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern "C" uint32_t iq_diag_trigger_us(void);

namespace iq_server
{
namespace
{

const char* TAG = "iq";

constexpr size_t k_hdr = 44;
constexpr size_t k_fragment = 1400;  // multiple of 2 (IQ8) and 5 (IQ10)
static_assert(k_hdr + k_fragment <= net::k_udp_payload_max, "WJIQ datagram too big");
constexpr uint8_t k_version = 2;
constexpr uint8_t k_flag_last_fragment = 1u << 0;
constexpr uint8_t k_flag_last_in_step = 1u << 1;
constexpr uint8_t k_flag_last_step = 1u << 2;
constexpr uint8_t k_flag_agc = 1u << 3;
constexpr uint8_t k_flag_last_capture = 1u << 4;  // last capture of the job
constexpr uint32_t k_stack = 4096;
constexpr int k_prio = 5;
constexpr unsigned k_default_freq_mhz = 2437;

enum class job_kind : uint8_t
{
    none,
    capture,
    sweep,
};

struct job
{
    job_kind kind = job_kind::none;
    uint16_t id = 0;
    uint16_t n = 16380;
    uint8_t rate_code = 6;
    uint8_t bits = 8;
    uint32_t count = 1;  // captures (capture) or passes (sweep); 0 = until stop
    uint32_t interval_ms = 0;
    uint16_t start_mhz = 0;
    uint16_t stop_mhz = 0;
    uint16_t step_mhz = 0;
    uint16_t dwell = 1;
    uint16_t dwell_ms = 0;
    netstack::udp_peer peer;
    bool peer_resolved = false;
};

netstack* g_net = nullptr;
SemaphoreHandle_t g_lock = nullptr;  // receiver + settings + job
TaskHandle_t g_task = nullptr;
job g_job;
std::atomic<uint32_t> g_gen{0};  // bumped by every start/stop
uint16_t g_next_id = 1;
bool g_ready = false;

unsigned g_freq_mhz = k_default_freq_mhz;
bool g_agc = true;
unsigned g_gain = 40;
unsigned g_bw = 0;

std::atomic<uint32_t> g_captures{0};
std::atomic<uint32_t> g_datagrams{0};
std::atomic<uint32_t> g_tx_errors{0};
std::atomic<uint32_t> g_capture_errors{0};
std::atomic<bool> g_running{false};

uint8_t g_frame[net::k_eth_max];

// --- WJIQ v2 ---------------------------------------------------------------

void put16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t* p, uint32_t v)
{
    put16(p, static_cast<uint16_t>(v));
    put16(p + 2, static_cast<uint16_t>(v >> 16));
}

size_t wire_bytes(unsigned n, unsigned bits)
{
    return bits == 8 ? n * 2u : (n * 20u + 7u) / 8u;
}

// ESP-SDR CAP16 / CAP20 packing of wire bytes [off, off + len) (raw Q field;
// negate Q for conventional IQ). off is a multiple of 2 (IQ8) or 5 (IQ10).
void pack(const uint32_t* w, unsigned n, unsigned bits, size_t off, size_t len,
          uint8_t* out)
{
    if (bits == 8)
    {
        const uint32_t* s = w + off / 2;
        for (size_t b = 0; b + 1 < len; b += 2, ++s)
        {
            out[b] = static_cast<uint8_t>(*s >> 2);
            out[b + 1] = static_cast<uint8_t>(*s >> 12);
        }
        return;
    }
    for (size_t b = 0; b < len; b += 5)
    {
        const unsigned j = static_cast<unsigned>((off + b) / 5 * 2);
        const uint32_t a = w[j] & 0xfffffu;
        const uint32_t c = j + 1 < n ? w[j + 1] & 0xfffffu : 0;
        const uint8_t five[5] = {static_cast<uint8_t>(a), static_cast<uint8_t>(a >> 8),
                                 static_cast<uint8_t>((a >> 16) | (c << 4)),
                                 static_cast<uint8_t>(c >> 4), static_cast<uint8_t>(c >> 12)};
        memcpy(out + b, five, len - b < 5 ? len - b : 5);
    }
}

struct capture_meta
{
    uint32_t capture_id;
    uint32_t timestamp_us;
    uint32_t frequency_khz;
    uint8_t gain_index;
    uint8_t flags;  // step/job flags; fragment flag added per datagram
    uint16_t step;
    uint16_t steps;
};

bool send_capture(const job& j, const uint32_t* words, const capture_meta& m)
{
    const size_t total = wire_bytes(j.n, j.bits);
    uint8_t* h = g_frame + net::k_udp_payload_off;
    uint8_t* payload = h + k_hdr;
    bool ok = true;
    for (size_t off = 0; off < total; off += k_fragment)
    {
        if (!g_net->link_up() || !g_net->connected()) return false;
        const size_t len = total - off < k_fragment ? total - off : k_fragment;
        pack(words, j.n, j.bits, off, len, payload);
        memcpy(h, "WJIQ", 4);
        h[4] = k_version;
        h[5] = m.flags | (off + len >= total ? k_flag_last_fragment : 0);
        h[6] = j.bits;
        h[7] = j.rate_code;
        put32(h + 8, m.capture_id);
        put32(h + 12, j.n);
        put32(h + 16, static_cast<uint32_t>(off));
        put16(h + 20, static_cast<uint16_t>(len));
        put16(h + 22, k_hdr);
        put32(h + 24, esp_rom_crc32_le(0, payload, len));
        put32(h + 28, m.frequency_khz);
        put32(h + 32, m.timestamp_us);
        h[36] = m.gain_index;
        h[37] = static_cast<uint8_t>(g_bw);
        put16(h + 38, j.id);
        put16(h + 40, m.step);
        put16(h + 42, m.steps);
        if (g_net->send_udp(g_frame, k_hdr + len, j.peer, IQ_DATA_PORT))
        {
            g_datagrams.fetch_add(1);
        }
        else
        {
            g_tx_errors.fetch_add(1);
            ok = false;
        }
    }
    return ok;
}

// --- IQ task -----------------------------------------------------------------

bool superseded(uint32_t gen)
{
    return g_gen.load() != gen;
}

// Wait without burning CPU while the Ethernet link/address is unavailable.
// A stop or replacement job remains responsive during an outage.
bool wait_for_network(uint32_t gen)
{
    while (!g_net->link_up() || !g_net->connected())
    {
        if (superseded(gen)) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return !superseded(gen);
}

// One capture under the receiver lock; words stay valid until the next.
bool capture_one(const job& j, unsigned freq_mhz, const uint32_t** words,
                 capture_meta* m)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (freq_mhz != 0 && freq_mhz != iq_radio_frequency())
    {
        iq_radio_tune(freq_mhz);
    }
    unsigned us = 0;
    const char* err = nullptr;
    const bool ok = iq_radio_capture(j.n, j.rate_code, words, &us, &err);
    m->frequency_khz = iq_radio_frequency() * 1000u;
    m->flags = iq_radio_hardware_agc() ? k_flag_agc : 0;
    xSemaphoreGive(g_lock);
    if (!ok)
    {
        g_capture_errors.fetch_add(1);
        ESP_LOGW(TAG, "capture failed: %s", err);
        return false;
    }
    m->timestamp_us = iq_diag_trigger_us();  // experimental trigger-side CPU timestamp
    m->gain_index = static_cast<uint8_t>(((*words)[0] >> 20) & 0xff);
    m->capture_id = g_captures.fetch_add(1) + 1;
    return true;
}

void run_capture_job(const job& j, uint32_t gen)
{
    TickType_t wake = xTaskGetTickCount();
    for (uint32_t k = 0; j.count == 0 || k < j.count; ++k)
    {
        if (!wait_for_network(gen))
        {
            return;
        }
        const uint32_t* words = nullptr;
        capture_meta m = {};
        if (!capture_one(j, 0, &words, &m))
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        m.step = 0;
        m.steps = 1;
        if (j.count != 0 && k + 1 == j.count)
        {
            m.flags |= k_flag_last_capture;
        }
        const bool sent = send_capture(j, words, m);
        if (!sent)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            wake = xTaskGetTickCount();
        }
        else if (j.interval_ms != 0)
        {
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(j.interval_ms));
        }
        else if ((k & 31u) == 31u)
        {
            vTaskDelay(1);  // Give the idle task CPU even under continuous load.
        }
    }
}

void run_sweep_job(const job& j, uint32_t gen)
{
    const unsigned steps = (j.stop_mhz - j.start_mhz) / j.step_mhz + 1;
    for (uint32_t pass = 0; j.count == 0 || pass < j.count; ++pass)
    {
        for (unsigned s = 0; s < steps; ++s)
        {
            const unsigned f = j.start_mhz + s * j.step_mhz;
            TickType_t started = 0;
            bool timing_started = false;
            for (unsigned d = 0; j.dwell_ms || d < j.dwell; ++d)
            {
                if (!wait_for_network(gen))
                {
                    goto done;
                }
                const uint32_t* words = nullptr;
                capture_meta m = {};
                if (!capture_one(j, f, &words, &m))
                {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                m.step = static_cast<uint16_t>(s);
                m.steps = static_cast<uint16_t>(steps);
                if (j.dwell_ms && !timing_started)
                {
                    started = xTaskGetTickCount();
                    timing_started = true;
                }
                const bool last = j.dwell_ms
                    ? (xTaskGetTickCount() - started >= pdMS_TO_TICKS(j.dwell_ms))
                    : (d + 1 == j.dwell);
                if (last)
                {
                    m.flags |= k_flag_last_in_step;
                    if (s + 1 == steps)
                    {
                        m.flags |= k_flag_last_step;
                        if (j.count != 0 && pass + 1 == j.count)
                        {
                            m.flags |= k_flag_last_capture;
                        }
                    }
                }
                if (!send_capture(j, words, m)) vTaskDelay(pdMS_TO_TICKS(10));
                else vTaskDelay(1);
                if (last) break;
            }
        }
    }
done:
    // Back to the user's frequency for the next job.
    xSemaphoreTake(g_lock, portMAX_DELAY);
    iq_radio_tune(g_freq_mhz);
    xSemaphoreGive(g_lock);
}

void task(void*)
{
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(g_lock, portMAX_DELAY);
        job j = g_job;
        const uint32_t gen = g_gen.load();
        xSemaphoreGive(g_lock);
        if (j.kind == job_kind::none)
        {
            continue;
        }
        if (!j.peer_resolved && !g_net->resolve(j.peer.ip, j.peer.port, &j.peer,
                                                pdMS_TO_TICKS(1000)))
        {
            ESP_LOGW(TAG, "job %u: destination unreachable (ARP)", j.id);
            g_tx_errors.fetch_add(1);
            continue;
        }
        g_running.store(true);
        if (j.kind == job_kind::capture)
        {
            run_capture_job(j, gen);
        }
        else
        {
            run_sweep_job(j, gen);
        }
        g_running.store(false);
    }
}

// --- Commands ------------------------------------------------------------------

bool parse_rate(const char* v, uint8_t* code)
{
    unsigned long hz = 0;
    if (!mplane_parse_uint(v, 1, 80000000, &hz))
    {
        return false;
    }
    if (hz <= 80)
    {
        hz *= 1000000;  // MS/s shorthand
    }
    unsigned c = 0;
    if (!iq_radio_rate_code(static_cast<uint32_t>(hz), &c))
    {
        return false;
    }
    *code = static_cast<uint8_t>(c);
    return true;
}

// dst=a.b.c.d[:port]; default: the requester, IQ_DATA_PORT.
bool parse_dst(const mplane_args& args, job* j)
{
    const netstack::udp_peer* req = mplane_udp::instance().current_peer();
    const char* v = args.find("dst");
    if (v == nullptr)
    {
        if (req == nullptr)
        {
            return false;
        }
        j->peer = *req;
        j->peer.port = IQ_DATA_PORT;
        j->peer_resolved = true;  // MAC of the hop the request came from
        return true;
    }
    char ip[24];
    strncpy(ip, v, sizeof(ip) - 1);
    ip[sizeof(ip) - 1] = '\0';
    unsigned long port = IQ_DATA_PORT;
    char* colon = strchr(ip, ':');
    if (colon != nullptr)
    {
        *colon = '\0';
        if (!mplane_parse_uint(colon + 1, 1, 65535, &port))
        {
            return false;
        }
    }
    uint32_t net_order = 0;
    if (!mplane_parse_ipv4(ip, &net_order))
    {
        return false;
    }
    j->peer = {};
    j->peer.ip = __builtin_bswap32(net_order);
    j->peer.port = static_cast<uint16_t>(port);
    j->peer_resolved = false;
    if (req != nullptr && req->ip == j->peer.ip)
    {
        j->peer.mac = req->mac;
        j->peer_resolved = true;
    }
    return true;
}

void print_settings(const char* prefix, mplane_reply& reply)
{
    char gain[8];
    if (g_agc)
    {
        strcpy(gain, "agc");
    }
    else
    {
        snprintf(gain, sizeof(gain), "%u", g_gain);
    }
    reply.print("%s freq=%u gain=%s bw=%u\n", prefix, g_freq_mhz, gain, g_bw);
}

void cmd_info(mplane_reply& reply)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const unsigned gain_max = iq_radio_gain_max();
    char gain[8];
    if (g_agc)
    {
        strcpy(gain, "agc");
    }
    else
    {
        snprintf(gain, sizeof(gain), "%u", g_gain);
    }
    const job j = g_job;
    xSemaphoreGive(g_lock);
    const bool running = g_running.load();
    reply.print("iq_info proto=%u rates=80000000,40000000,16000000 bits=8,10 "
                "max_n=%u freq_min=%u freq_max=%u gain_max=%u freq=%u gain=%s bw=%u\n",
                k_version, IQ_RADIO_MAX_SAMPLES, IQ_RADIO_FREQ_MIN_MHZ,
                IQ_RADIO_FREQ_MAX_MHZ, gain_max, g_freq_mhz, gain, g_bw);
    char dst[24] = "-";
    if (running)
    {
        snprintf(dst, sizeof(dst), "%u.%u.%u.%u:%u", static_cast<unsigned>(j.peer.ip >> 24),
                 static_cast<unsigned>((j.peer.ip >> 16) & 255),
                 static_cast<unsigned>((j.peer.ip >> 8) & 255),
                 static_cast<unsigned>(j.peer.ip & 255), j.peer.port);
    }
    reply.print("iq_info job=%u kind=%s dst=%s captures=%lu datagrams=%lu "
                "tx_errors=%lu capture_errors=%lu\n",
                running ? j.id : 0,
                !running ? "idle" : j.kind == job_kind::sweep ? "sweep" : "capture", dst,
                static_cast<unsigned long>(g_captures.load()),
                static_cast<unsigned long>(g_datagrams.load()),
                static_cast<unsigned long>(g_tx_errors.load()),
                static_cast<unsigned long>(g_capture_errors.load()));
}

void cmd_set(char* args_text, mplane_reply& reply)
{
    static const char* const k_keys[] = {"freq", "gain", "bw", nullptr};
    mplane_args args;
    if (!args.parse(args_text) || !args.only(k_keys))
    {
        reply.nok(mplane_status::invalid);
        return;
    }
    unsigned long freq = g_freq_mhz, bw = g_bw;
    if (args.get_uint("freq", IQ_RADIO_FREQ_MIN_MHZ, IQ_RADIO_FREQ_MAX_MHZ, &freq) ==
            mplane_args::status::invalid ||
        args.get_uint("bw", 0, 67, &bw) == mplane_args::status::invalid ||
        (bw != 0 && bw < 12))
    {
        reply.nok(mplane_status::invalid);
        return;
    }
    bool agc = g_agc;
    unsigned long gain = g_gain;
    const char* g = args.find("gain");
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g != nullptr)
    {
        if (strcasecmp(g, "agc") == 0)
        {
            agc = true;
        }
        else if (mplane_parse_uint(g, 0, iq_radio_gain_max(), &gain))
        {
            agc = false;
        }
        else
        {
            xSemaphoreGive(g_lock);
            reply.nok(mplane_status::invalid);
            return;
        }
    }
    g_agc = agc;
    g_gain = static_cast<unsigned>(gain);
    g_bw = static_cast<unsigned>(bw);
    iq_radio_set_gain(agc, g_gain);
    iq_radio_set_bandwidth(g_bw);
    bool ok = true;
    if (freq != g_freq_mhz)
    {
        ok = iq_radio_tune(static_cast<unsigned>(freq));
        if (ok)
        {
            g_freq_mhz = static_cast<unsigned>(freq);
        }
    }
    xSemaphoreGive(g_lock);
    if (!ok)
    {
        reply.nok(mplane_status::io_error);
        return;
    }
    print_settings("OK iq_set", reply);
}

void start(job& j, mplane_reply& reply, const char* name)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    j.id = g_next_id++;
    if (g_next_id == 0)
    {
        g_next_id = 1;
    }
    g_job = j;
    g_gen.fetch_add(1);
    xSemaphoreGive(g_lock);
    xTaskNotifyGive(g_task);
    reply.print("OK %s id=%u dst=%u.%u.%u.%u:%u n=%u bits=%u rate=%lu\n", name, j.id,
                static_cast<unsigned>(j.peer.ip >> 24),
                static_cast<unsigned>((j.peer.ip >> 16) & 255),
                static_cast<unsigned>((j.peer.ip >> 8) & 255),
                static_cast<unsigned>(j.peer.ip & 255), j.peer.port, j.n, j.bits,
                static_cast<unsigned long>(iq_radio_rate_hz(j.rate_code)));
}

bool parse_common(const mplane_args& args, job* j, unsigned default_n)
{
    unsigned long n = default_n, bits = 8;
    if (args.get_uint("n", 256, IQ_RADIO_MAX_SAMPLES, &n) == mplane_args::status::invalid ||
        args.get_uint("bits", 8, 10, &bits) == mplane_args::status::invalid ||
        (bits != 8 && bits != 10))
    {
        return false;
    }
    j->n = static_cast<uint16_t>(n);
    j->bits = static_cast<uint8_t>(bits);
    j->rate_code = 6;
    const char* rate = args.find("rate");
    if (rate != nullptr && !parse_rate(rate, &j->rate_code))
    {
        return false;
    }
    return parse_dst(args, j);
}

void cmd_capture(char* args_text, mplane_reply& reply)
{
    static const char* const k_keys[] = {"n", "rate", "bits", "count", "interval_ms",
                                         "dst", nullptr};
    mplane_args args;
    job j;
    j.kind = job_kind::capture;
    unsigned long count = 1, interval = 0;
    if (!args.parse(args_text) || !args.only(k_keys) || !parse_common(args, &j, 16380) ||
        args.get_uint("count", 0, 100000000, &count) == mplane_args::status::invalid ||
        args.get_uint("interval_ms", 0, 60000, &interval) == mplane_args::status::invalid)
    {
        reply.nok(mplane_status::invalid);
        return;
    }
    j.count = static_cast<uint32_t>(count);
    j.interval_ms = static_cast<uint32_t>(interval);
    start(j, reply, "iq_capture");
}

void cmd_sweep(char* args_text, mplane_reply& reply)
{
    static const char* const k_keys[] = {"start", "stop", "step", "n",      "rate",
                                         "bits",  "dwell", "dwell_ms", "passes", "dst", nullptr};
    mplane_args args;
    job j;
    j.kind = job_kind::sweep;
    unsigned long start_mhz = 0, stop_mhz = 0, step = 0, dwell = 1, dwell_ms = 0, passes = 1;
    if (!args.parse(args_text) || !args.only(k_keys) || !parse_common(args, &j, 1024) ||
        args.get_uint("start", IQ_RADIO_FREQ_MIN_MHZ, IQ_RADIO_FREQ_MAX_MHZ, &start_mhz) !=
            mplane_args::status::ok ||
        args.get_uint("stop", IQ_RADIO_FREQ_MIN_MHZ, IQ_RADIO_FREQ_MAX_MHZ, &stop_mhz) !=
            mplane_args::status::ok ||
        args.get_uint("step", 1, 80, &step) != mplane_args::status::ok ||
        args.get_uint("dwell", 1, 256, &dwell) == mplane_args::status::invalid ||
        args.get_uint("dwell_ms", 1, 1000, &dwell_ms) == mplane_args::status::invalid ||
        (args.find("dwell") && args.find("dwell_ms")) ||
        args.get_uint("passes", 0, 1000000, &passes) == mplane_args::status::invalid ||
        stop_mhz < start_mhz || (stop_mhz - start_mhz) / step + 1 > 6000)
    {
        reply.nok(mplane_status::invalid);
        return;
    }
    j.start_mhz = static_cast<uint16_t>(start_mhz);
    j.stop_mhz = static_cast<uint16_t>(stop_mhz);
    j.step_mhz = static_cast<uint16_t>(step);
    j.dwell = static_cast<uint16_t>(dwell);
    j.dwell_ms = static_cast<uint16_t>(dwell_ms);
    j.count = static_cast<uint32_t>(passes);
    start(j, reply, "iq_sweep");
}

void cmd_stop(mplane_reply& reply)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_job.kind = job_kind::none;
    g_gen.fetch_add(1);
    xSemaphoreGive(g_lock);
    reply.write("OK iq_stop\n");
}

}  // namespace

bool begin(netstack& net)
{
    g_net = &net;
    g_lock = xSemaphoreCreateMutex();
    if (g_lock == nullptr)
    {
        return false;
    }
    if (!iq_radio_enter(g_freq_mhz))
    {
        ESP_LOGE(TAG, "IQ receiver start failed");
        return false;
    }
    iq_radio_set_gain(g_agc, g_gain);
    if (xTaskCreatePinnedToCore(task, "iq", k_stack, nullptr, k_prio, &g_task,
                                APP_TASK_CORE) != pdPASS)
    {
        return false;
    }
    g_ready = true;
    ESP_LOGI(TAG, "IQ server up: %u MHz, gain_max %u, data -> UDP %u", g_freq_mhz,
             iq_radio_gain_max(), IQ_DATA_PORT);
    return true;
}

bool handle_line(void*, char* line, mplane_reply& reply)
{
    char* args = line;
    while (*args != '\0' && *args != ' ' && *args != '\t')
    {
        ++args;
    }
    if (*args != '\0')
    {
        *args++ = '\0';
    }
    if (!g_ready)
    {
        if (strncmp(line, "iq_", 3) == 0)
        {
            reply.nok(mplane_status::no_device);
            return true;
        }
        return false;
    }
    if (strcmp(line, "iq_info") == 0)
    {
        cmd_info(reply);
    }
    else if (strcmp(line, "iq_set") == 0)
    {
        cmd_set(args, reply);
    }
    else if (strcmp(line, "iq_capture") == 0)
    {
        cmd_capture(args, reply);
    }
    else if (strcmp(line, "iq_sweep") == 0)
    {
        cmd_sweep(args, reply);
    }
    else if (strcmp(line, "iq_stop") == 0)
    {
        cmd_stop(reply);
    }
    else
    {
        return false;
    }
    return true;
}

}  // namespace iq_server
