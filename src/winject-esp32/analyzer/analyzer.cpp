#include "analyzer.h"

#include "config.h"
#include "config_types.h"
#include "wifi.h"

#include <atomic>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern const uint8_t web_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t web_index_html_end[] asm("_binary_index_html_end");
extern const uint8_t web_app_js_start[] asm("_binary_app_js_start");
extern const uint8_t web_app_js_end[] asm("_binary_app_js_end");
extern const uint8_t web_style_css_start[] asm("_binary_style_css_start");
extern const uint8_t web_style_css_end[] asm("_binary_style_css_end");

namespace analyzer
{
namespace
{

const char* TAG = "analyzer";

constexpr size_t k_frame_slots = 10;
// Longest MPDU kept per record; longer frames are sent truncated (flagged).
constexpr size_t k_frame_cap = 1600;
constexpr size_t k_csi_slots = 8;
// L-LTF only: 64 subcarriers x (imag, real) int8.
constexpr size_t k_csi_cap = 128;
constexpr size_t k_max_clients = 3;
constexpr size_t k_recent = 16;
constexpr size_t k_csi_pending = 4;
constexpr int64_t k_csi_match_wait_us = 30 * 1000;
constexpr int64_t k_power_period_us = 200 * 1000;
constexpr int64_t k_stats_period_us = 1000 * 1000;
constexpr int k_sender_prio = 5;
constexpr uint32_t k_sender_stack = 6144;
// Event JSON: base64(k_frame_cap) + metadata.
constexpr size_t k_json_cap = ((k_frame_cap + 2) / 3) * 4 + 512;

// Bits follow wifi_promiscuous_pkt_type_t (MGMT 0, CTRL 1, DATA 2, MISC 3).
constexpr uint8_t k_type_mgmt = 1u << WIFI_PKT_MGMT;
constexpr uint8_t k_type_ctrl = 1u << WIFI_PKT_CTRL;
constexpr uint8_t k_type_data = 1u << WIFI_PKT_DATA;
constexpr uint8_t k_type_misc = 1u << WIFI_PKT_MISC;
constexpr uint8_t k_type_default = k_type_mgmt | k_type_data | k_type_misc;

struct frame_rec
{
    int64_t time_us;
    uint32_t rx_ts;
    int8_t rssi;
    int8_t noise;
    uint8_t channel;
    uint8_t secondary;
    uint8_t rate;
    uint8_t sig_mode;
    uint8_t mcs;
    uint8_t cwb;
    uint8_t aggregation;
    uint8_t stbc;
    uint8_t fec_coding;
    uint8_t sgi;
    uint8_t rx_state;
    uint8_t pkt_type;
    uint16_t sig_len;
    uint16_t len;
    bool truncated;
    uint8_t data[k_frame_cap];
};

struct csi_rec
{
    int64_t time_us;
    uint32_t rx_ts;
    uint8_t mac[6];
    int8_t rssi;
    int8_t noise;
    uint8_t channel;
    uint16_t len;
    int8_t buf[k_csi_cap];
};

struct recent_frame
{
    uint32_t rx_ts;
    uint32_t seq;
    bool valid;
};

struct state
{
    httpd_handle_t server = nullptr;
    wifi* radio = nullptr;

    frame_rec frames[k_frame_slots];
    QueueHandle_t frame_free = nullptr;
    QueueHandle_t frame_ready = nullptr;
    csi_rec csi[k_csi_slots];
    QueueHandle_t csi_free = nullptr;
    QueueHandle_t csi_ready = nullptr;

    SemaphoreHandle_t clients_lock = nullptr;
    int clients[k_max_clients] = {-1, -1, -1};
    std::atomic<uint8_t> client_count{0};

    // Config shared with the WiFi callbacks.
    std::atomic<uint8_t> type_mask{k_type_default};
    std::atomic<int8_t> min_rssi{-128};
    std::atomic<bool> fcs_ok_only{false};
    std::atomic<uint64_t> mac{0};
    std::atomic<bool> csi_enabled{true};
    std::atomic<uint32_t> epoch{1};

    std::atomic<uint32_t> dropped_frames{0};
    std::atomic<uint32_t> dropped_csi{0};
    std::atomic<uint32_t> dropped_ws{0};
    std::atomic<uint32_t> tapped{0};

