/*
Orbit: High-performance, zero-allocation bi-directional audio bridge.
Copyright (C) 2026 Mark Horila

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "orbit/config.h"
#include <stdlib.h>
#include "orbit/log.h"

struct app_config g_config = {};

static char const *get_env_strict(char const *const name) {
    auto const val = getenv(name);
    if (val == nullptr || val[0] == '\0') {
        return nullptr;
    }
    return val;
}

static char const *get_env_or_default(
    char const *const name,          // NOLINT(bugprone-easily-swappable-parameters)
    char const *const default_val) { // NOLINT(bugprone-easily-swappable-parameters)
    auto const val = get_env_strict(name);
    if (val == nullptr) {
        return default_val;
    }
    return val;
}

static int get_env_or_default_int(
    char const *const name,
    int const         default_val) { // NOLINT(bugprone-easily-swappable-parameters)
    auto const val = get_env_strict(name);
    if (val == nullptr) {
        return default_val;
    }
    return atoi(val);
}

static bool is_valid_port(int const port) { return (port > 1024 && port < 65536) != 0; }

static void config_print(void) {
    LOGINF(
        "Config: SIP_LISTEN_ADDR = %s",
        g_config.sip_listen_addr ? g_config.sip_listen_addr : "(null)");
    LOGINF("Config: SIP_LISTEN_PORT = %d", g_config.sip_listen_port);
    LOGINF(
        "Config: SIP_EXTERNAL_ADDR = %s",
        g_config.sip_external_addr ? g_config.sip_external_addr : "(null)");
    LOGINF(
        "Config: RTP_LISTEN_ADDR = %s",
        g_config.rtp_listen_addr ? g_config.rtp_listen_addr : "(null)");
    LOGINF(
        "Config: RTP_EXTERNAL_ADDR = %s",
        g_config.rtp_external_addr ? g_config.rtp_external_addr : "(null)");
    LOGINF("Config: RTP_MIN_PORT = %d", g_config.rtp_min_port);
    LOGINF("Config: RTP_MAX_PORT = %d", g_config.rtp_max_port);
    LOGINF(
        "Config: WS_LISTEN_ADDR = %s",
        g_config.ws_listen_addr ? g_config.ws_listen_addr : "(null)");
    LOGINF(
        "Config: WS_EXTERNAL_ADDR = %s",
        g_config.ws_external_addr ? g_config.ws_external_addr : "(null)");
    LOGINF("Config: WS_LISTEN_PORT = %d", g_config.ws_listen_port);
    LOGINF("Config: WS_EXTERNAL_PORT = %d", g_config.ws_external_port);
    LOGINF(
        "Config: EVENT_UDP_LISTEN_ADDR = %s",
        g_config.event_udp_listen_addr ? g_config.event_udp_listen_addr : "(null)");
    LOGINF("Config: EVENT_UDP_LISTEN_PORT = %d", g_config.event_udp_listen_port);
    LOGINF("Config: MAX_CALLS = %u", g_config.max_calls);
    LOGINF("Config: EVENT_PROVIDER = %s", g_config.event_provider);
    LOGINF("Config: EVENT_QUEUE_CAPACITY = %u", g_config.event_queue_capacity);
    LOGINF(
        "Config: EVENT_REDIS_CHANNEL = %s",
        g_config.event_redis_channel ? g_config.event_redis_channel : "(null)");
    LOGINF("Config: EVENT_REDIS_DATABASE = %d", g_config.event_redis_db);
    LOGINF(
        "Config: EVENT_REDIS_HOST = %s",
        g_config.event_redis_host ? g_config.event_redis_host : "(null)");
    LOGINF(
        "Config: EVENT_REDIS_PASSWORD = %s",
        g_config.event_redis_password ? "(redacted)" : "(null)");
    LOGINF("Config: EVENT_REDIS_PORT = %d", g_config.event_redis_port);
    LOGINF(
        "Config: EVENT_REDIS_USERNAME = %s",
        g_config.event_redis_username ? g_config.event_redis_username : "(null)");
    LOGINF(
        "Config: EVENT_KAFKA_BROKERS = %s",
        g_config.event_kafka_brokers ? g_config.event_kafka_brokers : "(null)");
    LOGINF(
        "Config: EVENT_KAFKA_TOPIC = %s",
        g_config.event_kafka_topic ? g_config.event_kafka_topic : "(null)");
}

int config_load(void) {
    auto const sip_listen     = get_env_or_default("SIP_LISTEN_ADDR", "0.0.0.0");
    auto const sip_port       = (uint16_t)get_env_or_default_int("SIP_LISTEN_PORT", 5060);
    auto const sip_ext        = get_env_or_default("SIP_EXTERNAL_ADDR", "127.0.0.1");
    auto const rtp_listen     = get_env_or_default("RTP_LISTEN_ADDR", "0.0.0.0");
    auto const rtp_ext        = get_env_or_default("RTP_EXTERNAL_ADDR", "127.0.0.1");
    auto const min_port       = (uint16_t)get_env_or_default_int("RTP_MIN_PORT", 16000);
    auto const max_port       = (uint16_t)get_env_or_default_int("RTP_MAX_PORT", 32000);
    auto const ws_listen      = get_env_or_default("WS_LISTEN_ADDR", "0.0.0.0");
    auto const ws_ext         = get_env_or_default("WS_EXTERNAL_ADDR", "127.0.0.1");
    auto const ws_listen_port = (uint16_t)get_env_or_default_int("WS_LISTEN_PORT", 8080);
    auto const ws_external_port =
        (uint16_t)get_env_or_default_int("WS_EXTERNAL_PORT", (int)ws_listen_port);
    auto const event_listen = get_env_strict("EVENT_UDP_LISTEN_ADDR");
    auto const event_port   = (uint16_t)get_env_or_default_int("EVENT_UDP_LISTEN_PORT", 0);
    auto       max_calls    = (uint32_t)get_env_or_default_int("MAX_CALLS", 1024);
    auto const event_prov   = get_env_or_default("EVENT_PROVIDER", "udp");
    auto const event_q_cap  = (uint32_t)get_env_or_default_int("EVENT_QUEUE_CAPACITY", 16384);
    auto const redis_host   = get_env_or_default("EVENT_REDIS_HOST", "127.0.0.1");
    auto const redis_port   = (uint16_t)get_env_or_default_int("EVENT_REDIS_PORT", 6379);
    auto const redis_chan   = get_env_or_default("EVENT_REDIS_CHANNEL", "orbit:events");
    auto const redis_user   = get_env_strict("EVENT_REDIS_USERNAME");
    auto const redis_pass   = get_env_strict("EVENT_REDIS_PASSWORD");
    auto const redis_db     = get_env_or_default_int("EVENT_REDIS_DATABASE", 0);
    auto const kafka_brok   = get_env_or_default("EVENT_KAFKA_BROKERS", "127.0.0.1:9092");
    auto const kafka_top    = get_env_or_default("EVENT_KAFKA_TOPIC", "orbit-events");

    if (!is_valid_port(min_port) || !is_valid_port(max_port) || min_port >= max_port) {
        LOGERR(
            "Config: Invalid RTP port range (%d-%d). Ports must be between 1025 and 65535, and min "
            "must be less than max.",
            min_port,
            max_port);
        return -1;
    }

    if (!is_valid_port(ws_listen_port)) {
        LOGERR(
            "Config: Invalid WS_LISTEN_PORT (%d). Must be between 1025 and 65535.",
            ws_listen_port);
        return -1;
    }

    if (!is_valid_port(ws_external_port)) {
        LOGERR(
            "Config: Invalid WS_EXTERNAL_PORT (%d). Must be between 1025 and 65535.",
            ws_external_port);
        return -1;
    }

    // Validate event port only if addr is present
    if (event_listen != nullptr && (!is_valid_port(event_port))) {
        LOGERR(
            "Config: Invalid EVENT_UDP_LISTEN_PORT (%d). Must be between 1025 and 65535 if "
            "EVENT_UDP_LISTEN_ADDR is set.",
            event_port);
        return -1;
    }

    // Validate Redis port only if host is present
    if (redis_host != nullptr && (!is_valid_port(redis_port))) {
        LOGERR(
            "Config: Invalid EVENT_REDIS_PORT (%d). Must be between 1025 and 65535 if "
            "EVENT_REDIS_HOST is set.",
            redis_port);
        return -1;
    }

    if (redis_db < 0 || redis_db > 15) {
        LOGERR("Config: Invalid EVENT_REDIS_DATABASE (%d). Must be between 0 and 15.", redis_db);
        return -1;
    }

    if (max_calls < 128) {
        max_calls = 128; // Enforce a reasonable minimum to avoid instability
    } else if (max_calls > 8192) {
        max_calls = 8192; // Enforce a reasonable maximum to avoid resource exhaustion
    }

    if (event_q_cap < max_calls || event_q_cap > max_calls * 16) {
        LOGERR(
            "Config: Invalid EVENT_QUEUE_CAPACITY (%u). Must be between %u and %u.",
            event_q_cap,
            max_calls,
            max_calls * 16);
        return -1;
    }

    uint32_t const safe_margin     = 256;
    uint32_t const required_ports  = max_calls + safe_margin;
    uint32_t const available_ports = (uint32_t)(max_port - min_port + 1);

    if (available_ports < required_ports) {
        LOGERR(
            "Config: RTP port range (%u-%u) provides %u ports, but %u are required for MAX_CALLS "
            "(%u) + safe margin (%u).",
            min_port,
            max_port,
            available_ports,
            required_ports,
            max_calls,
            safe_margin);
        return -1;
    }

    g_config.sip_listen_addr       = sip_listen;
    g_config.sip_listen_port       = sip_port;
    g_config.sip_external_addr     = sip_ext;
    g_config.rtp_listen_addr       = rtp_listen;
    g_config.rtp_external_addr     = rtp_ext;
    g_config.rtp_min_port          = min_port;
    g_config.rtp_max_port          = max_port;
    g_config.ws_listen_port        = ws_listen_port;
    g_config.ws_external_port      = ws_external_port;
    g_config.ws_listen_addr        = ws_listen;
    g_config.ws_external_addr      = ws_ext;
    g_config.event_udp_listen_addr = event_listen;
    g_config.event_udp_listen_port = event_port;
    g_config.max_calls             = max_calls;
    g_config.event_provider        = event_prov;
    g_config.event_queue_capacity  = event_q_cap;
    g_config.event_redis_host      = redis_host;
    g_config.event_redis_port      = redis_port;
    g_config.event_redis_channel   = redis_chan;
    g_config.event_redis_username  = redis_user;
    g_config.event_redis_password  = redis_pass;
    g_config.event_redis_db        = redis_db;
    g_config.event_kafka_brokers   = kafka_brok;
    g_config.event_kafka_topic     = kafka_top;

    config_print();

    return 0;
}
