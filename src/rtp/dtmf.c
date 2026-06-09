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
#include "orbit/rtp_engine.h"
#include "orbit/sip_router.h"

bool rtp_generate_dtmf(
    struct call_session *restrict const session,
    uint8_t const  digit,
    bool const     is_start,
    bool const     is_end,
    uint16_t const duration,
    void *restrict const buffer,
    size_t *restrict const out_length) {
    if (session == nullptr || buffer == nullptr || out_length == nullptr)
        return false;

    uint8_t const pt = session->remote_sdp.dtmf.payload_type;
    if (pt == 255 || pt == 0)
        return false; // Not negotiated

    uint8_t *bytes = (uint8_t *)buffer;
    bytes[0]       = 0x80; // V=2
    bytes[1]       = pt | (is_start ? 0x80 : 0x00);

    bytes[2] = (uint8_t)(session->tx_seq_num >> 8);
    bytes[3] = (uint8_t)(session->tx_seq_num & 0xFF);

    bytes[4] = (uint8_t)(session->tx_timestamp >> 24);
    bytes[5] = (uint8_t)(session->tx_timestamp >> 16);
    bytes[6] = (uint8_t)(session->tx_timestamp >> 8);
    bytes[7] = (uint8_t)(session->tx_timestamp & 0xFF);

    bytes[8]  = (uint8_t)(session->tx_ssrc >> 24);
    bytes[9]  = (uint8_t)(session->tx_ssrc >> 16);
    bytes[10] = (uint8_t)(session->tx_ssrc >> 8);
    bytes[11] = (uint8_t)(session->tx_ssrc & 0xFF);

    bytes[12] = digit;
    bytes[13] = (uint8_t)((is_end ? 0x80 : 0x00) | 10); // Volume = 10
    bytes[14] = (uint8_t)(duration >> 8);
    bytes[15] = (uint8_t)(duration & 0xFF);

    *out_length = 16;
    session->tx_seq_num++;
    return true;
}