    SemaphoreHandle_t config_lock = nullptr;
};

state g;

bool streaming()
{
    return g.client_count.load(std::memory_order_relaxed) != 0;
}

// --- WiFi callbacks (WiFi task) --------------------------------------------

bool pop_slot(QueueHandle_t free_q, QueueHandle_t ready_q,
              std::atomic<uint32_t>& dropped, uint8_t* slot)
{
    if (xQueueReceive(free_q, slot, 0) == pdTRUE)
    {
        return true;
    }
    // Keep the newest records: recycle the oldest queued one.
    dropped.fetch_add(1, std::memory_order_relaxed);
    return xQueueReceive(ready_q, slot, 0) == pdTRUE;
}

void frame_tap(const wifi_promiscuous_pkt_t* pkt,
               wifi_promiscuous_pkt_type_t type)
{
    if (!streaming() || pkt == nullptr || type > WIFI_PKT_MISC)
    {
        return;
    }
    if ((g.type_mask.load(std::memory_order_relaxed) & (1u << type)) == 0)
    {
        return;
    }
    const wifi_pkt_rx_ctrl_t& rx = pkt->rx_ctrl;
    if (rx.rssi < g.min_rssi.load(std::memory_order_relaxed))
    {
        return;
    }
    if (rx.rx_state != 0 && g.fcs_ok_only.load(std::memory_order_relaxed))
    {
        return;
    }
    const size_t mpdu_len =
        rx.sig_len > WIFI_FCS_LEN ? rx.sig_len - WIFI_FCS_LEN : 0;
    const mac_filter mf =
        mac_filter_unpack(g.mac.load(std::memory_order_relaxed));
    if (mf.enabled)
    {
        // Addr1 at 4; Addr2/Addr3 only exist in longer headers.
        bool hit = mpdu_len >= 10 && mf.matches(pkt->payload + 4);
        hit = hit || (mpdu_len >= 16 && mf.matches(pkt->payload + 10));
        hit = hit || (mpdu_len >= 22 && mf.matches(pkt->payload + 16));
        if (!hit)
        {
            return;
        }
    }

    uint8_t slot = 0;
    if (!pop_slot(g.frame_free, g.frame_ready, g.dropped_frames, &slot))
    {
        return;
    }
    frame_rec& r = g.frames[slot];
    r.time_us = esp_timer_get_time();
    r.rx_ts = rx.timestamp;
    r.rssi = static_cast<int8_t>(rx.rssi);
    r.noise = static_cast<int8_t>(rx.noise_floor);
    r.channel = rx.channel;
    r.secondary = rx.secondary_channel;
    r.rate = rx.rate;
    r.sig_mode = rx.sig_mode;
    r.mcs = rx.mcs;
    r.cwb = rx.cwb;
    r.aggregation = rx.aggregation;
    r.stbc = rx.stbc;
    r.fec_coding = rx.fec_coding;
    r.sgi = rx.sgi;
    r.rx_state = static_cast<uint8_t>(rx.rx_state);
    r.pkt_type = static_cast<uint8_t>(type);
    r.sig_len = rx.sig_len;
    r.truncated = mpdu_len > k_frame_cap;
    r.len = static_cast<uint16_t>(r.truncated ? k_frame_cap : mpdu_len);
    memcpy(r.data, pkt->payload, r.len);
    g.tapped.fetch_add(1, std::memory_order_relaxed);
    xQueueSend(g.frame_ready, &slot, 0);
}

void csi_cb(void* ctx, wifi_csi_info_t* info)
{
    (void)ctx;
    if (!streaming() || info == nullptr || info->buf == nullptr ||
        !g.csi_enabled.load(std::memory_order_relaxed))
    {
        return;
    }
    uint8_t slot = 0;
    if (!pop_slot(g.csi_free, g.csi_ready, g.dropped_csi, &slot))
    {
        return;
    }
    csi_rec& r = g.csi[slot];
    r.time_us = esp_timer_get_time();
    r.rx_ts = info->rx_ctrl.timestamp;
    memcpy(r.mac, info->mac, sizeof(r.mac));
    r.rssi = static_cast<int8_t>(info->rx_ctrl.rssi);
    r.noise = static_cast<int8_t>(info->rx_ctrl.noise_floor);
    r.channel = info->rx_ctrl.channel;
    r.len = info->len < k_csi_cap ? info->len : k_csi_cap;
    memcpy(r.buf, info->buf, r.len);
    if (info->first_word_invalid && r.len >= 4)
    {
        memset(r.buf, 0, 4);
    }
    xQueueSend(g.csi_ready, &slot, 0);
}

// --- WebSocket clients -----------------------------------------------------

void add_client(int fd)
{
    xSemaphoreTake(g.clients_lock, portMAX_DELAY);
    int free_idx = -1;
    for (size_t i = 0; i < k_max_clients; ++i)
    {
        if (g.clients[i] == fd)
        {
            xSemaphoreGive(g.clients_lock);
            return;
        }
        if (g.clients[i] < 0 && free_idx < 0)
        {
            free_idx = static_cast<int>(i);
        }
    }
    if (free_idx >= 0)
    {
        g.clients[free_idx] = fd;
        g.client_count.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGI(TAG, "ws client fd=%d (%u)", fd,
                 static_cast<unsigned>(g.client_count.load()));
    }
    else
    {
        ESP_LOGW(TAG, "ws client fd=%d rejected: %u clients max", fd,
                 static_cast<unsigned>(k_max_clients));
    }
    xSemaphoreGive(g.clients_lock);
}

void drop_client_locked(size_t i)
{
    ESP_LOGI(TAG, "ws client fd=%d gone", g.clients[i]);
    g.clients[i] = -1;
    g.client_count.fetch_sub(1, std::memory_order_relaxed);
}

void broadcast(const char* text, size_t len)
{
    httpd_ws_frame_t frame = {};
    frame.final = true;
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(text));
    frame.len = len;
    // Snapshot the fds: httpd_ws_send_data waits for the httpd task, which
    // may itself be in add_client(), so never send under clients_lock.
    int fds[k_max_clients];
    xSemaphoreTake(g.clients_lock, portMAX_DELAY);
    memcpy(fds, g.clients, sizeof(fds));
    xSemaphoreGive(g.clients_lock);

