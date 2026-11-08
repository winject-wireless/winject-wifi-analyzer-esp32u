#include "netstack.h"

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/task.h"

using namespace net;

static const char* TAG = "net";

namespace
{

std::atomic<uint32_t> diag_mac_rx{0}, diag_broadcast{0}, diag_arp{0}, diag_ipv4{0}, diag_probe{0}, diag_arp_reply_ok{0}, diag_arp_reply_err{0};
constexpr int k_task_prio = 6;
constexpr uint32_t k_task_stack = 6144;
constexpr size_t k_rx_queue = 16;
constexpr int64_t k_s = 1000000;
constexpr int64_t k_arp_max_age_us = 300 * k_s;

// network_config keeps addresses in network byte order (lwIP heritage);
// the stack works in host order.
uint32_t to_host(uint32_t net_order)
{
    return __builtin_bswap32(net_order);
}

eth_mac_dma_burst_len_t burst_len(uint8_t beats)
{
    switch (beats)
    {
    case 1:
        return ETH_DMA_BURST_LEN_1;
    case 2:
        return ETH_DMA_BURST_LEN_2;
    case 4:
        return ETH_DMA_BURST_LEN_4;
    case 8:
        return ETH_DMA_BURST_LEN_8;
    case 16:
        return ETH_DMA_BURST_LEN_16;
    default:
        return ETH_DMA_BURST_LEN_32;
    }
}

void ip_str(uint32_t ip, char* out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u", static_cast<unsigned>(ip >> 24),
             static_cast<unsigned>((ip >> 16) & 255),
             static_cast<unsigned>((ip >> 8) & 255), static_cast<unsigned>(ip & 255));
}

}  // namespace

netstack& netstack::instance()
{
    static netstack inst;
    return inst;
}

netstack::netstack() = default;

bool netstack::set_udp_handler(uint16_t port, udp_handler fn, void* ctx)
{
    for (auto& h : handlers_)
    {
        if (h.fn == nullptr || h.port == port)
        {
            h.port = port;
            h.ctx = ctx;
            h.fn = fn;
            return true;
        }
    }
    return false;
}

network_config netstack::network() const
{
    xSemaphoreTake(state_lock_, portMAX_DELAY);
    network_config c = cfg_pending_.load() ? pending_cfg_ : cfg_;
    xSemaphoreGive(state_lock_);
    return c;
}

bool netstack::set_network(const network_config& cfg)
{
    // For dhcp, ip/prefix are the static fallback and must be valid too.
    if (!network_config_valid(cfg))
    {
        return false;
    }
    if (state_lock_ == nullptr)
    {
        cfg_ = cfg;
        return true;
    }
    xSemaphoreTake(state_lock_, portMAX_DELAY);
    pending_cfg_ = cfg;
    cfg_pending_.store(true);
    xSemaphoreGive(state_lock_);
    return true;
}

bool netstack::link_up() const
{
    return link_.load();
}

bool netstack::connected() const
{
    return ip_.load() != 0;
}

uint32_t netstack::local_ipv4() const
{
    return ip_.load();
}

const mac_addr& netstack::mac() const
{
    return mac_;
}

netstack::counters netstack::stats() const
{
    return {rx_frames_.load(), rx_dropped_.load(), tx_frames_.load(), tx_errors_.load()};
}

// --- Ethernet driver ----------------------------------------------------------

