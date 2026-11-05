#ifndef WINJECT_OTA_H_
#define WINJECT_OTA_H_

#include "esp_http_server.h"

class manager;

void otaBegin(manager& netmgr);
bool otaActive();
// Shared httpd (port OTA_HTTP_PORT); nullptr if it failed to start.
httpd_handle_t otaHttpd();

#endif  // WINJECT_OTA_H_
