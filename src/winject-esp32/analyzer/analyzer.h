#ifndef WINJECT_ANALYZER_ANALYZER_H_
#define WINJECT_ANALYZER_ANALYZER_H_

#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"

class wifi;

// WiFi analyzer front end (docs/api.md): serves the embedded web app and the
// /api/v1 HTTP + WebSocket API on the shared httpd, and streams promiscuous
// frames with RSSI and frame-aligned CSI to WebSocket clients.
//
// Only the `frames` mode is implemented; `iq` / `sweep` (ESP-SDR) answer
// 501 until the RF-dump receiver lands.
namespace analyzer
{

// Registers the web app and API handlers. radio == nullptr (OTA mode or radio
// init failure): info/config still answer, the frame stream stays idle.
bool begin(httpd_handle_t server, wifi* radio);

}  // namespace analyzer

#endif  // WINJECT_ANALYZER_ANALYZER_H_
