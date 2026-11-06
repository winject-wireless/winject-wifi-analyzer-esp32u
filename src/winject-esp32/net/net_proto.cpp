#include "net_proto.h"

#include <string.h>

namespace net
{

const mac_addr k_broadcast_mac = {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};

uint32_t csum_add(uint32_t sum, const uint8_t* data, size_t len)
{
    size_t i = 0;
    for (; i + 1 < len; i += 2)
    {
        sum += static_cast<uint32_t>((data[i] << 8) | data[i + 1]);
    }
    if (i < len)
    {
        sum += static_cast<uint32_t>(data[i] << 8);
    }
    return sum;
}

uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16)
    {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return static_cast<uint16_t>(sum);
}

// --- Ethernet ----------------------------------------------------------------

bool parse_eth(const uint8_t* frame, size_t len, eth_frame* out)
{
    if (frame == nullptr || len < k_eth_hdr)
    {
        return false;
    }
    const uint16_t type = rd16(frame + 12);
    if (type < 0x0600)
    {
        return false;  // 802.3 length field, not Ethernet II
    }
    out->dst = frame;
    out->src = frame + 6;
    out->ethertype = type;
    out->payload = frame + k_eth_hdr;
    out->payload_len = len - k_eth_hdr;
    return true;
}

void write_eth(uint8_t* frame, const mac_addr& dst, const mac_addr& src,
               uint16_t ethertype)
{
    memcpy(frame, dst.b, 6);
    memcpy(frame + 6, src.b, 6);
    wr16(frame + 12, ethertype);
}

// --- ARP -----------------------------------------------------------------------

bool parse_arp(const uint8_t* p, size_t len, arp_packet* out)
{
    // Ethernet/IPv4 ARP only.
    if (len < k_arp_len || rd16(p) != 1 || rd16(p + 2) != k_ethertype_ipv4 ||
        p[4] != 6 || p[5] != 4)
    {
        return false;
    }
    out->op = rd16(p + 6);
    memcpy(out->sha.b, p + 8, 6);
    out->spa = rd32(p + 14);
    memcpy(out->tha.b, p + 18, 6);
    out->tpa = rd32(p + 24);
    return out->op == 1 || out->op == 2;
}

size_t build_arp(uint8_t* frame, const mac_addr& eth_dst, const arp_packet& arp)
{
    write_eth(frame, eth_dst, arp.sha, k_ethertype_arp);
    uint8_t* p = frame + k_eth_hdr;
    wr16(p, 1);
    wr16(p + 2, k_ethertype_ipv4);
    p[4] = 6;
    p[5] = 4;
    wr16(p + 6, arp.op);
    memcpy(p + 8, arp.sha.b, 6);
    wr32(p + 14, arp.spa);
    memcpy(p + 18, arp.tha.b, 6);
    wr32(p + 24, arp.tpa);
    return k_eth_hdr + k_arp_len;
}

// --- IPv4 / UDP / ICMP -----------------------------------------------------------

bool parse_ipv4(const uint8_t* p, size_t len, ipv4_packet* out)
{
    if (len < k_ip_hdr || (p[0] >> 4) != 4)
    {
        return false;
    }
    const size_t ihl = static_cast<size_t>(p[0] & 0x0f) * 4;
    const size_t total = rd16(p + 2);
    if (ihl < k_ip_hdr || total < ihl || total > len)
    {
        return false;
    }
    if (csum_fold(csum_add(0, p, ihl)) != 0xffff)
    {
        return false;
    }
    // More-fragments flag or a non-zero offset: not supported.
    if ((rd16(p + 6) & 0x3fff) != 0)
    {
        return false;
    }
    out->ttl = p[8];
    out->protocol = p[9];
    out->src = rd32(p + 12);
    out->dst = rd32(p + 16);
    out->payload = p + ihl;
    out->payload_len = total - ihl;
    return true;
}

void write_ipv4(uint8_t* p, uint8_t protocol, uint32_t src, uint32_t dst,
                uint16_t payload_len, uint16_t id)
{
    p[0] = 0x45;
    p[1] = 0;
    wr16(p + 2, static_cast<uint16_t>(k_ip_hdr + payload_len));
    wr16(p + 4, id);
    wr16(p + 6, 0x4000);  // DF
    p[8] = 64;
    p[9] = protocol;
    wr16(p + 10, 0);
    wr32(p + 12, src);
    wr32(p + 16, dst);
    wr16(p + 10, static_cast<uint16_t>(~csum_fold(csum_add(0, p, k_ip_hdr))));
}