bool netstack::bring_up_eth(uint8_t dma_burst_beats)
{
    gpio_config_t power = {};
    power.pin_bit_mask = 1ULL << ETH_PHY_POWER;
    power.mode = GPIO_MODE_OUTPUT;
    gpio_config(&power);
    gpio_set_level(static_cast<gpio_num_t>(ETH_PHY_POWER), 1);
    // LAN8720 + RMII clock need settle time; 50 ms raced the EMAC reset.
    vTaskDelay(pdMS_TO_TICKS(200));

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.sw_reset_timeout_ms = 1000;
    mac_config.flags |= ETH_MAC_FLAG_PIN_TO_CORE;
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac.smi_gpio.mdc_num = ETH_PHY_MDC;
    emac.smi_gpio.mdio_num = ETH_PHY_MDIO;
    emac.interface = EMAC_DATA_INTERFACE_RMII;
    emac.dma_burst_len = burst_len(dma_burst_beats);
#if ETH_CLK_MODE == ETH_CLK_GPIO17_OUT
    emac.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
    emac.clock_config.rmii.clock_gpio = 17;
    ESP_LOGI(TAG, "RMII REF_CLK GPIO17 out");
#else
    emac.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac.clock_config.rmii.clock_gpio = 0;
    gpio_reset_pin(GPIO_NUM_0);
    ESP_LOGI(TAG, "RMII REF_CLK GPIO0 in");
#endif
    esp_eth_mac_t* mac = esp_eth_mac_new_esp32(&emac, &mac_config);
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = -1;
    esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phy_config);
    if (mac == nullptr || phy == nullptr)
    {
        ESP_LOGE(TAG, "ETH MAC/PHY alloc failed");
        return false;
    }
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3 && err != ESP_OK; ++attempt)
    {
        err = esp_eth_driver_install(&eth_config, &eth_);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "ETH install attempt %d: %s", attempt + 1, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (err != ESP_OK)
    {
        return false;
    }
    if (esp_read_mac(mac_.b, ESP_MAC_ETH) != ESP_OK)
    {
        esp_eth_ioctl(eth_, ETH_CMD_G_MAC_ADDR, mac_.b);
    }
    esp_eth_ioctl(eth_, ETH_CMD_S_MAC_ADDR, mac_.b);
    if (esp_eth_update_input_path(eth_, on_eth_input, this) != ESP_OK ||
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, this) !=
            ESP_OK ||
        esp_eth_start(eth_) != ESP_OK)
    {
        ESP_LOGE(TAG, "ETH start failed");
        return false;
    }
    ESP_LOGI(TAG, "ETH MAC %02x:%02x:%02x:%02x:%02x:%02x (raw stack, no lwIP)",
             mac_.b[0], mac_.b[1], mac_.b[2], mac_.b[3], mac_.b[4], mac_.b[5]);
    return true;
}

esp_err_t netstack::on_eth_input(esp_eth_handle_t, uint8_t* buf, uint32_t len,
                                 void* priv)
{
    auto* self = static_cast<netstack*>(priv);
    diag_mac_rx.fetch_add(1);
    if (len >= 57 && memcmp(buf + 42, "ESP32U_RX_TEST_", 15) == 0) diag_probe.fetch_add(1);
    if (len >= k_eth_hdr) {
        if (memcmp(buf, k_broadcast_mac.b, 6) == 0) diag_broadcast.fetch_add(1);
        if (rd16(buf + 12) == k_ethertype_arp) diag_arp.fetch_add(1);
        if (rd16(buf + 12) == k_ethertype_ipv4) diag_ipv4.fetch_add(1);
    }
    // Cheap filter in the EMAC task: ours or broadcast, IPv4 or ARP.
    bool keep = len >= k_eth_hdr &&
                (memcmp(buf, self->mac_.b, 6) == 0 || memcmp(buf, k_broadcast_mac.b, 6) == 0);
    if (keep)
    {
        const uint16_t type = rd16(buf + 12);
        keep = type == k_ethertype_ipv4 || type == k_ethertype_arp;
    }
    rx_item item = {buf, len};
    if (!keep || xQueueSend(self->rx_q_, &item, 0) != pdTRUE)
    {
        if (keep)
        {
            self->rx_dropped_.fetch_add(1);
        }
        free(buf);
    }
    return ESP_OK;
}

void netstack::on_eth_event(void* arg, esp_event_base_t, int32_t id, void*)
{
    auto* self = static_cast<netstack*>(arg);
    if (id == ETHERNET_EVENT_CONNECTED)
    {
        self->link_.store(true);
        self->link_changed_.store(true);
        ESP_LOGI(TAG, "link up");
    }
    else if (id == ETHERNET_EVENT_DISCONNECTED)
    {
        self->link_.store(false);
        self->link_changed_.store(true);
        ESP_LOGW(TAG, "link down");
    }
}

