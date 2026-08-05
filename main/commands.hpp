#pragma once

/*
 * Demo command handlers (getInfo, reboot, setLogging, setConfig, echo).
 */

#include <soulcloud.hpp>

namespace soulcloud_demo
{
    class demo_commands
    {
    public:
        /** Registers all demo commands on the client. */
        static esp_err_t register_all(soulcloud::soulcloud_client &client);

        /** True after a reboot command was acknowledged. */
        static bool reboot_pending();

    private:
        demo_commands() = delete;

        static constexpr char TAG[] = "demo_cmds";

        static esp_err_t get_info(const soulcloud::command_exec *cmd, soulcloud::command_result *out);
        static esp_err_t reboot(const soulcloud::command_exec *cmd, soulcloud::command_result *out);
        static esp_err_t set_logging(const soulcloud::command_exec *cmd, soulcloud::command_result *out);
        static esp_err_t set_config(const soulcloud::command_exec *cmd, soulcloud::command_result *out);
        static esp_err_t echo(const soulcloud::command_exec *cmd, soulcloud::command_result *out);

        static volatile bool s_reboot_pending;
    };
}  // namespace soulcloud_demo