static uint32_t pseudo_header_sum(uint32_t src, uint32_t dst, uint8_t protocol,
                                  uint16_t len)
{
    uint32_t sum = (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff);
    return sum + protocol + len;
}

bool parse_udp(const ipv4_packet& ip, udp_datagram* out)
{
    if (ip.protocol != k_ip_udp || ip.payload_len < k_udp_hdr)
    {
        return false;
    }
    const uint8_t* p = ip.payload;
    const uint16_t len = rd16(p + 4);
    if (len < k_udp_hdr || len > ip.payload_len)
    {
        return false;
    }
    if (rd16(p + 6) != 0)
    {
        uint32_t sum = pseudo_header_sum(ip.src, ip.dst, k_ip_udp, len);
        if (csum_fold(csum_add(sum, p, len)) != 0xffff)
        {
            return false;
        }
    }
    out->src_port = rd16(p);
    out->dst_port = rd16(p + 2);
    out->payload = p + k_udp_hdr;
    out->payload_len = len - k_udp_hdr;
    return true;
}

size_t finish_udp_frame(uint8_t* frame, const mac_addr& dst_mac,
                        const mac_addr& src_mac, uint32_t src_ip, uint32_t dst_ip,
                        uint16_t src_port, uint16_t dst_port, size_t payload_len,
                        uint16_t ip_id)
{
    const uint16_t udp_len = static_cast<uint16_t>(k_udp_hdr + payload_len);
    write_eth(frame, dst_mac, src_mac, k_ethertype_ipv4);
    write_ipv4(frame + k_eth_hdr, k_ip_udp, src_ip, dst_ip, udp_len, ip_id);
    uint8_t* u = frame + k_eth_hdr + k_ip_hdr;
    wr16(u, src_port);
    wr16(u + 2, dst_port);
    wr16(u + 4, udp_len);
    wr16(u + 6, 0);
    uint16_t c = static_cast<uint16_t>(
        ~csum_fold(csum_add(pseudo_header_sum(src_ip, dst_ip, k_ip_udp, udp_len), u,
                            udp_len)));
    wr16(u + 6, c == 0 ? 0xffff : c);
    return k_udp_payload_off + payload_len;
}

bool icmp_echo_to_reply(uint8_t* frame, size_t len, const mac_addr& our_mac,
                        size_t* out_len)
{
    eth_frame eth;
    ipv4_packet ip;
    if (!parse_eth(frame, len, &eth) || eth.ethertype != k_ethertype_ipv4 ||
        !parse_ipv4(eth.payload, eth.payload_len, &ip) || ip.protocol != k_ip_icmp ||
        ip.payload_len < 8 || ip.payload[0] != 8)
    {
        return false;
    }
    uint8_t* icmp = const_cast<uint8_t*>(ip.payload);
    if (csum_fold(csum_add(0, icmp, ip.payload_len)) != 0xffff)
    {
        return false;
    }
    uint8_t src_mac[6];
    memcpy(src_mac, eth.src, 6);
    memcpy(frame, src_mac, 6);
    memcpy(frame + 6, our_mac.b, 6);
    icmp[0] = 0;  // echo reply
    wr16(icmp + 2, 0);
    wr16(icmp + 2, static_cast<uint16_t>(~csum_fold(csum_add(0, icmp, ip.payload_len))));
    // Answer with a plain 20-byte header (drops any IP options).
    uint8_t* iph = frame + k_eth_hdr;
    const size_t ihl = static_cast<size_t>(iph[0] & 0x0f) * 4;
    if (ihl != k_ip_hdr)
    {
        memmove(iph + k_ip_hdr, icmp, ip.payload_len);
    }
    write_ipv4(iph, k_ip_icmp, ip.dst, ip.src, static_cast<uint16_t>(ip.payload_len),
               rd16(iph + 4));
    *out_len = k_eth_hdr + k_ip_hdr + ip.payload_len;
    return true;
}

// --- DHCP ------------------------------------------------------------------------