bool netstack::transmit(const uint8_t* frame, size_t len)
{
    if (eth_ == nullptr || !link_.load())
    {
        tx_errors_.fetch_add(1);
        return false;
    }
    xSemaphoreTake(tx_lock_, portMAX_DELAY);
    esp_err_t err = esp_eth_transmit(eth_, const_cast<uint8_t*>(frame), len);
    xSemaphoreGive(tx_lock_);
    if (err != ESP_OK)
    {
        tx_errors_.fetch_add(1);
        return false;
    }
    tx_frames_.fetch_add(1);
    return true;
}

bool netstack::start(const network_config& cfg, uint8_t eth_dma_burst_beats)
{
    rx_q_ = xQueueCreate(k_rx_queue, sizeof(rx_item));
    tx_lock_ = xSemaphoreCreateMutex();
    state_lock_ = xSemaphoreCreateMutex();
    if (rx_q_ == nullptr || tx_lock_ == nullptr || state_lock_ == nullptr)
    {
        return false;
    }
    pending_cfg_ = cfg;
    cfg_pending_.store(true);
    if (!bring_up_eth(eth_dma_burst_beats))
    {
        return false;
    }
    return xTaskCreatePinnedToCore(task_entry, "net", k_task_stack, this, k_task_prio,
                                   nullptr, APP_TASK_CORE) == pdPASS;
}

void netstack::task_entry(void* arg)
{
    static_cast<netstack*>(arg)->run();
}

