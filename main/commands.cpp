/*
 * Demo command handlers.
 */

#include "commands.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <on9log.h>

#include <soulcloud_types.hpp>

using namespace soulcloud_demo;

volatile bool soulcloud_demo::demo_commands::s_reboot_pending = false;

// Per-instance state handed to every handler as its ctx (see commands.hpp).
// Handlers run synchronously on the MQTT event task, so the buffers are
// single-threaded.
demo_state soulcloud_demo::demo_commands::s_state;

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

bool soulcloud_demo::demo_commands::reboot_pending()
{
    return s_reboot_pending;
}

esp_err_t soulcloud_demo::demo_commands::register_all(soulcloud::soulcloud_client &client)
{
    // Register with the shared state as ctx. Errors are returned (not
    // asserted) so app_main decides what to do.
    ESP_RETURN_ON_ERROR(client.register_command("getInfo", get_info, &s_state), TAG,
                        "register getInfo");
    ESP_RETURN_ON_ERROR(client.register_command("reboot", reboot, &s_state), TAG,
                        "register reboot");
    ESP_RETURN_ON_ERROR(client.register_command("setLogging", set_logging, &s_state), TAG,
                        "register setLogging");
    ESP_RETURN_ON_ERROR(client.register_command("setConfig", set_config, &s_state), TAG,
                        "register setConfig");
    ESP_RETURN_ON_ERROR(client.register_command("echo", echo, &s_state), TAG,
                        "register echo");
    return ESP_OK;
}

esp_err_t soulcloud_demo::demo_commands::get_info(const soulcloud::command_exec *cmd, soulcloud::command_result *out, void *ctx)
{
    (void)cmd;
    demo_state *st = static_cast<demo_state *>(ctx);
    if (st == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    soulcloud::config cfg;
    soulcloud::config_store::instance().load(&cfg);
    snprintf(st->uid, sizeof(st->uid), "%s", cfg.device_uid);
    esp_app_get_elf_sha256(st->fw, sizeof(st->fw));
    snprintf(st->rst, sizeof(st->rst), "%s", reset_reason_str(esp_reset_reason()));

    wifi_ap_record_t ap = {};
    int32_t rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }

    uint32_t i = 0;
    st->args[i].key = "device_uid";
    st->args[i].key_len = 10;
    st->args[i].value = make_str(st->uid);
    i++;
    st->args[i].key = "fw_sha256";
    st->args[i].key_len = 9;
    st->args[i].value = make_str(st->fw);
    i++;
    st->args[i].key = "uptime_s";
    st->args[i].key_len = 8;
    st->args[i].value = make_int((int64_t)(esp_timer_get_time() / 1000000));
    i++;
    st->args[i].key = "reset_reason";
    st->args[i].key_len = 12;
    st->args[i].value = make_str(st->rst);
    i++;
    st->args[i].key = "wifi_rssi";
    st->args[i].key_len = 9;
    st->args[i].value = make_int(rssi);
    i++;
    st->args[i].key = "heap_free";
    st->args[i].key_len = 9;
    st->args[i].value = make_int((int64_t)esp_get_free_heap_size());
    i++;

    out->code = 0;
    out->args = st->args;
    out->arg_count = i;
    return ESP_OK;
}

esp_err_t soulcloud_demo::demo_commands::reboot(const soulcloud::command_exec *cmd, soulcloud::command_result *out, void *ctx)
{
    (void)cmd;
    (void)ctx;
    out->code = 0;
    s_reboot_pending = true;  // main loop performs the actual restart
    return ESP_OK;
}

esp_err_t soulcloud_demo::demo_commands::set_logging(const soulcloud::command_exec *cmd, soulcloud::command_result *out, void *ctx)
{
    (void)ctx;
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

esp_err_t soulcloud_demo::demo_commands::set_config(const soulcloud::command_exec *cmd, soulcloud::command_result *out, void *ctx)
{
    (void)ctx;
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

    using store = soulcloud::config_store;

    // String keys: whitelist + content checks (the component's set_string
    // writes any NVS key, so unknown keys must be rejected here).
    const bool is_uid = strcmp(key_buf, store::KEY_UID) == 0;
    if (is_uid || strcmp(key_buf, store::KEY_PASS) == 0 ||
        strcmp(key_buf, store::KEY_SERIAL) == 0 ||
        strcmp(key_buf, store::KEY_BROKER) == 0 ||
        strcmp(key_buf, store::KEY_API) == 0) {
        // uid must stay topic-safe: no '/', '+', '#' or whitespace
        if (is_uid) {
            for (size_t i = 0; i < strlen(value_buf); ++i) {
                const char c = value_buf[i];
                if (c == '/' || c == '+' || c == '#' || c == ' ' || c == '\t') {
                    ESP_LOGW(TAG, "setConfig: uid contains a topic-reserved character");
                    out->code = -1;
                    return ESP_OK;
                }
            }
        }
        const esp_err_t err = store::instance().set_string(key_buf, value_buf);
        ESP_LOGI(TAG, "setConfig %s=%s -> %s", key_buf, value_buf, esp_err_to_name(err));
        out->code = (err == ESP_OK) ? 0 : -2;
        return ESP_OK;
    }

    // Numeric keys: whitelist + range check (same ranges the component
    // clamps to on load; a value outside them is rejected, not stored).
    struct num_limits
    {
        const char *key;
        uint32_t min;
        uint32_t max;
    };
    static constexpr num_limits NUMERIC[] = {
        {store::KEY_STAT_INT, 1, 86400},
        {store::KEY_LOG_RATE, 1, 1000},
        {store::KEY_MQTT_IN, 512, 65536},
        {store::KEY_MQTT_OUT, 512, 65536},
        {store::KEY_KA, 5, 3600},
        {store::KEY_RECONN, 100, 600000},
        {store::KEY_OTA_MAX, 65536, 67108864},
        {store::KEY_OTA_TO, 1, 3600},
    };
    for (const num_limits &lim : NUMERIC) {
        if (strcmp(key_buf, lim.key) == 0) {
            char *end = nullptr;
            errno = 0;
            const unsigned long v = strtoul(value_buf, &end, 10);
            if (errno != 0 || end == value_buf || *end != '\0' || v < lim.min || v > lim.max) {
                ESP_LOGW(TAG, "setConfig: %s=%s out of range [%lu, %lu]",
                         key_buf, value_buf, (unsigned long)lim.min, (unsigned long)lim.max);
                out->code = -1;
                return ESP_OK;
            }
            const esp_err_t err = store::instance().set_u32(key_buf, (uint32_t)v);
            ESP_LOGI(TAG, "setConfig %s=%lu -> %s", key_buf, v, esp_err_to_name(err));
            out->code = (err == ESP_OK) ? 0 : -2;
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "setConfig: unknown key %s", key_buf);
    out->code = -1;
    return ESP_OK;
}

esp_err_t soulcloud_demo::demo_commands::echo(const soulcloud::command_exec *cmd, soulcloud::command_result *out, void *ctx)
{
    (void)ctx;
    out->code = 0;
    out->args = cmd->args;  // echo the received args verbatim
    out->arg_count = cmd->arg_count;
    return ESP_OK;
}
