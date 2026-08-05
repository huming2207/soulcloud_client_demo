/*
 * Demo command handlers.
 */

#include "commands.hpp"

#include <cstdio>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <on9log.h>

#include <soulcloud_types.hpp>

namespace soulcloud_demo
{
    volatile bool demo_commands::s_reboot_pending = false;

    namespace
    {
        // static storage for handler payloads (handlers run synchronously
        // on the MQTT event task, so this is single-threaded)
        soulcloud::cmd_arg s_args[6];
        char s_uid[128];
        char s_fw[65];
        char s_rst[16];

        const char *reset_reason_str(esp_reset_reason_t r)
        {
            switch (r) {
            case ESP_RST_UNKNOWN: return "UNKNOWN";
            case ESP_RST_POWERON: return "POWERON";
            case ESP_RST_EXT: return "EXT";
            case ESP_RST_SW: return "SW";
            case ESP_RST_PANIC: return "PANIC";
            case ESP_RST_INT_WDT: return "INT_WDT";
            case ESP_RST_TASK_WDT: return "TASK_WDT";
            case ESP_RST_WDT: return "WDT";
            case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
            case ESP_RST_BROWNOUT: return "BROWNOUT";
            case ESP_RST_SDIO: return "SDIO";
            case ESP_RST_USB: return "USB";
            case ESP_RST_JTAG: return "JTAG";
            case ESP_RST_EFUSE: return "EFUSE";
            case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
            case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
            default: return "UNKNOWN";
            }
        }

        soulcloud::cmd_arg_value make_str(const char *s)
        {
            soulcloud::cmd_arg_value v = {};
            v.type = soulcloud::cmd_arg_value::TYPE_STR;
            v.str.ptr = s;
            v.str.len = (uint32_t)strlen(s);
            return v;
        }

        soulcloud::cmd_arg_value make_int(int64_t i)
        {
            soulcloud::cmd_arg_value v = {};
            v.type = soulcloud::cmd_arg_value::TYPE_INT;
            v.i = i;
            return v;
        }

        soulcloud::cmd_arg_value make_bool(bool b)
        {
            soulcloud::cmd_arg_value v = {};
            v.type = soulcloud::cmd_arg_value::TYPE_BOOL;
            v.b = b;
            return v;
        }
    }  // namespace

    bool demo_commands::reboot_pending()
    {
        return s_reboot_pending;
    }

    esp_err_t demo_commands::register_all(soulcloud::soulcloud_client &client)
    {
        ESP_ERROR_CHECK(client.register_command("getInfo", get_info));
        ESP_ERROR_CHECK(client.register_command("reboot", reboot));
        ESP_ERROR_CHECK(client.register_command("setLogging", set_logging));
        ESP_ERROR_CHECK(client.register_command("setConfig", set_config));
        ESP_ERROR_CHECK(client.register_command("echo", echo));
        return ESP_OK;
    }

    esp_err_t demo_commands::get_info(const soulcloud::command_exec *cmd, soulcloud::command_result *out)
    {
        (void)cmd;

        soulcloud::config cfg;
        soulcloud::config_store::instance().load(&cfg);
        snprintf(s_uid, sizeof(s_uid), "%s", cfg.device_uid);
        esp_app_get_elf_sha256(s_fw, sizeof(s_fw));
        snprintf(s_rst, sizeof(s_rst), "%s", reset_reason_str(esp_reset_reason()));

        wifi_ap_record_t ap = {};
        int32_t rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }

        uint32_t i = 0;
        s_args[i].key = "device_uid";
        s_args[i].key_len = 10;
        s_args[i].value = make_str(s_uid);
        i++;
        s_args[i].key = "fw_sha256";
        s_args[i].key_len = 9;
        s_args[i].value = make_str(s_fw);
        i++;
        s_args[i].key = "uptime_s";
        s_args[i].key_len = 8;
        s_args[i].value = make_int((int64_t)(esp_timer_get_time() / 1000000));
        i++;
        s_args[i].key = "reset_reason";
        s_args[i].key_len = 12;
        s_args[i].value = make_str(s_rst);
        i++;
        s_args[i].key = "wifi_rssi";
        s_args[i].key_len = 9;
        s_args[i].value = make_int(rssi);
        i++;
        s_args[i].key = "heap_free";
        s_args[i].key_len = 9;
        s_args[i].value = make_int((int64_t)esp_get_free_heap_size());
        i++;

        out->code = 0;
        out->args = s_args;
        out->arg_count = i;
        return ESP_OK;
    }

    esp_err_t demo_commands::reboot(const soulcloud::command_exec *cmd, soulcloud::command_result *out)
    {
        (void)cmd;
        out->code = 0;
        s_reboot_pending = true;  // main loop performs the actual restart
        return ESP_OK;
    }

    esp_err_t demo_commands::set_logging(const soulcloud::command_exec *cmd, soulcloud::command_result *out)
    {
        // args: [{enabled: bool}]
        bool enabled = false;
        for (uint32_t i = 0; i < cmd->arg_count; ++i) {
            const soulcloud::cmd_arg *a = &cmd->args[i];
            if (a->key_len == 7 && memcmp(a->key, "enabled", 7) == 0 &&
                a->value.type == soulcloud::cmd_arg_value::TYPE_BOOL) {
                enabled = a->value.b;
            }
        }
        on9log_set_level(enabled ? ON9_LOG_LEVEL_VERBOSE : ON9_LOG_LEVEL_INFO);
        ESP_LOGI(TAG, "setLogging: %s", enabled ? "verbose" : "info");
        out->code = 0;
        return ESP_OK;
    }

    esp_err_t demo_commands::set_config(const soulcloud::command_exec *cmd, soulcloud::command_result *out)
    {
        // args: [{key: str}, {value: str}]
        const char *key = nullptr;
        uint32_t key_len = 0;
        const char *value = nullptr;
        uint32_t value_len = 0;
        for (uint32_t i = 0; i < cmd->arg_count; ++i) {
            const soulcloud::cmd_arg *a = &cmd->args[i];
            if (a->value.type == soulcloud::cmd_arg_value::TYPE_STR) {
                if (a->key_len == 3 && memcmp(a->key, "key", 3) == 0) {
                    key = a->value.str.ptr;
                    key_len = a->value.str.len;
                } else if (a->key_len == 5 && memcmp(a->key, "value", 5) == 0) {
                    value = a->value.str.ptr;
                    value_len = a->value.str.len;
                }
            }
        }
        if (key == nullptr || key_len == 0 || key_len >= 16) {
            out->code = -1;
            return ESP_OK;
        }

        char key_buf[16] = {};
        memcpy(key_buf, key, key_len);
        char value_buf[256] = {};
        if (value != nullptr && value_len < sizeof(value_buf)) {
            memcpy(value_buf, value, value_len);
        }

        const esp_err_t err = soulcloud::config_store::instance().set_string(key_buf, value_buf);
        ESP_LOGI(TAG, "setConfig %s=%s -> %s", key_buf, value_buf, esp_err_to_name(err));
        out->code = (err == ESP_OK) ? 0 : -2;
        return ESP_OK;
    }

    esp_err_t demo_commands::echo(const soulcloud::command_exec *cmd, soulcloud::command_result *out)
    {
        out->code = 0;
        out->args = cmd->args;  // echo the received args verbatim
        out->arg_count = cmd->arg_count;
        return ESP_OK;
    }
}  // namespace soulcloud_demo
