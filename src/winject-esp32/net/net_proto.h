#ifndef WINJECT_NET_NET_PROTO_H_
#define WINJECT_NET_NET_PROTO_H_

// Wire formats for the raw Ethernet stack (no lwIP): Ethernet II, ARP,
// IPv4 (no options sent, no fragments accepted), UDP, ICMP echo and DHCP.
// IDF-free so the host tests cover it. Addresses are host-order uint32_t
// (a.b.c.d == 0xaabbccdd); everything on the wire is big-endian.

#include <stddef.h>
#include <stdint.h>

namespace net
{

constexpr size_t k_eth_hdr = 14;
constexpr size_t k_ip_hdr = 20;
constexpr size_t k_udp_hdr = 8;
// Ethernet + IPv4 + UDP: UDP payload offset in a frame we build.
constexpr size_t k_udp_payload_off = k_eth_hdr + k_ip_hdr + k_udp_hdr;
constexpr size_t k_eth_max = 1514;  // without FCS
constexpr size_t k_udp_payload_max = k_eth_max - k_udp_payload_off;  // 1472

constexpr uint16_t k_ethertype_ipv4 = 0x0800;
constexpr uint16_t k_ethertype_arp = 0x0806;
constexpr uint8_t k_ip_icmp = 1;
constexpr uint8_t k_ip_udp = 17;
constexpr uint16_t k_dhcp_server_port = 67;
constexpr uint16_t k_dhcp_client_port = 68;

struct mac_addr
{
    uint8_t b[6];
};

extern const mac_addr k_broadcast_mac;

inline uint16_t rd16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t rd32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

inline void wr16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

inline void wr32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

// One's-complement sum (RFC 1071) of len bytes, folded into 16 bits, not
// inverted; chain with `sum` as the start value.
uint32_t csum_add(uint32_t sum, const uint8_t* data, size_t len);
uint16_t csum_fold(uint32_t sum);

// --- Ethernet --------------------------------------------------------------

struct eth_frame
{
    const uint8_t* dst;
    const uint8_t* src;
    uint16_t ethertype;
    const uint8_t* payload;
    size_t payload_len;
};

// false: shorter than an Ethernet header or not Ethernet II.
bool parse_eth(const uint8_t* frame, size_t len, eth_frame* out);
void write_eth(uint8_t* frame, const mac_addr& dst, const mac_addr& src,
               uint16_t ethertype);

// --- ARP -------------------------------------------------------------------

struct arp_packet
{
    uint16_t op;  // 1 request, 2 reply
    mac_addr sha;
    uint32_t spa;
    mac_addr tha;
    uint32_t tpa;
};

constexpr size_t k_arp_len = 28;
bool parse_arp(const uint8_t* p, size_t len, arp_packet* out);
// Builds a complete ARP frame (42 bytes) into frame; returns its length.
size_t build_arp(uint8_t* frame, const mac_addr& eth_dst, const arp_packet& arp);

// --- IPv4 / UDP / ICMP -------------------------------------------------------

struct ipv4_packet
{
    uint8_t protocol;
    uint8_t ttl;
    uint32_t src;
    uint32_t dst;
    const uint8_t* payload;
    size_t payload_len;
};

// Verifies version, header checksum and lengths; rejects fragments.
bool parse_ipv4(const uint8_t* p, size_t len, ipv4_packet* out);
// Writes a 20-byte header (DF set, no options) for payload_len bytes.
void write_ipv4(uint8_t* p, uint8_t protocol, uint32_t src, uint32_t dst,
                uint16_t payload_len, uint16_t id);

struct udp_datagram
{
    uint16_t src_port;
    uint16_t dst_port;
    const uint8_t* payload;
    size_t payload_len;
};

// Checks the UDP checksum when present (non-zero).
bool parse_udp(const ipv4_packet& ip, udp_datagram* out);

// Fills Ethernet + IPv4 + UDP headers in front of a payload already placed
// at frame + k_udp_payload_off; returns the frame length.
size_t finish_udp_frame(uint8_t* frame, const mac_addr& dst_mac,
                        const mac_addr& src_mac, uint32_t src_ip, uint32_t dst_ip,
                        uint16_t src_port, uint16_t dst_port, size_t payload_len,
                        uint16_t ip_id);

// Turns an ICMP echo request (frame as received) into the reply in place;
// false if it is not a valid echo request. Returns the frame length to send.
bool icmp_echo_to_reply(uint8_t* frame, size_t len, const mac_addr& our_mac,
                        size_t* out_len);

// --- DHCP (RFC 2131 client) ---------------------------------------------------

enum class dhcp_msg : uint8_t
{
    none = 0,
    discover = 1,
    offer = 2,
    request = 3,
    decline = 4,
    ack = 5,
    nak = 6,
    release = 7,
};

struct dhcp_reply
{
    dhcp_msg type = dhcp_msg::none;
    uint32_t xid = 0;
    uint32_t yiaddr = 0;
    uint32_t server_id = 0;
    uint32_t subnet_mask = 0;
    uint32_t router = 0;
    uint32_t lease_s = 0;
    uint32_t t1_s = 0;
    uint32_t t2_s = 0;
};

// Builds a DHCP client message as a UDP payload (BOOTP + options) into p;
// returns its length (<= 300). requested_ip / server_id 0 = omit.
size_t build_dhcp(uint8_t* p, dhcp_msg type, uint32_t xid, const mac_addr& chaddr,
                  uint32_t ciaddr, uint32_t requested_ip, uint32_t server_id,
                  const char* hostname, uint16_t secs);
// Parses a server reply addressed to chaddr; false if not one.
bool parse_dhcp(const uint8_t* p, size_t len, const mac_addr& chaddr,
                dhcp_reply* out);

}  // namespace net

#endif  // WINJECT_NET_NET_PROTO_H_