    bool failed[k_max_clients] = {};
    bool any_failed = false;
    for (size_t i = 0; i < k_max_clients; ++i)
    {
        if (fds[i] < 0)
        {
            continue;
        }
        // The fd may have been closed and reused by a plain HTTP request.
        if (httpd_ws_get_fd_info(g.server, fds[i]) !=
                HTTPD_WS_CLIENT_WEBSOCKET ||
            httpd_ws_send_data(g.server, fds[i], &frame) != ESP_OK)
        {
            failed[i] = any_failed = true;
        }
    }
    if (!any_failed)
    {
        return;
    }
    xSemaphoreTake(g.clients_lock, portMAX_DELAY);
    for (size_t i = 0; i < k_max_clients; ++i)
    {
        if (failed[i] && g.clients[i] == fds[i])
        {
            g.dropped_ws.fetch_add(1, std::memory_order_relaxed);
            drop_client_locked(i);
        }
    }
    xSemaphoreGive(g.clients_lock);
}

// --- Formatting ------------------------------------------------------------

size_t base64(const uint8_t* in, size_t len, char* out)
{
    static const char k_tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char* p = out;
    size_t i = 0;
    for (; i + 2 < len; i += 3)
    {
        const uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        *p++ = k_tab[(v >> 18) & 63];
        *p++ = k_tab[(v >> 12) & 63];
        *p++ = k_tab[(v >> 6) & 63];
        *p++ = k_tab[v & 63];
    }
    if (i < len)
    {
        uint32_t v = in[i] << 16;
        if (i + 1 < len)
        {
            v |= in[i + 1] << 8;
        }
        *p++ = k_tab[(v >> 18) & 63];
        *p++ = k_tab[(v >> 12) & 63];
        *p++ = i + 1 < len ? k_tab[(v >> 6) & 63] : '=';
        *p++ = '=';
    }
    *p = '\0';
    return static_cast<size_t>(p - out);
}

const char* pkt_type_name(uint8_t type)
{
    switch (type)
    {
    case WIFI_PKT_MGMT:
        return "mgmt";
    case WIFI_PKT_CTRL:
        return "ctrl";
    case WIFI_PKT_DATA:
        return "data";
    default:
        return "misc";
    }
}

uint16_t channel_mhz(uint8_t channel)
{
    if (channel == 14)
    {
        return 2484;
    }
    return static_cast<uint16_t>(2407 + 5 * channel);
}

uint8_t current_channel()
{
    radio_config cfg;
    if (g.radio != nullptr && g.radio->config(&cfg))
    {
        return cfg.channel;
    }
    return 0;
}

char g_json[k_json_cap];

void send_frame(const frame_rec& r, uint32_t seq)
{
    int n = snprintf(
        g_json, sizeof(g_json),
        "{\"event\":\"frame\",\"epoch\":%u,\"sequence\":%u,\"time_us\":%lld,"
        "\"data\":{\"sequence\":%u,\"time_us\":%lld,\"rx_timestamp_us\":%u,"
        "\"pkt_type\":\"%s\",\"rssi\":%d,\"noise\":%d,\"channel\":%u,"
        "\"secondary\":%u,\"rate\":%u,\"sig_mode\":%u,\"mcs\":%u,\"cwb\":%u,"
        "\"aggregation\":%u,\"stbc\":%u,\"fec_coding\":%u,\"sgi\":%u,"
        "\"rx_state\":%u,\"fcs\":\"%s\",\"sig_len\":%u,\"length\":%u,"
        "\"truncated\":%s,\"mpdu\":\"",
        static_cast<unsigned>(g.epoch.load()), static_cast<unsigned>(seq),
        static_cast<long long>(r.time_us), static_cast<unsigned>(seq),
        static_cast<long long>(r.time_us), static_cast<unsigned>(r.rx_ts),
        pkt_type_name(r.pkt_type), r.rssi, r.noise, r.channel, r.secondary,
        r.rate, r.sig_mode, r.mcs, r.cwb, r.aggregation, r.stbc, r.fec_coding,
        r.sgi, r.rx_state, r.rx_state == 0 ? "ok" : "fail", r.sig_len, r.len,
        r.truncated ? "true" : "false");
    if (n < 0 || static_cast<size_t>(n) + ((r.len + 2) / 3) * 4 + 4 >=
                     sizeof(g_json))
    {
        return;
    }
    n += base64(r.data, r.len, g_json + n);
    n += snprintf(g_json + n, sizeof(g_json) - n, "\"}}");
    broadcast(g_json, static_cast<size_t>(n));
}

void send_csi(const csi_rec& r, const recent_frame* match)
{
    char seq[16] = "null";
    if (match != nullptr)
    {
        snprintf(seq, sizeof(seq), "%u", static_cast<unsigned>(match->seq));
    }
    int n = snprintf(
        g_json, sizeof(g_json),
        "{\"event\":\"frame.csi\",\"epoch\":%u,\"time_us\":%lld,"
        "\"data\":{\"frame_sequence\":%s,\"rx_timestamp_us\":%u,"
        "\"source\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d,"
        "\"noise\":%d,\"channel\":%u,\"format\":\"lltf_imag_real_i8\","
        "\"subcarriers\":%u,\"csi\":\"",
        static_cast<unsigned>(g.epoch.load()),
        static_cast<long long>(r.time_us), seq,
        static_cast<unsigned>(r.rx_ts), r.mac[0], r.mac[1], r.mac[2],
        r.mac[3], r.mac[4], r.mac[5], r.rssi, r.noise, r.channel,
        static_cast<unsigned>(r.len / 2));
    if (n < 0)
    {
        return;
    }
    n += base64(reinterpret_cast<const uint8_t*>(r.buf), r.len, g_json + n);
    n += snprintf(g_json + n, sizeof(g_json) - n, "\"}}");
    broadcast(g_json, static_cast<size_t>(n));
}

struct rssi_window
{
    uint32_t count = 0;
    double sum_mw = 0;
    int peak = -128;
    int min = 127;

