#ifndef WINJECT_NET_MPLANE_UDP_H_
#define WINJECT_NET_MPLANE_UDP_H_

#include "mplane_reply.h"
#include "net_proto.h"
#include "netstack.h"

#include <stddef.h>
#include <stdint.h>

class mplane_commands;

// M-plane on UDP CONTROL_CONSOLE_PORT over the raw stack (docs/mplane.md):
// one datagram = newline-separated lines; all reply lines go back to the
// sender in as few datagrams as possible. Runs on the net task.
class mplane_udp : public mplane_reply
{
public:
    // Extra command family (e.g. iq_*): gets the line with any `cmd:<u8>`
    // prefix removed and a reply that decorates it. Return false to fall
    // through to mplane_commands.
    using line_handler = bool (*)(void* ctx, char* line, mplane_reply& reply);

    static mplane_udp& instance();
    bool init(netstack& net, mplane_commands& commands, line_handler extra,
              void* extra_ctx);

    void write(const char* data, size_t n) override;
    // Sender of the datagram being handled (nullptr outside a handler).
    const netstack::udp_peer* current_peer() const
    {
        return peer_set_ ? &peer_ : nullptr;
    }

private:
    mplane_udp() = default;
    static void on_udp(void* ctx, const netstack::udp_rx& rx);
    void handle(const netstack::udp_rx& rx);
    void handle_line(char* line);
    void flush();

    netstack* net_ = nullptr;
    mplane_commands* commands_ = nullptr;
    line_handler extra_ = nullptr;
    void* extra_ctx_ = nullptr;
    netstack::udp_peer peer_ = {};
    bool peer_set_ = false;
    size_t reply_len_ = 0;
    char recv_[net::k_udp_payload_max + 1];
    uint8_t frame_[net::k_eth_max];
};

#endif  // WINJECT_NET_MPLANE_UDP_H_
