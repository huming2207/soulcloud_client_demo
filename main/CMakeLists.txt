idf_component_register(
    SRCS
        "main.cpp"
        "demo_network.cpp" "demo_network.hpp"

    INCLUDE_DIRS
        "."

    PRIV_REQUIRES
        "soulcloud"
        "on9log"
        "nvs_flash"
        "esp_netif"
        "esp_event"
        "esp_wifi"
        "esp_eth"
        "esp_timer"
)