namespace
{

constexpr size_t k_bootp_len = 236;
constexpr uint8_t k_cookie[4] = {99, 130, 83, 99};

uint8_t* put_opt(uint8_t* o, uint8_t code, const void* data, uint8_t len)
{
    o[0] = code;
    o[1] = len;
    memcpy(o + 2, data, len);
    return o + 2 + len;
}

uint8_t* put_opt32(uint8_t* o, uint8_t code, uint32_t v)
{
    uint8_t b[4];
    wr32(b, v);
    return put_opt(o, code, b, 4);
}

}  // namespace

size_t build_dhcp(uint8_t* p, dhcp_msg type, uint32_t xid, const mac_addr& chaddr,
                  uint32_t ciaddr, uint32_t requested_ip, uint32_t server_id,
                  const char* hostname, uint16_t secs)
{
    memset(p, 0, 300);
    p[0] = 1;  // BOOTREQUEST
    p[1] = 1;  // Ethernet
    p[2] = 6;
    wr32(p + 4, xid);
    wr16(p + 8, secs);
    // Without an address we cannot take a unicast reply to the new IP before
    // it is configured; ask for broadcast replies (renewals are unicast).
    wr16(p + 10, ciaddr == 0 ? 0x8000 : 0);
    wr32(p + 12, ciaddr);
    memcpy(p + 28, chaddr.b, 6);
    memcpy(p + k_bootp_len, k_cookie, 4);
    uint8_t* o = p + k_bootp_len + 4;
    const uint8_t t = static_cast<uint8_t>(type);
    o = put_opt(o, 53, &t, 1);
    uint8_t client_id[7] = {1};
    memcpy(client_id + 1, chaddr.b, 6);
    o = put_opt(o, 61, client_id, sizeof(client_id));
    if (requested_ip != 0)
    {
        o = put_opt32(o, 50, requested_ip);
    }
    if (server_id != 0)
    {
        o = put_opt32(o, 54, server_id);
    }
    if (hostname != nullptr && hostname[0] != '\0')
    {
        const size_t n = strlen(hostname);
        o = put_opt(o, 12, hostname, static_cast<uint8_t>(n > 63 ? 63 : n));
    }
    const uint8_t prl[] = {1, 3, 6, 51, 54, 58, 59};
    o = put_opt(o, 55, prl, sizeof(prl));
    uint8_t max_size[2];
    wr16(max_size, 1500);
    o = put_opt(o, 57, max_size, 2);
    *o++ = 255;
    const size_t len = static_cast<size_t>(o - p);
    return len < 300 ? 300 : len;  // BOOTP minimum
}

bool parse_dhcp(const uint8_t* p, size_t len, const mac_addr& chaddr,
                dhcp_reply* out)
{
    if (len < k_bootp_len + 4 || p[0] != 2 || p[1] != 1 || p[2] != 6 ||
        memcmp(p + 28, chaddr.b, 6) != 0 || memcmp(p + k_bootp_len, k_cookie, 4) != 0)
    {
        return false;
    }
    *out = dhcp_reply();
    out->xid = rd32(p + 4);
    out->yiaddr = rd32(p + 16);
    size_t i = k_bootp_len + 4;
    while (i < len)
    {
        const uint8_t code = p[i];
        if (code == 0)
        {
            ++i;
            continue;
        }
        if (code == 255 || i + 1 >= len)
        {
            break;
        }
        const uint8_t olen = p[i + 1];
        const uint8_t* v = p + i + 2;
        if (i + 2 + olen > len)
        {
            return false;
        }
        switch (code)
        {
        case 53:
            if (olen >= 1)
            {
                out->type = static_cast<dhcp_msg>(v[0]);
            }
            break;
        case 54:
            if (olen >= 4)
            {
                out->server_id = rd32(v);
            }
            break;
        case 1:
            if (olen >= 4)
            {
                out->subnet_mask = rd32(v);
            }
            break;
        case 3:
            if (olen >= 4)
            {
                out->router = rd32(v);
            }
            break;
        case 51:
            if (olen >= 4)
            {
                out->lease_s = rd32(v);
            }
            break;
        case 58:
            if (olen >= 4)
            {
                out->t1_s = rd32(v);
            }
            break;
        case 59:
            if (olen >= 4)
            {
                out->t2_s = rd32(v);
            }
            break;
        default:
            break;
        }
        i += 2 + olen;
    }
    return out->type != dhcp_msg::none;
}

}  // namespace net
