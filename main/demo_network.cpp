#include "demo_network.hpp"

#include <esp_eth.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>

esp_err_t demo_network::init()
{
    events = xEventGroupCreate();
    if (events == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    auto ret = esp_netif_init();
    ret = ret ?: esp_event_loop_create_default();
    ret = ret ?: esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, event_handler, this);
    ret = ret ?: esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init: event setup failed: 0x%x %s", ret, esp_err_to_name(ret));
        return ret;
    }

#if CONFIG_SOULCLOUD_DEMO_NET_ETH
    return init_eth();
#else
    return init_wifi();
#endif
}

esp_err_t demo_network::init_wifi()
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    auto ret = esp_wifi_init(&init_cfg);

    wifi_config_t wifi_cfg = {};
    strlcpy(reinterpret_cast<char *>(wifi_cfg.sta.ssid), CONFIG_SOULCLOUD_DEMO_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy(reinterpret_cast<char *>(wifi_cfg.sta.password), CONFIG_SOULCLOUD_DEMO_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    ret = ret ?: esp_wifi_set_mode(WIFI_MODE_STA);
    ret = ret ?: esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    ret = ret ?: esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init_wifi: failed: 0x%x %s", ret, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t demo_network::init_eth()
{
#if CONFIG_ETH_USE_OPENETH
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_cfg);
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);

    esp_eth_handle_t eth = nullptr;
    auto ret = esp_eth_driver_install(&eth_cfg, &eth);
    ret = ret ?: esp_netif_attach(netif, esp_eth_new_netif_glue(eth));
    ret = ret ?: esp_eth_start(eth);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init_eth: failed: 0x%x %s", ret, esp_err_to_name(ret));
    }
    return ret;
#else
    ESP_LOGE(TAG, "init_eth: enable CONFIG_ETH_USE_OPENETH (QEMU only)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void demo_network::event_handler(void *ctx, esp_event_base_t base, int32_t event_id, void *event_data)
{
    auto *net = static_cast<demo_network *>(ctx);
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
        xEventGroupClearBits(net->events, BIT_GOT_IP);
        esp_wifi_connect();
    } else if (base == IP_EVENT && (event_id == IP_EVENT_STA_GOT_IP || event_id == IP_EVENT_ETH_GOT_IP)) {
        auto *got_ip = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&got_ip->ip_info.ip));
        xEventGroupSetBits(net->events, BIT_GOT_IP);
    }
}

bool demo_network::wait_for_ip(TickType_t timeout)
{
    return (xEventGroupWaitBits(events, BIT_GOT_IP, pdFALSE, pdTRUE, timeout) & BIT_GOT_IP) != 0;
}
