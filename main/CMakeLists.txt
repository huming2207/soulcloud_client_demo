idf_component_register(SRCS "main.cpp" "commands.cpp"
                       REQUIRES soulcloud on9log esp_flash_dispatcher
                       PRIV_REQUIRES nvs_flash esp_wifi esp_eth esp_netif esp_event esp_timer esp_app_format log)