    void add(int dbm)
    {
        ++count;
        sum_mw += pow(10.0, dbm / 10.0);
        peak = dbm > peak ? dbm : peak;
        min = dbm < min ? dbm : min;
    }
};

void send_power(const rssi_window& w)
{
    // RSSI power meter: linear mean of the frame powers in the window.
    const double mean = 10.0 * log10(w.sum_mw / w.count);
    const int n = snprintf(
        g_json, sizeof(g_json),
        "{\"event\":\"power\",\"epoch\":%u,\"time_us\":%lld,"
        "\"data\":{\"source\":\"rssi\",\"unit\":\"dBm\",\"value\":%.1f,"
        "\"rms\":%.1f,\"peak\":%d,\"min\":%d,\"clipped\":0,\"samples\":%u,"
        "\"window_ms\":%u}}",
        static_cast<unsigned>(g.epoch.load()),
        static_cast<long long>(esp_timer_get_time()), mean, mean, w.peak,
        w.min, static_cast<unsigned>(w.count),
        static_cast<unsigned>(k_power_period_us / 1000));
    broadcast(g_json, static_cast<size_t>(n));
}

void send_stats(uint32_t frames_per_s, uint32_t fcs_fail_per_s)
{
    const int n = snprintf(
        g_json, sizeof(g_json),
        "{\"event\":\"dropped\",\"epoch\":%u,\"time_us\":%lld,"
        "\"data\":{\"frames\":%u,\"csi\":%u,\"ws\":%u,\"frame_rate\":%u,"
        "\"fcs_fail_rate\":%u}}",
        static_cast<unsigned>(g.epoch.load()),
        static_cast<long long>(esp_timer_get_time()),
        static_cast<unsigned>(g.dropped_frames.load()),
        static_cast<unsigned>(g.dropped_csi.load()),
        static_cast<unsigned>(g.dropped_ws.load()),
        static_cast<unsigned>(frames_per_s),
        static_cast<unsigned>(fcs_fail_per_s));
    broadcast(g_json, static_cast<size_t>(n));
}

// --- Sender task -----------------------------------------------------------

void sender_task(void* arg)
{
    (void)arg;
    recent_frame recent[k_recent] = {};
    size_t recent_head = 0;
    uint8_t pending[k_csi_pending];
    size_t pending_n = 0;
    uint32_t seq = 0;
    uint32_t frames_in_period = 0;
    uint32_t fcs_fail_in_period = 0;
    rssi_window power;
    int64_t next_power = esp_timer_get_time() + k_power_period_us;
    int64_t next_stats = esp_timer_get_time() + k_stats_period_us;

    auto find_recent = [&](uint32_t rx_ts) -> const recent_frame*
    {
        for (const recent_frame& f : recent)
        {
            if (f.valid && f.rx_ts == rx_ts)
            {
                return &f;
            }
        }
        return nullptr;
    };

    while (true)
    {
        uint8_t slot = 0;
        if (xQueueReceive(g.frame_ready, &slot, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            const frame_rec& r = g.frames[slot];
            ++seq;
            send_frame(r, seq);
            recent[recent_head] = {r.rx_ts, seq, true};
            recent_head = (recent_head + 1) % k_recent;
            power.add(r.rssi);
            ++frames_in_period;
            fcs_fail_in_period += r.rx_state != 0 ? 1 : 0;
            xQueueSend(g.frame_free, &slot, 0);
        }

        // CSI usually trails its frame; hold unmatched CSI briefly.
        while (pending_n < k_csi_pending &&
               xQueueReceive(g.csi_ready, &slot, 0) == pdTRUE)
        {
            pending[pending_n++] = slot;
        }
        const int64_t now = esp_timer_get_time();
        size_t keep = 0;
        for (size_t i = 0; i < pending_n; ++i)
        {
            const csi_rec& c = g.csi[pending[i]];
            const recent_frame* match = find_recent(c.rx_ts);
            if (match != nullptr || now - c.time_us > k_csi_match_wait_us ||
                !streaming())
            {
                if (streaming())
                {
                    send_csi(c, match);
                }
                xQueueSend(g.csi_free, &pending[i], 0);
            }
            else
            {
                pending[keep++] = pending[i];
            }
        }
        pending_n = keep;

        if (now >= next_power)
        {
            if (power.count != 0 && streaming())
            {
                send_power(power);
            }
            power = rssi_window();
            next_power = now + k_power_period_us;
        }
        if (now >= next_stats)
        {
            if (streaming())
            {
                send_stats(frames_in_period, fcs_fail_in_period);
            }
            frames_in_period = 0;
            fcs_fail_in_period = 0;
            next_stats = now + k_stats_period_us;
        }
    }
}

// --- HTTP ------------------------------------------------------------------

esp_err_t send_json(httpd_req_t* req, cJSON* root, const char* status)
{
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr)
    {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

esp_err_t send_error(httpd_req_t* req, const char* status, const char* msg)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", msg);
    return send_json(req, root, status);
}

esp_err_t send_static(httpd_req_t* req, const uint8_t* start,
                      const uint8_t* end, const char* type)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, reinterpret_cast<const char*>(start),
                           end - start);
}

