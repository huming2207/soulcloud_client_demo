#define ON9_LOG_LOCAL_LEVEL 5

/*
 * Soulcloud client demo: WiFi STA or Ethernet (OpenETH for QEMU) +
 * client lifecycle + periodic on9log telemetry. Commands are registered
 * in demo_commands.
 */

#include <cstdio>
#include <cstring>

#include <esp_err.h>
#include <esp_eth.h>
#include <esp_event.h>
#if CONFIG_SOULCLOUD_CORE_TASK_STACK_PSRAM
#include <esp_flash_dispatcher.h>
#endif
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <on9log.h>
#include <on9log_esp_vfs.h>
#include <soulcloud.hpp>

#include "commands.hpp"

using namespace soulcloud_demo;

class demo_app
{
public:
    static demo_app &instance()
    {
        static demo_app s_instance;
        return s_instance;
    }

    demo_app(const demo_app &) = delete;
    demo_app &operator=(const demo_app &) = delete;

    esp_err_t init();
    esp_err_t start();
    void run(); // never returns

private:
    demo_app() = default;

    static constexpr char TAG[] = "soulcloud_demo";
    static constexpr uint32_t NET_CONNECTED_BIT = BIT0;
    static constexpr uint32_t NET_FAIL_BIT = BIT1;

    EventGroupHandle_t net_events_ = nullptr;

    static void net_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
    static void connection_cb(bool connected, void *ctx);
    esp_err_t net_connect();
#if CONFIG_SOULCLOUD_DEMO_NET_ETH
    esp_err_t eth_connect();
#else
    esp_err_t wifi_connect();
#endif
    void run_loop(); // telemetry loop body
};

// ------------------------------------------------------------------ //

void demo_app::net_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    demo_app *self = static_cast<demo_app *>(arg);

#if !CONFIG_SOULCLOUD_DEMO_NET_ETH
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // auto-reconnect
        esp_wifi_connect();
    } else
#endif
        if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "ethernet link up");
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "ethernet link down");
    } else if (base == IP_EVENT && (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP)) {
        const ip_event_got_ip_t *evt = static_cast<const ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(self->net_events_, NET_CONNECTED_BIT);
        soulcloud::soulcloud_client::instance().notify_wifi_connected();
    }
}

void demo_app::connection_cb(bool connected, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "soulcloud %s", connected ? "connected" : "disconnected");
}

esp_err_t demo_app::init()
{
    // When this demo places soulcloud's large core-task stack in PSRAM,
    // route cache-disabling flash operations (including NVS) through
    // Espressif's small internal-RAM worker before creating that task.
#if CONFIG_SOULCLOUD_CORE_TASK_STACK_PSRAM
    const esp_flash_dispatcher_config_t flash_dispatcher_cfg = ESP_FLASH_DISPATCHER_DEFAULT_CONFIG;
    ESP_ERROR_CHECK(esp_flash_dispatcher_init(&flash_dispatcher_cfg));
#endif

    // NVS (config + credentials storage)
    ESP_ERROR_CHECK(nvs_flash_init());

    // on9log: core + serial sink for local debugging; the MQTT sink
    // (component) is layered on top later. Both sinks run in parallel:
    // the serial sink emits the SLIP-encoded stream and the component's
    // sink forwards the same packets to the `log` topic. On hardware the
    // stream goes to the console UART (decode with the on9log host tool,
    // github.com/huming2207/on9log_host); in the QEMU build it goes to
    // UART1 so the emulated console stays clean for ESP_LOG text (the
    // E2E harness matches it).
    if (on9log_init() != ON9LOG_OK) {
        ESP_LOGE(TAG, "on9log_init failed");
        return ESP_FAIL;
    }
#if CONFIG_SOULCLOUD_DEMO_NET_ETH
    // QEMU build: skip the VFS (SLIP) sink on purpose. The SLIP stream
    // would interleave with ESP_LOG on the emulated console and tear the
    // text lines the E2E harness matches on (two writers on the same
    // UART are not line-atomic). Logs still flow over MQTT in this build;
    // the console stays plain ESP_LOG text.
#else
    // Real device: dual output — the SLIP stream on the console (decode
    // with the on9log host tool) plus the MQTT sink installed by the
    // component.
    ESP_ERROR_CHECK(on9log_esp_vfs_init());
#endif

    // network stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    return ESP_OK;
}