void netstack::run()
{
    int64_t diag_next = 0;
    while (true)
    {
        rx_item item;
        if (xQueueReceive(rx_q_, &item, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            rx_frames_.fetch_add(1);
            handle_frame(item.buf, item.len);
            free(item.buf);
        }
        if (cfg_pending_.load() || link_changed_.exchange(false))
        {
            apply_config();
        }
        const int64_t now = esp_timer_get_time();
        dhcp_tick(now);
        if (now >= diag_next) {
            diag_next = now + 5000000;
            ESP_LOGI("eth_diag", "mac_rx=%lu broadcast=%lu arp=%lu ipv4=%lu accepted=%lu rx_drop=%lu tx_ok=%lu tx_error=%lu probe=%lu arp_reply_ok=%lu arp_reply_err=%lu", (unsigned long)diag_mac_rx.load(), (unsigned long)diag_broadcast.load(), (unsigned long)diag_arp.load(), (unsigned long)diag_ipv4.load(), (unsigned long)rx_frames_.load(), (unsigned long)rx_dropped_.load(), (unsigned long)tx_frames_.load(), (unsigned long)tx_errors_.load(), (unsigned long)diag_probe.load(), (unsigned long)diag_arp_reply_ok.load(), (unsigned long)diag_arp_reply_err.load());
        }
    }
}

// --- Addressing -----------------------------------------------------------------

void netstack::set_address(uint32_t ip, uint32_t mask, uint32_t gw, const char* why)
{
    const uint32_t old = ip_.load();
    mask_.store(mask);
    gw_.store(gw);
    ip_.store(ip);
    if (ip == old)
    {
        return;
    }
    char a[16], m[16], g[16];
    ip_str(ip, a, sizeof(a));
    ip_str(mask, m, sizeof(m));
    ip_str(gw, g, sizeof(g));
    ESP_LOGI(TAG, "%s: ip %s mask %s gw %s", why, a, m, g);
    if (ip != 0)
    {
        // Gratuitous ARP so switches and hosts learn the (new) address.
        arp_packet arp = {};
        arp.op = 1;
        arp.sha = mac_;
        arp.spa = ip;
        arp.tpa = ip;
        transmit(tx_buf_, build_arp(tx_buf_, k_broadcast_mac, arp));
    }
}

void netstack::apply_config()
{
    xSemaphoreTake(state_lock_, portMAX_DELAY);
    if (cfg_pending_.exchange(false))
    {
        cfg_ = pending_cfg_;
    }
    const network_config cfg = cfg_;
    xSemaphoreGive(state_lock_);

    if (cfg.type == network_type::static_ip)
    {
        dhcp_ = dhcp_state::off;
        set_address(to_host(cfg.ip), to_host(network_prefix_netmask(cfg.prefix)), 0,
                    "static");
        return;
    }
    if (!link_.load())
    {
        return;  // DHCP starts on link up
    }
    dhcp_restart();
}

bool netstack::on_link(uint32_t ip) const
{
    const uint32_t mask = mask_.load();
    return mask != 0 && (ip & mask) == (ip_.load() & mask);
}

// --- DHCP client ----------------------------------------------------------------

void netstack::dhcp_restart()
{
    dhcp_ = dhcp_state::selecting;
    xid_ = esp_random();
    dhcp_started_us_ = esp_timer_get_time();
    dhcp_next_us_ = dhcp_started_us_;
    dhcp_backoff_s_ = 2;
    fallback_active_ = false;
    fallback_at_us_ = dhcp_started_us_ + static_cast<int64_t>(cfg_.timeout_s) * k_s;
    if (ip_.load() != 0)
    {
        set_address(0, 0, 0, "dhcp restart");
    }
}

void netstack::dhcp_send(dhcp_msg type, uint32_t ciaddr)
{
    uint8_t* payload = tx_buf_ + k_udp_payload_off;
    const uint32_t requested = type == dhcp_msg::request && ciaddr == 0 ? offer_ip_ : 0;
    const uint32_t server = type == dhcp_msg::request && ciaddr == 0 ? server_id_ : 0;
    const uint16_t secs = static_cast<uint16_t>(
        (esp_timer_get_time() - dhcp_started_us_) / k_s);
    const size_t n = build_dhcp(payload, type, xid_, mac_, ciaddr, requested, server,
                                DEVICE_HOSTNAME, secs);
    const size_t len =
        finish_udp_frame(tx_buf_, k_broadcast_mac, mac_, ciaddr, 0xffffffffu,
                         k_dhcp_client_port, k_dhcp_server_port, n, ip_id_.fetch_add(1));
    transmit(tx_buf_, len);
}

void netstack::dhcp_tick(int64_t now)
{
    if (dhcp_ == dhcp_state::off || !link_.load())
    {
        return;
    }
    // Static fallback while DHCP keeps trying in the background.
    if (!fallback_active_ && (dhcp_ == dhcp_state::selecting || dhcp_ == dhcp_state::requesting) &&
        now >= fallback_at_us_)
    {
        fallback_active_ = true;
        set_address(to_host(cfg_.ip), to_host(network_prefix_netmask(cfg_.prefix)), 0,
                    "dhcp timeout, static fallback");
    }
    switch (dhcp_)
    {
    case dhcp_state::selecting:
        if (now >= dhcp_next_us_)
        {
            dhcp_send(dhcp_msg::discover, 0);
            dhcp_next_us_ = now + static_cast<int64_t>(dhcp_backoff_s_) * k_s;
            dhcp_backoff_s_ = dhcp_backoff_s_ >= 16 ? 16 : dhcp_backoff_s_ * 2;
        }
        break;
    case dhcp_state::requesting:
        if (now >= dhcp_deadline_us_)
        {
            dhcp_ = dhcp_state::selecting;  // no ACK: start over
            dhcp_next_us_ = now;
        }
        break;
    case dhcp_state::bound:
        if (now >= lease_t1_us_)
        {
            dhcp_ = dhcp_state::renewing;
            xid_ = esp_random();
            dhcp_next_us_ = now;
        }
        break;
    case dhcp_state::renewing:
        if (now >= lease_end_us_)
        {
            ESP_LOGW(TAG, "lease expired");
            dhcp_restart();
        }
        else if (now >= dhcp_next_us_)
        {
            // Broadcast with ciaddr (rebinding form): needs no server ARP.
            dhcp_send(dhcp_msg::request, ip_.load());
            dhcp_next_us_ = now + 10 * k_s;
        }
        break;
    default:
        break;
    }
}

void netstack::dhcp_input(const uint8_t* p, size_t len)
{
    dhcp_reply r;
    if (dhcp_ == dhcp_state::off || !parse_dhcp(p, len, mac_, &r) || r.xid != xid_)
    {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (r.type == dhcp_msg::offer && dhcp_ == dhcp_state::selecting && r.yiaddr != 0)
    {
        offer_ip_ = r.yiaddr;
        server_id_ = r.server_id;
        dhcp_ = dhcp_state::requesting;
        dhcp_deadline_us_ = now + 4 * k_s;
        dhcp_send(dhcp_msg::request, 0);
    }
    else if (r.type == dhcp_msg::ack &&
             (dhcp_ == dhcp_state::requesting || dhcp_ == dhcp_state::renewing))
    {
        const uint32_t lease = r.lease_s != 0 ? r.lease_s : 3600;
        const uint32_t t1 = r.t1_s != 0 && r.t1_s < lease ? r.t1_s : lease / 2;
        dhcp_ = dhcp_state::bound;
        fallback_active_ = false;
        lease_t1_us_ = now + static_cast<int64_t>(t1) * k_s;
        lease_end_us_ = now + static_cast<int64_t>(lease) * k_s;
        if (r.server_id != 0)
        {
            server_id_ = r.server_id;
        }
        const uint32_t mask = r.subnet_mask != 0 ? r.subnet_mask : 0xffffff00u;
        set_address(r.yiaddr, mask, r.router, "dhcp lease");
    }
    else if (r.type == dhcp_msg::nak)
    {
        ESP_LOGW(TAG, "dhcp NAK");
        dhcp_restart();
    }
}

// --- ARP --------------------------------------------------------------------------

void netstack::arp_learn(uint32_t ip, const mac_addr& mac)
{
    if (ip == 0)
    {
        return;
    }
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(state_lock_, portMAX_DELAY);
    arp_entry* slot = &arp_[0];
    for (auto& e : arp_)
    {
        if (e.ip == ip)
        {
            slot = &e;
            break;
        }
        if (e.seen_us < slot->seen_us)
        {
            slot = &e;
        }
    }
    slot->ip = ip;
    slot->mac = mac;
    slot->seen_us = now;
    xSemaphoreGive(state_lock_);
}

bool netstack::arp_lookup(uint32_t ip, mac_addr* mac)
{
    const int64_t now = esp_timer_get_time();
    bool found = false;
    xSemaphoreTake(state_lock_, portMAX_DELAY);
    for (const auto& e : arp_)
    {
        if (e.ip == ip && e.seen_us != 0 && now - e.seen_us < k_arp_max_age_us)
        {
            *mac = e.mac;
            found = true;
            break;
        }
    }
    xSemaphoreGive(state_lock_);
    return found;
}

void netstack::arp_request(uint32_t ip)
{
    uint8_t frame[k_eth_hdr + k_arp_len];
    arp_packet arp = {};
    arp.op = 1;
    arp.sha = mac_;
    arp.spa = ip_.load();
    arp.tpa = ip;
    transmit(frame, build_arp(frame, k_broadcast_mac, arp));
}

void netstack::handle_arp(const eth_frame& eth)
{
    arp_packet arp;
    if (!parse_arp(eth.payload, eth.payload_len, &arp))
    {
        return;
    }
    const uint32_t ours = ip_.load();
    if (on_link(arp.spa) || arp.tpa == ours)
    {
        arp_learn(arp.spa, arp.sha);
    }
    if (arp.op == 1 && ours != 0 && arp.tpa == ours)
    {
        arp_packet reply = {};
        reply.op = 2;
        reply.sha = mac_;
        reply.spa = ours;
        reply.tha = arp.sha;
        reply.tpa = arp.spa;
        if (transmit(tx_buf_, build_arp(tx_buf_, arp.sha, reply))) diag_arp_reply_ok.fetch_add(1);
        else diag_arp_reply_err.fetch_add(1);
    }
}

bool netstack::resolve(uint32_t ip, uint16_t port, udp_peer* peer, TickType_t wait)
{
    if (ip_.load() == 0)
    {
        return false;
    }
    peer->ip = ip;
    peer->port = port;
    if (ip == 0xffffffffu)
    {
        peer->mac = k_broadcast_mac;
        return true;
    }
    if ((ip & 0xf0000000u) == 0xe0000000u)
    {
        peer->mac = {{0x01, 0x00, 0x5e, static_cast<uint8_t>((ip >> 16) & 0x7f), static_cast<uint8_t>(ip >> 8), static_cast<uint8_t>(ip)}};
        return true;
    }
    const uint32_t hop = on_link(ip) ? ip : gw_.load();
    if (hop == 0)
    {
        return false;
    }
    const TickType_t start = xTaskGetTickCount();
    TickType_t next_request = start;
    while (true)
    {
        if (arp_lookup(hop, &peer->mac))
        {
            return true;
        }
        const TickType_t now = xTaskGetTickCount();
        if (now - start >= wait)
        {
            return false;
        }
        if (now >= next_request)
        {
            arp_request(hop);
            next_request = now + pdMS_TO_TICKS(200);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// --- IPv4 -------------------------------------------------------------------------

void netstack::handle_frame(uint8_t* buf, size_t len)
{
    eth_frame eth;
    if (!parse_eth(buf, len, &eth))
    {
        return;
    }
    if (eth.ethertype == k_ethertype_arp)
    {
        handle_arp(eth);
    }
    else if (eth.ethertype == k_ethertype_ipv4)
    {
        handle_ipv4(buf, len, eth);
    }
}

void netstack::handle_ipv4(uint8_t* buf, size_t len, const eth_frame& eth)
{
    ipv4_packet ip;
    if (!parse_ipv4(eth.payload, eth.payload_len, &ip))
    {
        return;
    }
    const uint32_t ours = ip_.load();
    const uint32_t mask = mask_.load();
    const bool to_us = (ours != 0 && ip.dst == ours) || ip.dst == 0xffffffffu ||
                       (mask != 0 && ip.dst == (ours | ~mask));
    udp_datagram udp;
    const bool is_udp = parse_udp(ip, &udp);
    // Before an address exists only DHCP replies (possibly unicast to the
    // offered address) are of interest.
    if (!to_us && !(is_udp && udp.dst_port == k_dhcp_client_port))
    {
        return;
    }
    mac_addr src_mac;
    memcpy(src_mac.b, eth.src, 6);
    if (on_link(ip.src))
    {
        arp_learn(ip.src, src_mac);
    }
    if (ip.protocol == k_ip_icmp && ip.dst == ours)
    {
        size_t out_len = 0;
        if (icmp_echo_to_reply(buf, len, mac_, &out_len))
        {
            transmit(buf, out_len);
        }
        return;
    }
    if (!is_udp)
    {
        return;
    }
    if (udp.dst_port == k_dhcp_client_port)
    {
        dhcp_input(udp.payload, udp.payload_len);
        return;
    }
    for (const auto& h : handlers_)
    {
        if (h.fn != nullptr && h.port == udp.dst_port)
        {
            const udp_rx rx = {ip.src, src_mac, udp.src_port, udp.dst_port, udp.payload,
                               udp.payload_len};
            h.fn(h.ctx, rx);
            return;
        }
    }
}

bool netstack::send_udp(uint8_t* frame, size_t payload_len, const udp_peer& peer,
                        uint16_t src_port)
{
    const uint32_t ours = ip_.load();
    if (ours == 0 || payload_len > k_udp_payload_max)
    {
        tx_errors_.fetch_add(1);
        return false;
    }
    const size_t len = finish_udp_frame(frame, peer.mac, mac_, ours, peer.ip, src_port,
                                        peer.port, payload_len, ip_id_.fetch_add(1));
    return transmit(frame, len);
}
