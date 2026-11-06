#include "mplane_udp.h"

#include "config.h"
#include "mplane_commands.h"
#include "mplane_req_id.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"

static const char* TAG = "mplane";

mplane_udp& mplane_udp::instance()
{
    static mplane_udp inst;
    return inst;
}

bool mplane_udp::init(netstack& net, mplane_commands& commands, line_handler extra,
                      void* extra_ctx)
{
    net_ = &net;
    commands_ = &commands;
    extra_ = extra;
    extra_ctx_ = extra_ctx;
    if (!net.set_udp_handler(CONTROL_CONSOLE_PORT, on_udp, this))
    {
        ESP_LOGE(TAG, "no UDP handler slot");
        return false;
    }
    ESP_LOGI(TAG, "m-plane on UDP %u (raw stack)", CONTROL_CONSOLE_PORT);
    return true;
}

void mplane_udp::on_udp(void* ctx, const netstack::udp_rx& rx)
{
    static_cast<mplane_udp*>(ctx)->handle(rx);
}

void mplane_udp::handle(const netstack::udp_rx& rx)
{
    if (rx.len == 0)
    {
        return;
    }
    memcpy(recv_, rx.data, rx.len);
    recv_[rx.len] = '\0';
    peer_.ip = rx.src_ip;
    peer_.port = rx.src_port;
    peer_.mac = rx.src_mac;  // reply to the hop the request came from
    peer_set_ = true;
    reply_len_ = 0;
    char* line = recv_;
    while (line != nullptr)
    {
        char* next = strchr(line, '\n');
        if (next != nullptr)
        {
            *next++ = '\0';
        }
        handle_line(line);
        line = next;
    }
    flush();
    peer_set_ = false;
}

void mplane_udp::handle_line(char* line)
{
    if (extra_ != nullptr)
    {
        // Peek at the command word behind an optional `cmd:<u8>` prefix
        // without disturbing the line mplane_commands may still parse.
        char copy[256];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char* text = copy;
        while (isspace(static_cast<unsigned char>(*text)))
        {
            ++text;
        }
        uint8_t req_id = 0;
        bool have_id = false;
        if (strncmp(text, "cmd:", 4) == 0 || strncmp(text, "iq_", 3) == 0)
        {
            if (mplane_peel_cmd_prefix(&text, &req_id, &have_id) &&
                strncmp(text, "iq_", 3) == 0 && strlen(line) < sizeof(copy))
            {
                if (have_id)
                {
                    mplane_req_id_reply decorated(*this, req_id);
                    if (extra_(extra_ctx_, text, decorated))
                    {
                        return;
                    }
                }
                else if (extra_(extra_ctx_, text, *this))
                {
                    return;
                }
            }
        }
    }
    commands_->handle_line(line, *this);
}

void mplane_udp::write(const char* data, size_t n)
{
    if (!peer_set_ || data == nullptr)
    {
        return;
    }
    char* buf = reinterpret_cast<char*>(frame_ + net::k_udp_payload_off);
    while (n > 0)
    {
        if (reply_len_ == net::k_udp_payload_max)
        {
            flush();
        }
        const size_t room = net::k_udp_payload_max - reply_len_;
        const size_t chunk = n < room ? n : room;
        memcpy(buf + reply_len_, data, chunk);
        reply_len_ += chunk;
        data += chunk;
        n -= chunk;
    }
}

void mplane_udp::flush()
{
    if (peer_set_ && reply_len_ > 0)
    {
        net_->send_udp(frame_, reply_len_, peer_, CONTROL_CONSOLE_PORT);
    }
    reply_len_ = 0;
}
