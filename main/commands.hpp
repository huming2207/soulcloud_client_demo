#pragma once

/*
 * Demo command handlers (getInfo, reboot, setLogging, setConfig, echo).
 *
 * The handlers demonstrate the ctx parameter of
 * soulcloud::soulcloud_client::register_command(): all mutable state
 * lives in a single demo_state instance and is reached through ctx, so
 * no process-wide statics are needed and the handlers stay re-entrant
 * across instances.
 */

#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soulcloud.hpp>

namespace soulcloud_demo
{
    /**
     * @brief Per-instance state for the demo handlers.
     *
     * Handlers run synchronously on the dedicated soulcloud core task,
     * so the payload buffers below are single-threaded.
     */
    struct demo_state {
        soulcloud::cmd_arg args[6]; // getInfo result payload slots
        char uid[128];              // getInfo: device uid
        char fw[65];                // getInfo: ELF SHA-256 hex
        char rst[16];               // getInfo: reset reason
        TaskHandle_t app_task;      // notified after reboot result is prepared
    };

    class demo_commands
    {
    public:
        /**
         * @brief Registers all demo commands on the client.
         *
         * Each command is registered with the same demo_state instance as
         * its ctx. Returns the first registration error instead of
         * asserting, so the caller (app_main) decides what to do.
         *
         * @param[in] client Client to register on.
         * @return ESP_OK or ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM.
         */
        static esp_err_t register_all(soulcloud::soulcloud_client &client, TaskHandle_t app_task);

    private:
        demo_commands() = delete;

        static constexpr char TAG[] = "demo_cmds";

        static demo_state s_state; // ctx passed to every registration

        static esp_err_t get_info(const soulcloud::command_exec *cmd, soulcloud::command_result *out, void *ctx);
        static esp_err_t reboot(const soulcloud::command_exec *cmd, soulcloud::command_result *out, void *ctx);
        static esp_err_t set_logging(const soulcloud::command_exec *cmd, soulcloud::command_result *out, void *ctx);
        static esp_err_t set_config(const soulcloud::command_exec *cmd, soulcloud::command_result *out, void *ctx);
        static esp_err_t echo(const soulcloud::command_exec *cmd, soulcloud::command_result *out, void *ctx);

        static bool contains_nul(const char *value, size_t len);
    };
} // namespace soulcloud_demo
