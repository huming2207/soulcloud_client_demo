#pragma once

#include <esp_err.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

/**
 * Brings up the demo's network: Wi-Fi station on hardware, or the QEMU
 * OpenCores Ethernet. Reconnects Wi-Fi on its own after a drop.
 */
class demo_network
{
public:
    esp_err_t init();

    /** Wait until the interface has an IP address. */
    bool wait_for_ip(TickType_t timeout);

private:
    esp_err_t init_wifi();
    esp_err_t init_eth();
    static void event_handler(void *ctx, esp_event_base_t base, int32_t event_id, void *event_data);

    EventGroupHandle_t events = nullptr;

    static const constexpr EventBits_t BIT_GOT_IP = (1UL << 0UL);
    static const constexpr char TAG[] = "demo_net";
};