esp_err_t handle_index(httpd_req_t* req)
{
    return send_static(req, web_index_html_start, web_index_html_end,
                       "text/html; charset=utf-8");
}

esp_err_t handle_app_js(httpd_req_t* req)
{
    return send_static(req, web_app_js_start, web_app_js_end,
                       "text/javascript; charset=utf-8");
}

esp_err_t handle_style_css(httpd_req_t* req)
{
    return send_static(req, web_style_css_start, web_style_css_end,
                       "text/css; charset=utf-8");
}

void add_types(cJSON* obj, uint8_t mask)
{
    cJSON* types = cJSON_AddArrayToObject(obj, "types");
    for (uint8_t t = 0; t <= WIFI_PKT_MISC; ++t)
    {
        if (mask & (1u << t))
        {
            cJSON_AddItemToArray(types, cJSON_CreateString(pkt_type_name(t)));
        }
    }
}

cJSON* config_json()
{
    cJSON* root = cJSON_CreateObject();
    const uint8_t ch = current_channel();
    cJSON_AddStringToObject(root, "mode", "frames");
    cJSON_AddNumberToObject(root, "epoch", g.epoch.load());
    if (ch != 0)
    {
        cJSON_AddNumberToObject(root, "channel", ch);
        cJSON_AddNumberToObject(root, "frequency_mhz", channel_mhz(ch));
    }
    else
    {
        cJSON_AddNullToObject(root, "channel");
        cJSON_AddNullToObject(root, "frequency_mhz");
    }
    cJSON* gain = cJSON_AddObjectToObject(root, "gain");
    cJSON_AddStringToObject(gain, "mode", "hardware");
    cJSON_AddBoolToObject(root, "csi", g.csi_enabled.load());
    cJSON* filter = cJSON_AddObjectToObject(root, "filter");
    add_types(filter, g.type_mask.load());
    cJSON_AddNumberToObject(filter, "min_rssi", g.min_rssi.load());
    cJSON_AddStringToObject(filter, "fcs", g.fcs_ok_only.load() ? "ok" : "any");
    const mac_filter mf = mac_filter_unpack(g.mac.load());
    if (mf.enabled)
    {
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mf.addr[0], mf.addr[1], mf.addr[2], mf.addr[3], mf.addr[4],
                 mf.addr[5]);
        cJSON_AddStringToObject(filter, "mac", mac);
    }
    else
    {
        cJSON_AddNullToObject(filter, "mac");
    }
    return root;
}

