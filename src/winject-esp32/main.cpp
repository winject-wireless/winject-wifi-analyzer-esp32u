// winject-wifi-analyzer-esp32u: ESP32 IQ sample server.
//
// Boot: settings -> WiFi PHY (ESP-SDR minimal init, only to power and
// calibrate the receiver) -> raw Ethernet stack (no lwIP) -> mplane on
// UDP 22 -> IQ server (samples as WJIQ v2 datagrams). Firmware updates are
// serial only.

#include "config.h"
#include "device_backend_esp.h"
#include "iq_server.h"
#include "mplane_commands.h"
#include "mplane_udp.h"
#include "netstack.h"
#include "settings.h"

#include <optional>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char* TAG = "main";

static std::optional<device_backend_esp> g_device;
static std::optional<mplane_commands> g_commands;

static bool init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        if (nvs_flash_erase() == ESP_OK)
        {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs init failed: %s (continuing)", esp_err_to_name(err));
        return false;
    }
    return true;
}

// ESP-SDR's receiver setup (firmware/esp-sdr targets/esp32): the WiFi
// stack only powers and calibrates the PHY; no netif, no association.
static bool init_wifi_phy()
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err == ESP_OK)
    {
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (err == ESP_OK)
    {
        err = esp_wifi_set_mode(WIFI_MODE_NULL);
    }
    if (err == ESP_OK)
    {
        err = esp_wifi_start();
    }
    if (err == ESP_OK)
    {
        err = esp_wifi_set_ps(WIFI_PS_NONE);
    }
    if (err == ESP_OK)
    {
        err = esp_wifi_set_promiscuous(true);
    }
    if (err == ESP_OK)
    {
        err = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi PHY init failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "winject-wifi-analyzer-esp32u starting, reset %d",
             static_cast<int>(esp_reset_reason()));
    init_nvs();
    if (esp_event_loop_create_default() != ESP_OK)
    {
        ESP_LOGE(TAG, "event loop failed");
    }

    settings& store = settings::instance();
    store.load_boot();
    const settings_snapshot& boot = store.boot();

    netstack& net = netstack::instance();
    g_device.emplace(net, store, nullptr);
    g_commands.emplace(*g_device, nullptr, nullptr);
    mplane_udp::instance().init(net, *g_commands, iq_server::handle_line, nullptr);

    if (!net.start(boot.network, boot.tune.eth_dma_burst_len))
    {
        ESP_LOGE(TAG, "network start failed; serial console only");
    }

    // A radio failure leaves Ethernet + mplane up (iq_* reply ENODEV).
    if (!init_wifi_phy() || !iq_server::begin(net))
    {
        ESP_LOGE(TAG, "IQ receiver unavailable");
    }
}
