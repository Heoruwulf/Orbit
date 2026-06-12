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
#pragma once

#include <stdint.h>

/**
 * @brief Global application configuration settings.
 *
 * Holds the parsed and validated network settings, port ranges, and system limits
 * read from environment variables during startup.
 */
struct app_config {
    char const *sip_listen_addr;       /**< IP address to bind the SIP listener. */
    char const *sip_external_addr;     /**< Public IP address advertised in SIP Via/Contact. */
    char const *rtp_listen_addr;       /**< IP address to bind RTP sockets. */
    char const *rtp_external_addr;     /**< Public IP address advertised in SDP answers. */
    char const *ws_listen_addr;        /**< IP address to bind the WebSocket listener. */
    char const *ws_external_addr;      /**< Public IP address used for WebSocket media URLs. */
    char const *event_udp_listen_addr; /**< Target UDP IP address for event publishing. */
    uint16_t    sip_listen_port;       /**< Local port for the SIP listener (e.g. 5060). */
    uint16_t    rtp_min_port;          /**< Minimum port number of the RTP dynamic port range. */
    uint16_t    rtp_max_port;          /**< Maximum port number of the RTP dynamic port range. */
    uint16_t    ws_listen_port;   /**< Local port for the WebSocket TCP listener (e.g. 8080). */
    uint16_t    ws_external_port; /**< External port advertised for WebSocket connections. */
    uint16_t    event_udp_listen_port; /**< Target UDP port for event publishing. */
    uint32_t    max_calls;             /**< Maximum concurrent call sessions allowed. */
    char const *event_provider; /**< Event provider type (udp, redis, kafka, mock, disabled). */
    uint32_t    event_queue_capacity;  /**< Event queue capacity per worker. */
    char const *event_redis_host;      /**< Redis host address. */
    uint16_t    event_redis_port;      /**< Redis host port. */
    char const *event_redis_channel;   /**< Redis channel for publishing. */
    char const *event_redis_username;  /**< Redis username for authentication. */
    char const *event_redis_password;  /**< Redis password for authentication. */
    int32_t     event_redis_db;        /**< Redis database number. */
    char const *event_kafka_brokers;   /**< Kafka bootstrap brokers. */
    char const *event_kafka_topic;     /**< Kafka topic name. */
    char const *ws_codec_payload_type; /**< Expected WS payload format. */
    uint16_t    ws_codec_sample_rate;  /**< Expected WS sample rate. */
    uint16_t    ws_codec_channels;     /**< Expected WS channels. */
    char const *ws_codec_endian;       /**< Expected endianness (little, big, none). */
    bool        ws_codec_vad_enable;   /**< Activate VAD on WebSocket bridge. */
    char const *vad_file;              /**< Path to external VAD file. */
};

extern struct app_config g_config;

/**
 * @brief Loads application configuration from environment variables.
 *
 * Reads variables such as SIP_LISTEN_ADDR, SIP_LISTEN_PORT, RTP_MIN_PORT,
 * RTP_MAX_PORT, WS_LISTEN_PORT, and MAX_CALLS. Validates the configuration bounds
 * and updates the global configuration structure g_config.
 *
 * @return 0 on success, or -1 if any validation fails.
 */
int config_load(void);