esp_err_t handle_info(httpd_req_t* req)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api", 1);
    cJSON_AddStringToObject(root, "name", "winject-wifi-analyzer-esp32u");
    cJSON_AddStringToObject(root, "version", WINJECT_VERSION_STRING);
    cJSON_AddStringToObject(root, "mode", "frames");
    cJSON_AddBoolToObject(root, "radio", g.radio != nullptr);
    const uint8_t ch = current_channel();
    if (ch != 0)
    {
        cJSON_AddNumberToObject(root, "channel", ch);
        cJSON_AddNumberToObject(root, "frequency_mhz", channel_mhz(ch));
    }
    cJSON* channels = cJSON_AddArrayToObject(root, "channels");
    for (uint8_t c = WIFI_CHANNEL_MIN; c <= WIFI_CHANNEL_MAX; ++c)
    {
        cJSON_AddItemToArray(channels, cJSON_CreateNumber(c));
    }
    cJSON* features = cJSON_AddArrayToObject(root, "features");
    if (g.radio != nullptr)
    {
        cJSON_AddItemToArray(features, cJSON_CreateString("frames"));
        cJSON_AddItemToArray(features, cJSON_CreateString("csi"));
        cJSON_AddItemToArray(features, cJSON_CreateString("rssi_power"));
    }
    cJSON* planned = cJSON_AddArrayToObject(root, "planned");
    cJSON_AddItemToArray(planned, cJSON_CreateString("iq"));
    cJSON_AddItemToArray(planned, cJSON_CreateString("power"));
    cJSON_AddItemToArray(planned, cJSON_CreateString("sweep"));
    cJSON_AddNumberToObject(root, "max_ws_clients", k_max_clients);
    cJSON_AddNumberToObject(root, "frame_cap", k_frame_cap);
    return send_json(req, root, "200 OK");
}

esp_err_t handle_config_get(httpd_req_t* req)
{
    return send_json(req, config_json(), "200 OK");
}

bool parse_mac(const char* s, uint8_t out[6])
{
    unsigned b[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3],
               &b[4], &b[5]) != 6 ||
        strlen(s) != 17)
    {
        return false;
    }
    for (int i = 0; i < 6; ++i)
    {
        out[i] = static_cast<uint8_t>(b[i]);
    }
    return true;
}

uint32_t extra_promisc_mask(uint8_t types)
{
    uint32_t mask = 0;
    if (types & k_type_mgmt)
    {
        mask |= WIFI_PROMIS_FILTER_MASK_MGMT;
    }
    if (types & k_type_ctrl)
    {
        mask |= WIFI_PROMIS_FILTER_MASK_CTRL;
    }
    return mask;
}

