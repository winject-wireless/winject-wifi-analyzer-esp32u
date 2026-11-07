// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WINJECT_IQ_IQ_SERVER_H_
#define WINJECT_IQ_IQ_SERVER_H_

#include "mplane_reply.h"

class netstack;

// IQ sample server (docs/radio-server.md): mplane `iq_*` commands tune the
// ESP-SDR receiver and start capture/sweep jobs; samples leave as WJIQ v2
// datagrams (docs/sample-protocol.md) on the raw Ethernet stack.
namespace iq_server
{

// Initializes the receiver (WiFi PHY must be up) and starts the IQ task.
bool begin(netstack& net);

// mplane_udp extra-line handler: true when the line was an iq_* command.
bool handle_line(void* ctx, char* line, mplane_reply& reply);

}  // namespace iq_server

#endif  // WINJECT_IQ_IQ_SERVER_H_
