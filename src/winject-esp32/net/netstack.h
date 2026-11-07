#ifndef WINJECT_NET_NETSTACK_H_
#define WINJECT_NET_NETSTACK_H_

#include "config_types.h"
#include "net_proto.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "esp_eth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// Raw-Ethernet IPv4 stack replacing lwIP: the EMAC input path delivers frames
// to one net task, which answers ARP and ICMP echo, runs the DHCP client
// (with the configured static fallback) and dispatches UDP to port handlers.
// Senders build complete frames (net_proto.h) and transmit them directly.
class netstack
{
public:
    struct udp_rx
    {
        uint32_t src_ip;
        net::mac_addr src_mac;
        uint16_t src_port;
        uint16_t dst_port;
        const uint8_t* data;
        size_t len;
    };

    struct udp_peer
    {
        uint32_t ip = 0;
        uint16_t port = 0;
        net::mac_addr mac = {};  // next hop
    };

    // Runs on the net task; data is valid only during the call.
    using udp_handler = void (*)(void* ctx, const udp_rx& rx);

    struct counters
    {
        uint32_t rx_frames;
        uint32_t rx_dropped;
        uint32_t tx_frames;
        uint32_t tx_errors;
    };

    static netstack& instance();
    netstack(const netstack&) = delete;
    netstack& operator=(const netstack&) = delete;

    // Brings up the LAN8720 (config.h pins, clock mode) and starts the net
    // task with cfg. Handlers must be registered before start().
    bool start(const network_config& cfg, uint8_t eth_dma_burst_beats);
    bool set_udp_handler(uint16_t port, udp_handler fn, void* ctx);

    network_config network() const;
    // Validates cfg; the net task applies it on its next turn (safe from any
    // task, including a UDP handler).
    bool set_network(const network_config& cfg);

    bool link_up() const;
    // true once an address is configured (DHCP lease or static/fallback).
    bool connected() const;
    // Host order (a.b.c.d == 0xaabbccdd); 0 without an address.
    uint32_t local_ipv4() const;
    const net::mac_addr& mac() const;
    counters stats() const;

    // Resolves the next hop for ip (ARP, waiting up to wait); fills peer.mac.
    // Do not call from the net task with wait > 0.
    bool resolve(uint32_t ip, uint16_t port, udp_peer* peer, TickType_t wait);
    // frame: k_eth_max bytes with the payload at net::k_udp_payload_off.
    bool send_udp(uint8_t* frame, size_t payload_len, const udp_peer& peer,
                  uint16_t src_port);

private:
    netstack();

    enum class dhcp_state : uint8_t
    {
        off,
        selecting,
        requesting,
        bound,
        renewing,
    };

    struct rx_item
    {
        uint8_t* buf;
        uint32_t len;
    };

    struct arp_entry
    {
        uint32_t ip;
        net::mac_addr mac;
        int64_t seen_us;
    };

    static constexpr size_t k_handlers = 4;
    static constexpr size_t k_arp_entries = 8;

    static esp_err_t on_eth_input(esp_eth_handle_t eth, uint8_t* buf, uint32_t len,
                                  void* priv);
    static void on_eth_event(void* arg, esp_event_base_t base, int32_t id,
                             void* data);
    static void task_entry(void* arg);
    bool bring_up_eth(uint8_t dma_burst_beats);
    void run();
    void handle_frame(uint8_t* buf, size_t len);
    void handle_arp(const net::eth_frame& eth);
    void handle_ipv4(uint8_t* buf, size_t len, const net::eth_frame& eth);
    bool transmit(const uint8_t* frame, size_t len);

    void apply_config();
    void set_address(uint32_t ip, uint32_t mask, uint32_t gw, const char* why);
    void dhcp_restart();
    void dhcp_tick(int64_t now);
    void dhcp_send(net::dhcp_msg type, uint32_t ciaddr);
    void dhcp_input(const uint8_t* p, size_t len);
    void arp_learn(uint32_t ip, const net::mac_addr& mac);
    bool arp_lookup(uint32_t ip, net::mac_addr* mac);
    void arp_request(uint32_t ip);
    bool on_link(uint32_t ip) const;

    esp_eth_handle_t eth_ = nullptr;
    net::mac_addr mac_ = {};
    QueueHandle_t rx_q_ = nullptr;
    SemaphoreHandle_t tx_lock_ = nullptr;
    SemaphoreHandle_t state_lock_ = nullptr;
    std::atomic<bool> link_{false};
    std::atomic<bool> link_changed_{false};
    std::atomic<uint32_t> ip_{0};
    std::atomic<uint32_t> mask_{0};
    std::atomic<uint32_t> gw_{0};
    std::atomic<uint16_t> ip_id_{1};
    std::atomic<uint32_t> rx_frames_{0};
    std::atomic<uint32_t> rx_dropped_{0};
    std::atomic<uint32_t> tx_frames_{0};
    std::atomic<uint32_t> tx_errors_{0};

    network_config cfg_;
    network_config pending_cfg_;
    std::atomic<bool> cfg_pending_{false};

    struct
    {
        uint16_t port;
        udp_handler fn;
        void* ctx;
    } handlers_[k_handlers] = {};

    arp_entry arp_[k_arp_entries] = {};

    // DHCP client state (net task only).
    dhcp_state dhcp_ = dhcp_state::off;
    uint32_t xid_ = 0;
    uint32_t offer_ip_ = 0;
    uint32_t server_id_ = 0;
    int64_t dhcp_started_us_ = 0;
    int64_t dhcp_next_us_ = 0;
    int64_t dhcp_deadline_us_ = 0;
    int64_t lease_t1_us_ = 0;
    int64_t lease_end_us_ = 0;
    unsigned dhcp_backoff_s_ = 2;
    bool fallback_active_ = false;
    int64_t fallback_at_us_ = 0;

    uint8_t tx_buf_[net::k_eth_max];  // net task's own frames
};

#endif  // WINJECT_NET_NETSTACK_H_
