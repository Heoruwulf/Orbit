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

struct app_config {
    char const *sip_listen_addr;
    char const *sip_external_addr;
    char const *rtp_listen_addr;
    char const *rtp_external_addr;
    char const *ws_listen_addr;
    char const *ws_external_addr;
    char const *event_listen_addr;
    uint16_t    sip_listen_port;
    uint16_t    rtp_min_port;
    uint16_t    rtp_max_port;
    uint16_t    ws_listen_port;
    uint16_t    ws_external_port;
    uint16_t    event_listen_port;
    uint32_t    max_calls;
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

