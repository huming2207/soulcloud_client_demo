#define ON9_LOG_LOCAL_LEVEL 5

/*
 * Soulcloud client demo: WiFi STA + client lifecycle + periodic on9log
 * telemetry. Commands are registered in demo_commands.
 */

#include <cstdio>
#include <cstring>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
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

namespace soulcloud_demo
{
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
        void run();  // never returns

    private:
        demo_app() = default;

        static constexpr char TAG[] = "soulcloud_demo";
        static constexpr uint32_t WIFI_CONNECTED_BIT = BIT0;
        static constexpr uint32_t WIFI_FAIL_BIT = BIT1;
        static constexpr uint32_t WIFI_RETRY_MAX = 10;

        EventGroupHandle_t wifi_events_ = nullptr;

        static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
        static void connection_cb(bool connected, void *ctx);
        esp_err_t wifi_connect();
        void run_loop();  // telemetry loop body
    };

    // ------------------------------------------------------------------ //

    void demo_app::wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
    {
        demo_app *self = static_cast<demo_app *>(arg);
        if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
            // auto-reconnect (bounded by the retry counter in wifi_connect)
            esp_wifi_connect();
        } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            const ip_event_got_ip_t *evt = static_cast<const ip_event_got_ip_t *>(data);
            ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&evt->ip_info.ip));
            xEventGroupSetBits(self->wifi_events_, WIFI_CONNECTED_BIT);
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
        // NVS (config + credentials storage)
        ESP_ERROR_CHECK(nvs_flash_init());

        // on9log: core + UART/VFS sink for local debugging; the MQTT sink
        // (component) is layered on top later
        if (on9log_init() != ON9LOG_OK) {
            ESP_LOGE(TAG, "on9log_init failed");
            return ESP_FAIL;
        }
        ESP_ERROR_CHECK(on9log_esp_vfs_init());

        // network stack
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        return ESP_OK;
    }

    esp_err_t demo_app::wifi_connect()
    {
        wifi_events_ = xEventGroupCreate();
        if (wifi_events_ == nullptr) {
            return ESP_ERR_NO_MEM;
        }

        if (esp_netif_create_default_wifi_sta() == nullptr) {
            return ESP_FAIL;
        }

        const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, this));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, this));

        wifi_config_t wifi_cfg = {};
        snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", CONFIG_SOULCLOUD_DEMO_WIFI_SSID);
        snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", CONFIG_SOULCLOUD_DEMO_WIFI_PASSWORD);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "connecting to wifi \"%s\"...", CONFIG_SOULCLOUD_DEMO_WIFI_SSID);
        const EventBits_t bits = xEventGroupWaitBits(wifi_events_,
                                                     WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                     pdFALSE, pdFALSE,
                                                     pdMS_TO_TICKS(30000));
        if ((bits & WIFI_CONNECTED_BIT) == 0) {
            ESP_LOGE(TAG, "wifi connect timeout");
            return ESP_ERR_WIFI_NOT_CONNECT;
        }
        return ESP_OK;
    }

    esp_err_t demo_app::start()
    {
        ESP_ERROR_CHECK(wifi_connect());

        soulcloud::config cfg;
        ESP_ERROR_CHECK(soulcloud::config_store::instance().load(&cfg));

        soulcloud::soulcloud_client &client = soulcloud::soulcloud_client::instance();
        ESP_ERROR_CHECK(demo_commands::register_all(client));
        client.set_connection_cb(connection_cb, this);
        ESP_ERROR_CHECK(client.init(&cfg));
        ESP_ERROR_CHECK(client.start());
        return ESP_OK;
    }

    void demo_app::run_loop()
    {
        uint32_t tick = 0;
        for (;;) {
            // periodic telemetry (binary on9log -> UART now, MQTT once the
            // component's log sink lands)
            ON9_LOGI(TAG, "tick=%u uptime=%llds heap=%u",
                     tick,
                     (long long)(esp_timer_get_time() / 1000000),
                     (unsigned)esp_get_free_heap_size());

            if (tick % 30 == 1) {
                ON9_LOGW(TAG, "sample warning: free heap %u bytes", (unsigned)esp_get_free_heap_size());
            }

            if (demo_commands::reboot_pending()) {
                ESP_LOGI(TAG, "reboot command acknowledged; restarting");
                vTaskDelay(pdMS_TO_TICKS(1000));  // let the result flush
                esp_restart();
            }

            vTaskDelay(pdMS_TO_TICKS(10000));
            tick++;
        }
    }

    void demo_app::run()
    {
        run_loop();
    }
}  // namespace soulcloud_demo

extern "C" void app_main(void)
{
    soulcloud_demo::demo_app &app = soulcloud_demo::demo_app::instance();
    ESP_ERROR_CHECK(app.init());
    ESP_ERROR_CHECK(app.start());
    app.run();
}