#if CONFIG_SOULCLOUD_DEMO_NET_ETH

esp_err_t demo_app::eth_connect()
{
    net_events_ = xEventGroupCreate();
    if (net_events_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // OpenCores Ethernet (only exists in QEMU)
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    // QEMU's openeth model only answers MII accesses for PHY address 1
    // (DEFAULT_PHY in hw/net/opencores_eth.c); address 0 returns 0xffff,
    // which trips the 802.3 power-up poll in esp_eth_phy_802_3_pwrctl.
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (mac == nullptr || phy == nullptr) {
        ESP_LOGE(TAG, "openeth mac/phy init failed");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = nullptr;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    // deterministic MAC from eFuse (burn_custom_mac in QEMU works too)
    uint8_t mac_addr[6] = {};
    esp_read_mac(mac_addr, ESP_MAC_ETH);
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    if (eth_netif == nullptr) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, net_event_handler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, net_event_handler, this));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "waiting for ethernet link + dhcp...");
    const EventBits_t bits = xEventGroupWaitBits(net_events_, NET_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if ((bits & NET_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "ethernet connect timeout");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

#else // WiFi STA

esp_err_t demo_app::wifi_connect()
{
    net_events_ = xEventGroupCreate();
    if (net_events_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    if (esp_netif_create_default_wifi_sta() == nullptr) {
        return ESP_FAIL;
    }

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, net_event_handler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, net_event_handler, this));

    wifi_config_t wifi_cfg = {};
    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", CONFIG_SOULCLOUD_DEMO_WIFI_SSID);
    snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", CONFIG_SOULCLOUD_DEMO_WIFI_PASSWORD);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to wifi \"%s\"...", CONFIG_SOULCLOUD_DEMO_WIFI_SSID);
    const EventBits_t bits = xEventGroupWaitBits(net_events_, NET_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    if ((bits & NET_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "wifi connect timeout");
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    return ESP_OK;
}

#endif

esp_err_t demo_app::net_connect()
{
#if CONFIG_SOULCLOUD_DEMO_NET_ETH
    return eth_connect();
#else
    return wifi_connect();
#endif
}

esp_err_t demo_app::start()
{
    ESP_ERROR_CHECK(net_connect());

    soulcloud::config cfg;
    ESP_ERROR_CHECK(soulcloud::config_store::instance().load(&cfg));

    soulcloud::soulcloud_client &client = soulcloud::soulcloud_client::instance();
    ESP_ERROR_CHECK(soulcloud_demo::demo_commands::register_all(client, xTaskGetCurrentTaskHandle()));
    client.set_connection_cb(connection_cb, this);
    ESP_ERROR_CHECK(client.init(&cfg));
    ESP_ERROR_CHECK(client.start());
    return ESP_OK;
}

void demo_app::run_loop()
{
    uint32_t tick = 0;
    for (;;) {
        // periodic telemetry (binary on9log -> MQTT via the component's log
        // sink)
        ON9_LOGI(TAG, "tick=%u uptime=%llds heap=%u", tick, (long long)(esp_timer_get_time() / 1000000),
                 (unsigned)esp_get_free_heap_size());

        if (tick % 30 == 1) {
            ON9_LOGW(TAG, "sample warning: free heap %u bytes", (unsigned)esp_get_free_heap_size());
        }

        // A command handler wakes this task directly; the timeout also
        // provides the 10-second telemetry cadence without polling a
        // cross-task flag.
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000)) > 0) {
            ESP_LOGI(TAG, "reboot command acknowledged; restarting");
            vTaskDelay(pdMS_TO_TICKS(1000)); // let the result flush
            esp_restart();
        }
        tick++;
    }
}

void demo_app::run()
{
    run_loop();
}

extern "C" void app_main(void)
{
    demo_app &app = demo_app::instance();
    ESP_ERROR_CHECK(app.init());
    ESP_ERROR_CHECK(app.start());
    app.run();
}