// Validates the whole patch before applying any of it.
esp_err_t handle_config_patch(httpd_req_t* req)
{
    char body[512];
    if (req->content_len <= 0 || req->content_len >= sizeof(body))
    {
        return send_error(req, "400 Bad Request", "body must be 1..511 bytes");
    }
    int got = 0;
    while (got < req->content_len)
    {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0)
        {
            return send_error(req, "400 Bad Request", "body read failed");
        }
        got += n;
    }
    body[got] = '\0';
    cJSON* root = cJSON_Parse(body);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "body is not a JSON object");
    }

    const char* error = nullptr;
    const char* status = "400 Bad Request";
    uint8_t channel = 0;
    uint8_t types = g.type_mask.load();
    int min_rssi = g.min_rssi.load();
    bool fcs_ok = g.fcs_ok_only.load();
    bool csi = g.csi_enabled.load();
    uint64_t mac = g.mac.load();

    const cJSON* mode = cJSON_GetObjectItem(root, "mode");
    if (mode != nullptr)
    {
        if (!cJSON_IsString(mode))
        {
            error = "mode must be a string";
        }
        else if (strcmp(mode->valuestring, "iq") == 0 ||
                 strcmp(mode->valuestring, "sweep") == 0)
        {
            error = "ESP-SDR modes are not implemented in this firmware yet";
            status = "501 Not Implemented";
        }
        else if (strcmp(mode->valuestring, "frames") != 0)
        {
            error = "mode must be frames, iq or sweep";
        }
    }
    const cJSON* freq = cJSON_GetObjectItem(root, "frequency_mhz");
    const cJSON* chan = cJSON_GetObjectItem(root, "channel");
    if (error == nullptr && chan != nullptr)
    {
        if (!cJSON_IsNumber(chan) || chan->valueint < WIFI_CHANNEL_MIN ||
            chan->valueint > WIFI_CHANNEL_MAX)
        {
            error = "channel must be 1..14";
        }
        else
        {
            channel = static_cast<uint8_t>(chan->valueint);
        }
    }
    else if (error == nullptr && freq != nullptr)
    {
        if (!cJSON_IsNumber(freq))
        {
            error = "frequency_mhz must be a number";
        }
        else
        {
            for (uint8_t c = WIFI_CHANNEL_MIN; c <= WIFI_CHANNEL_MAX; ++c)
            {
                if (channel_mhz(c) == freq->valueint)
                {
                    channel = c;
                }
            }
            if (channel == 0)
            {
                error = "frames mode tunes to WiFi channel centers "
                        "(2412..2472 step 5, 2484 MHz)";
            }
        }
    }
    const cJSON* gain = cJSON_GetObjectItem(root, "gain");
    if (error == nullptr && gain != nullptr)
    {
        const cJSON* gmode = cJSON_GetObjectItem(gain, "mode");
        if (!cJSON_IsString(gmode) ||
            strcmp(gmode->valuestring, "hardware") != 0)
        {
            error = "frames mode uses the WiFi hardware AGC; manual gain is "
                    "an ESP-SDR (iq) setting";
            status = "501 Not Implemented";
        }
    }
    const cJSON* jcsi = cJSON_GetObjectItem(root, "csi");
    if (error == nullptr && jcsi != nullptr)
    {
        if (!cJSON_IsBool(jcsi))
        {
            error = "csi must be a boolean";
        }
        else
        {
            csi = cJSON_IsTrue(jcsi);
        }
    }
    const cJSON* filter = cJSON_GetObjectItem(root, "filter");
    if (error == nullptr && filter != nullptr)
    {
        const cJSON* jtypes = cJSON_GetObjectItem(filter, "types");
        if (jtypes != nullptr)
        {
            if (!cJSON_IsArray(jtypes))
            {
                error = "filter.types must be an array";
            }
            types = 0;
            const cJSON* t = nullptr;
            cJSON_ArrayForEach(t, jtypes)
            {
                bool known = false;
                for (uint8_t i = 0; cJSON_IsString(t) && i <= WIFI_PKT_MISC;
                     ++i)
                {
                    if (strcmp(t->valuestring, pkt_type_name(i)) == 0)
                    {
                        types |= 1u << i;
                        known = true;
                    }
                }
                if (!known)
                {
                    error = "filter.types: mgmt, ctrl, data, misc";
                }
            }
        }
        const cJSON* jrssi = cJSON_GetObjectItem(filter, "min_rssi");
        if (jrssi != nullptr)
        {
            if (!cJSON_IsNumber(jrssi) || jrssi->valueint < -128 ||
                jrssi->valueint > 0)
            {
                error = "filter.min_rssi must be -128..0";
            }
            else
            {
                min_rssi = jrssi->valueint;
            }
        }
        const cJSON* jfcs = cJSON_GetObjectItem(filter, "fcs");
        if (jfcs != nullptr)
        {
            if (!cJSON_IsString(jfcs) || (strcmp(jfcs->valuestring, "ok") != 0 &&
                                          strcmp(jfcs->valuestring, "any") != 0))
            {
                error = "filter.fcs must be ok or any";
            }
            else
            {
                fcs_ok = strcmp(jfcs->valuestring, "ok") == 0;
            }
        }
        const cJSON* jmac = cJSON_GetObjectItem(filter, "mac");
        if (jmac != nullptr)
        {
            mac_filter mf;
            if (cJSON_IsNull(jmac) ||
                (cJSON_IsString(jmac) && jmac->valuestring[0] == '\0'))
            {
                mac = 0;
            }
            else if (cJSON_IsString(jmac) && parse_mac(jmac->valuestring, mf.addr))
            {
                mf.enabled = true;
                mac = mac_filter_pack(mf);
            }
            else
            {
                error = "filter.mac must be aa:bb:cc:dd:ee:ff or null";
            }
        }
    }
    cJSON_Delete(root);
    if (error != nullptr)
    {
        return send_error(req, status, error);
    }

    xSemaphoreTake(g.config_lock, portMAX_DELAY);
    if (channel != 0 && channel != current_channel())
    {
        if (g.radio == nullptr || !g.radio->set_channel(channel))
        {
            xSemaphoreGive(g.config_lock);
            return send_error(req, "409 Conflict",
                              "set_channel failed (channel 14 needs a DSSS "
                              "modulation)");
        }
        g.epoch.fetch_add(1);
    }
    const bool types_changed = types != g.type_mask.load();
    g.type_mask.store(types);
    g.min_rssi.store(static_cast<int8_t>(min_rssi));
    g.fcs_ok_only.store(fcs_ok);
    g.csi_enabled.store(csi);
    g.mac.store(mac);
    if (types_changed && g.radio != nullptr)
    {
        g.radio->rx().set_tap(frame_tap, extra_promisc_mask(types));
    }
    xSemaphoreGive(g.config_lock);
    return send_json(req, config_json(), "200 OK");
}

