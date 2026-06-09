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
#include <string.h>
#include "orbit/sip_router.h"

static int parse_int(char const *ptr, size_t const len) {
    int val = 0;
    for (size_t i = 0; i < len; ++i) {
        if (ptr[i] >= '0' && ptr[i] <= '9') {
            val = (val * 10) + (ptr[i] - '0');
        } else {
            break;
        }
    }
    return val;
}

bool sdp_parse(struct string_view const body, struct sdp_media_info *restrict const out_info) {
    if (body.data == nullptr || body.length == 0 || out_info == nullptr)
        return false;

    *out_info = (struct sdp_media_info){};
    // Default to invalid payload types (255)
    out_info->opus.payload_type = 255;
    out_info->pcmu.payload_type = 255;
    out_info->pcma.payload_type = 255;
    out_info->l16.payload_type  = 255;
    out_info->dtmf.payload_type = 255;

    char const       *ptr = body.data;
    char const *const end = body.data + body.length;

    while (ptr < end) {
        char const *line_end = memchr(ptr, '\n', (size_t)(end - ptr));
        if (line_end == nullptr) {
            line_end = end;
        }

        size_t line_len = (size_t)(line_end - ptr);
        if (line_len > 0 && *(line_end - 1) == '\r') {
            line_len--;
        }

        if (line_len >= 9 && memcmp(ptr, "c=IN IP4 ", 9) == 0) {
            out_info->ip_addr.data   = ptr + 9;
            out_info->ip_addr.length = line_len - 9;
        } else if (line_len >= 8 && memcmp(ptr, "m=audio ", 8) == 0) {
            out_info->port = (uint16_t)parse_int(ptr + 8, line_len - 8);
        } else if (line_len >= 9 && memcmp(ptr, "a=rtpmap:", 9) == 0) {
            int const         pt    = parse_int(ptr + 9, line_len - 9);
            char const *const space = memchr(ptr + 9, ' ', line_len - 9);
            if (space != nullptr) {
                char const *const codec     = space + 1;
                size_t const      remaining = (size_t)(ptr + line_len - codec);

                struct sdp_codec_info *target    = nullptr;
                size_t                 codec_len = 0;

                if (remaining >= 4 && memcmp(codec, "OPUS", 4) == 0) {
                    target    = &out_info->opus;
                    codec_len = 4;
                } else if (remaining >= 4 && memcmp(codec, "PCMU", 4) == 0) {
                    target    = &out_info->pcmu;
                    codec_len = 4;
                } else if (remaining >= 4 && memcmp(codec, "PCMA", 4) == 0) {
                    target    = &out_info->pcma;
                    codec_len = 4;
                } else if (remaining >= 3 && memcmp(codec, "L16", 3) == 0) {
                    target    = &out_info->l16;
                    codec_len = 3;
                } else if (remaining >= 15 && memcmp(codec, "telephone-event", 15) == 0) {
                    target    = &out_info->dtmf;
                    codec_len = 15;
                }

                if (target != nullptr) {
                    target->payload_type = (uint8_t)pt;
                    target->channels     = 1; // Default
                    if (remaining > codec_len && codec[codec_len] == '/') {
                        char const *const rate_ptr = codec + codec_len + 1;
                        size_t const      rate_rem = (size_t)(ptr + line_len - rate_ptr);
                        target->sample_rate        = (uint32_t)parse_int(rate_ptr, rate_rem);

                        char const *const slash = memchr(rate_ptr, '/', rate_rem);
                        if (slash != nullptr) {
                            target->channels = (uint8_t)parse_int(
                                slash + 1,
                                (size_t)(ptr + line_len - (slash + 1)));
                        }
                    }
                }
            }
        } else if (line_len >= 8 && memcmp(ptr, "a=ptime:", 8) == 0) {
            out_info->ptime = (uint32_t)parse_int(ptr + 8, line_len - 8);
        }

        ptr = line_end + 1;
    }

    return out_info->port != 0 && out_info->ip_addr.length > 0;
}

#include <stdio.h>
#include "orbit/config.h"

size_t sdp_generate_reply(
    struct call_session const *restrict const session,
    char *restrict const buffer,
    size_t const max_len) {
    if (session == nullptr || buffer == nullptr || max_len == 0)
        return 0;

    int const written = snprintf(
        buffer,
        max_len,
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=orbit\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=audio %u RTP/AVP 0 8 111 96 101\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=rtpmap:111 OPUS/48000/2\r\n"
        "a=rtpmap:96 L16/16000/1\r\n"
        "a=rtpmap:101 telephone-event/8000\r\n"
        "a=fmtp:101 0-15\r\n"
        "a=sendrecv\r\n",
        g_config.rtp_external_addr ? g_config.rtp_external_addr : "127.0.0.1",
        g_config.rtp_external_addr ? g_config.rtp_external_addr : "127.0.0.1",
        session->local_sdp.port);

    if (written < 0 || (size_t)written >= max_len) {
        return 0;
    }

    return (size_t)written;
}

size_t sdp_generate_capabilities_reply(char *restrict const buffer, size_t const max_len) {
    if (buffer == nullptr || max_len == 0)
        return 0;

    int const written = snprintf(
        buffer,
        max_len,
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=orbit\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=audio 0 RTP/AVP 0 8 111 96 101\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=rtpmap:111 OPUS/48000/2\r\n"
        "a=rtpmap:96 L16/16000/1\r\n"
        "a=rtpmap:101 telephone-event/8000\r\n"
        "a=fmtp:101 0-15\r\n"
        "a=sendrecv\r\n",
        g_config.rtp_external_addr ? g_config.rtp_external_addr : "127.0.0.1",
        g_config.rtp_external_addr ? g_config.rtp_external_addr : "127.0.0.1");

    if (written < 0 || (size_t)written >= max_len) {
        return 0;
    }

    return (size_t)written;
}
