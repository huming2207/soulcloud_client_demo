#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <on9log.hpp>
#include <on9log_esp_vfs.h>
#include <soulcloud.hpp>

#include "demo_network.hpp"

static const char *TAG = "main";
static uint32_t tick_count = 0;

static esp_err_t init_nvs()
{
    auto ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ret = nvs_flash_erase();
        ret = ret ?: nvs_flash_init();
    }
    return ret;
}

/** Examples of the client's extension points. */
static void register_demo_features(sc_client *client)
{
    // A custom request: the web console's Commands tab can send {"method":"demo.echo","params":{"text":"hi"}}
    client->register_method("demo.echo", [](sc_rpc_call &call) {
        char text[64] = {};
        if (!call.params.get_str(0, "text", text, sizeof(text))) {
            call.fail("params.text must be a string of at most %u bytes", sizeof(text) - 1);
            return ESP_ERR_INVALID_ARG;
        }
        call.result.add_str("text", text);
        return ESP_OK;
    });

    // A custom shell command: "demo ticks" in the web console's Shell tab
    client->register_shell_cmd("demo ticks", "Demo loop iterations", [](int, char **, sc_shell_out &out) {
        out.printf("ticks= %lu\n", static_cast<unsigned long>(tick_count));
        return 0;
    });

    // Extra status fields, sent with every heartbeat
    client->set_status_cb([](sc_json_writer &status) {
#if CONFIG_SOULCLOUD_DEMO_NET_WIFI
        int rssi = 0;
        if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
            status.add_int("rssi", rssi);
        }
#endif
        status.begin_object("extra").add_uint("ticks", tick_count).end_object();
    });
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Started");

    // Console output goes through on9log's framed UART transport; decode it with the on9log host CLI.
    on9log_esp_vfs_init();

    auto ret = init_nvs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: 0x%x %s", ret, esp_err_to_name(ret));
        return;
    }

    static demo_network net;
    ret = net.init();
    if (ret != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "Waiting for the network");
    net.wait_for_ip(portMAX_DELAY);

    sc_config cfg = {};
    cfg.pairing_token = CONFIG_SOULCLOUD_DEMO_PAIRING_TOKEN;
    cfg.device_name = CONFIG_SOULCLOUD_DEMO_DEVICE_NAME;

    auto *client = sc_client::instance();
    ret = client->init(cfg);
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "Not enrolled: set a pairing token in menuconfig (SoulCloud demo)");
        return;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SoulCloud init failed: 0x%x %s", ret, esp_err_to_name(ret));
        return;
    }

    register_demo_features(client);
    client->start();
    ESP_LOGI(TAG, "Device %s started", client->device_id());

    // Binary logs: decoded on the platform with the ELF uploaded alongside the firmware.
    static const on9log::Logger log("demo");
    while (true) {
        tick_count += 1;
        log.info("tick {} free heap {} bytes", tick_count, heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