esp_err_t handle_not_implemented(httpd_req_t* req)
{
    return send_error(req, "501 Not Implemented",
                      "ESP-SDR captures and sweeps are not implemented in "
                      "this firmware yet");
}

// IDF 6 answers the handshake itself and never calls the URI handler for
// it; the post-handshake hook is where a client joins the stream.
esp_err_t on_ws_handshake(httpd_req_t* req)
{
    add_client(httpd_req_to_sockfd(req));
    return ESP_OK;
}

esp_err_t handle_events(httpd_req_t* req)
{
    // Drain client messages; the stream is server -> client only.
    httpd_ws_frame_t frame = {};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK || frame.len == 0)
    {
        return err;
    }
    uint8_t buf[64];
    if (frame.len > sizeof(buf))
    {
        return ESP_FAIL;
    }
    frame.payload = buf;
    return httpd_ws_recv_frame(req, &frame, frame.len);
}

bool reg(const char* uri, httpd_method_t method,
         esp_err_t (*fn)(httpd_req_t*), bool ws = false)
{
    httpd_uri_t h = {};
    h.uri = uri;
    h.method = method;
    h.handler = fn;
    h.is_websocket = ws;
    if (ws)
    {
        h.ws_post_handshake_cb = on_ws_handshake;
    }
    const esp_err_t err = httpd_register_uri_handler(g.server, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "register %s failed: %s", uri, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool start_csi()
{
    wifi_csi_config_t cfg = {};
    cfg.lltf_en = true;
    cfg.htltf_en = false;
    cfg.stbc_htltf2_en = false;
    cfg.ltf_merge_en = false;
    // Keep subcarriers independent for the constellation-style plot.
    cfg.channel_filter_en = false;
    cfg.manu_scale = false;
    esp_err_t err = esp_wifi_set_csi_config(&cfg);
    if (err == ESP_OK)
    {
        err = esp_wifi_set_csi_rx_cb(csi_cb, nullptr);
    }
    if (err == ESP_OK)
    {
        err = esp_wifi_set_csi(true);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "CSI start failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool make_ring(QueueHandle_t* free_q, QueueHandle_t* ready_q, size_t n)
{
    *free_q = xQueueCreate(n, sizeof(uint8_t));
    *ready_q = xQueueCreate(n, sizeof(uint8_t));
    if (*free_q == nullptr || *ready_q == nullptr)
    {
        return false;
    }
    for (uint8_t i = 0; i < n; ++i)
    {
        xQueueSend(*free_q, &i, 0);
    }
    return true;
}

}  // namespace

bool begin(httpd_handle_t server, wifi* radio)
{
    if (server == nullptr)
    {
        ESP_LOGE(TAG, "no httpd; analyzer API disabled");
        return false;
    }
    g.server = server;
    g.radio = radio;
    g.clients_lock = xSemaphoreCreateMutex();
    g.config_lock = xSemaphoreCreateMutex();
    if (g.clients_lock == nullptr || g.config_lock == nullptr ||
        !make_ring(&g.frame_free, &g.frame_ready, k_frame_slots) ||
        !make_ring(&g.csi_free, &g.csi_ready, k_csi_slots))
    {
        ESP_LOGE(TAG, "analyzer alloc failed");
        return false;
    }

    bool ok = reg("/", HTTP_GET, handle_index) &&
              reg("/index.html", HTTP_GET, handle_index) &&
              reg("/app.js", HTTP_GET, handle_app_js) &&
              reg("/style.css", HTTP_GET, handle_style_css) &&
              reg("/api/v1/info", HTTP_GET, handle_info) &&
              reg("/api/v1/config", HTTP_GET, handle_config_get) &&
              reg("/api/v1/config", HTTP_PATCH, handle_config_patch) &&
              reg("/api/v1/captures", HTTP_POST, handle_not_implemented) &&
              reg("/api/v1/sweeps", HTTP_POST, handle_not_implemented) &&
              reg("/api/v1/operations/*", HTTP_DELETE,
                  handle_not_implemented) &&
              reg("/api/v1/events", HTTP_GET, handle_events, true);

    if (radio != nullptr)
    {
        if (xTaskCreatePinnedToCore(sender_task, "analyzer_tx", k_sender_stack,
                                    nullptr, k_sender_prio, nullptr,
                                    APP_TASK_CORE) != pdPASS)
        {
            ESP_LOGE(TAG, "sender task create failed");
            return false;
        }
        ok = radio->rx().set_tap(frame_tap,
                                 extra_promisc_mask(g.type_mask.load())) &&
             ok;
        ok = start_csi() && ok;
    }
    ESP_LOGI(TAG, "web app + /api/v1 up (radio %s)",
             radio != nullptr ? "on" : "off");
    return ok;
}

}  // namespace analyzer
