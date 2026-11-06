#include "net_proto.h"

#include <gtest/gtest.h>

#include <string.h>

using namespace net;

namespace
{

const mac_addr k_us = {{0x20, 0x50, 0x0d, 0x08, 0x0d, 0x6b}};
const mac_addr k_peer = {{0x02, 0x11, 0x22, 0x33, 0x44, 0x55}};
constexpr uint32_t k_ip_us = 0xc0a8fd0e;    // 192.168.253.14
constexpr uint32_t k_ip_peer = 0xc0a8fd6a;  // 192.168.253.106

}  // namespace

TEST(NetProtoTest, ChecksumMatchesRfc1071Example)
{
    // RFC 1071 section 3 example words.
    const uint8_t data[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    EXPECT_EQ(csum_fold(csum_add(0, data, sizeof(data))), 0xddf2);
}

TEST(NetProtoTest, ArpRoundTrip)
{
    uint8_t frame[64];
    arp_packet req = {};
    req.op = 1;
    req.sha = k_peer;
    req.spa = k_ip_peer;
    req.tpa = k_ip_us;
    const size_t len = build_arp(frame, k_broadcast_mac, req);
    ASSERT_EQ(len, 42u);
    eth_frame eth;
    ASSERT_TRUE(parse_eth(frame, len, &eth));
    EXPECT_EQ(eth.ethertype, k_ethertype_arp);
    arp_packet got;
    ASSERT_TRUE(parse_arp(eth.payload, eth.payload_len, &got));
    EXPECT_EQ(got.op, 1);
    EXPECT_EQ(got.spa, k_ip_peer);
    EXPECT_EQ(got.tpa, k_ip_us);
    EXPECT_EQ(memcmp(got.sha.b, k_peer.b, 6), 0);
}

TEST(NetProtoTest, UdpFrameParsesWithValidChecksums)
{
    uint8_t frame[k_eth_max] = {};
    const char msg[] = "cmd:1 iq_info";
    memcpy(frame + k_udp_payload_off, msg, sizeof(msg) - 1);
    const size_t len = finish_udp_frame(frame, k_peer, k_us, k_ip_us, k_ip_peer, 5300,
                                        40000, sizeof(msg) - 1, 7);
    ASSERT_EQ(len, k_udp_payload_off + sizeof(msg) - 1);
    eth_frame eth;
    ipv4_packet ip;
    udp_datagram udp;
    ASSERT_TRUE(parse_eth(frame, len, &eth));
    ASSERT_TRUE(parse_ipv4(eth.payload, eth.payload_len, &ip));
    EXPECT_EQ(ip.src, k_ip_us);
    EXPECT_EQ(ip.dst, k_ip_peer);
    ASSERT_TRUE(parse_udp(ip, &udp));
    EXPECT_EQ(udp.src_port, 5300);
    EXPECT_EQ(udp.dst_port, 40000);
    ASSERT_EQ(udp.payload_len, sizeof(msg) - 1);
    EXPECT_EQ(memcmp(udp.payload, msg, udp.payload_len), 0);
    // A flipped payload byte must fail the UDP checksum.
    frame[k_udp_payload_off] ^= 1;
    ASSERT_TRUE(parse_ipv4(eth.payload, eth.payload_len, &ip));
    EXPECT_FALSE(parse_udp(ip, &udp));
}

TEST(NetProtoTest, RejectsIpFragments)
{
    uint8_t frame[k_eth_max] = {};
    const size_t len = finish_udp_frame(frame, k_peer, k_us, k_ip_us, k_ip_peer, 1, 2, 4, 1);
    uint8_t* iph = frame + k_eth_hdr;
    wr16(iph + 6, 0x2000);  // more fragments
    wr16(iph + 10, 0);
    wr16(iph + 10, static_cast<uint16_t>(~csum_fold(csum_add(0, iph, k_ip_hdr))));
    ipv4_packet ip;
    EXPECT_FALSE(parse_ipv4(frame + k_eth_hdr, len - k_eth_hdr, &ip));
}

TEST(NetProtoTest, IcmpEchoBecomesReply)
{
    uint8_t frame[128] = {};
    write_eth(frame, k_us, k_peer, k_ethertype_ipv4);
    uint8_t* icmp = frame + k_eth_hdr + k_ip_hdr;
    icmp[0] = 8;  // echo request
    wr16(icmp + 4, 0x1234);
    wr16(icmp + 6, 1);
    memcpy(icmp + 8, "pingpong", 8);
    wr16(icmp + 2, static_cast<uint16_t>(~csum_fold(csum_add(0, icmp, 16))));
    write_ipv4(frame + k_eth_hdr, k_ip_icmp, k_ip_peer, k_ip_us, 16, 99);
    size_t out = 0;
    ASSERT_TRUE(icmp_echo_to_reply(frame, k_eth_hdr + k_ip_hdr + 16, k_us, &out));
    EXPECT_EQ(memcmp(frame, k_peer.b, 6), 0);
    eth_frame eth;
    ipv4_packet ip;
    ASSERT_TRUE(parse_eth(frame, out, &eth));
    ASSERT_TRUE(parse_ipv4(eth.payload, eth.payload_len, &ip));
    EXPECT_EQ(ip.src, k_ip_us);
    EXPECT_EQ(ip.dst, k_ip_peer);
    EXPECT_EQ(ip.payload[0], 0);
    EXPECT_EQ(csum_fold(csum_add(0, ip.payload, ip.payload_len)), 0xffff);
}

TEST(NetProtoTest, DhcpRequestAndOfferParse)
{
    uint8_t msg[400];
    const size_t len = build_dhcp(msg, dhcp_msg::discover, 0xdeadbeef, k_us, 0, 0, 0,
                                  "winject-iq-esp32u", 3);
    ASSERT_GE(len, 300u);
    EXPECT_EQ(msg[0], 1);
    EXPECT_EQ(rd32(msg + 4), 0xdeadbeefu);
    EXPECT_EQ(rd16(msg + 10), 0x8000);  // broadcast flag without ciaddr

    // Turn it into a server OFFER and parse it as the client.
    msg[0] = 2;
    wr32(msg + 16, k_ip_us);
    uint8_t* o = msg + 240;
    const uint8_t opts[] = {53, 1, 2,  54, 4, 192, 168, 253, 1, 1, 4, 255, 255, 255, 0,
                            3,  4, 192, 168, 253, 1, 51, 4, 0,  0, 0x0e, 0x10, 255};
    memcpy(o, opts, sizeof(opts));
    dhcp_reply r;
    ASSERT_TRUE(parse_dhcp(msg, 240 + sizeof(opts), k_us, &r));
    EXPECT_EQ(r.type, dhcp_msg::offer);
    EXPECT_EQ(r.xid, 0xdeadbeefu);
    EXPECT_EQ(r.yiaddr, k_ip_us);
    EXPECT_EQ(r.server_id, 0xc0a8fd01u);
    EXPECT_EQ(r.subnet_mask, 0xffffff00u);
    EXPECT_EQ(r.router, 0xc0a8fd01u);
    EXPECT_EQ(r.lease_s, 3600u);
    // Not for us: different chaddr.
    EXPECT_FALSE(parse_dhcp(msg, 240 + sizeof(opts), k_peer, &r));
}
