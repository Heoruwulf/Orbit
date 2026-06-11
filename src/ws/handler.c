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
#include <stdalign.h>
#include <stdio.h>
#include <string.h>
#include "orbit/ws_handler.h"
#include "yyjson.h"

bool ws_parse_handshake_url(
    struct string_view const req_line,
    struct string_view *restrict const out_internal_id) {
    if (req_line.data == nullptr || out_internal_id == nullptr)
        return false;

    // req_line is something like "GET /media?id=abc HTTP/1.1"
    char const *const start = req_line.data;
    char const *const end   = start + req_line.length;

    // Find '?'
    char const *q = memchr(start, '?', (size_t)(end - start));
    if (!q)
        return false;

    // Find ' ' after '?'
    char const *space = memchr(q, ' ', (size_t)(end - q));
    if (!space)
        return false;

    struct string_view query = SV_INIT_LEN(q + 1, (size_t)(space - q - 1));

    // Parse Internal ID (mapped to 'id=')
    char const *id_ptr = sv_find(query, "id=");
    if (!id_ptr)
        return false;
    id_ptr += 3; // skip "id="

    char const *id_end = memchr(id_ptr, '&', (size_t)(query.data + query.length - id_ptr));
    if (!id_end)
        id_end = query.data + query.length;

    *out_internal_id = SV_LEN(id_ptr, (size_t)(id_end - id_ptr));

    return true;
}

bool ws_parse_dtmf_json(struct string_view const text, uint8_t *restrict const out_digit) {
    if (text.data == nullptr || text.length == 0 || out_digit == nullptr)
        return false;

    alignas(64) uint8_t buf[2048];
    yyjson_alc          alc;
    yyjson_alc_pool_init(&alc, buf, sizeof(buf));

    auto const doc =
        yyjson_read_opts((char *)text.data, text.length, YYJSON_READ_NOFLAG, &alc, nullptr);
    if (doc == nullptr)
        return false;

    auto const root     = yyjson_doc_get_root(doc);
    auto const type_val = yyjson_obj_get(root, "type");
    if (!yyjson_is_str(type_val) || __builtin_strcmp(yyjson_get_str(type_val), "dtmf") != 0)
        return false;

    auto const digit_val = yyjson_obj_get(root, "digit");
    if (!yyjson_is_str(digit_val))
        return false;

    char const *c_str = yyjson_get_str(digit_val);
    if (c_str == nullptr || c_str[0] == '\0')
        return false;

    char const c = c_str[0];
    if (c >= '0' && c <= '9') {
        *out_digit = (uint8_t)(c - '0');
        return true;
    } else if (c == '*') {
        *out_digit = 10;
        return true;
    } else if (c == '#') {
        *out_digit = 11;
        return true;
    } else if (c >= 'A' && c <= 'D') {
        *out_digit = (uint8_t)(12 + (c - 'A'));
        return true;
    }
    return false;
}

size_t ws_generate_metadata_json(
    struct call_session const *restrict const session,
    char *restrict const buffer,
    size_t const max_len) {
    if (session == nullptr || buffer == nullptr || max_len == 0)
        return 0;

    char const *codec_name  = "unknown";
    uint32_t    sample_rate = 8000;
    uint32_t    channels    = 1;
    uint32_t    ptime       = session->remote_sdp.ptime > 0 ? session->remote_sdp.ptime : 20;

    if (session->remote_sdp.pcmu.payload_type != 255) {
        codec_name  = "PCMU";
        sample_rate = session->remote_sdp.pcmu.sample_rate;
        channels    = session->remote_sdp.pcmu.channels;
    } else if (session->remote_sdp.pcma.payload_type != 255) {
        codec_name  = "PCMA";
        sample_rate = session->remote_sdp.pcma.sample_rate;
        channels    = session->remote_sdp.pcma.channels;
    } else if (session->remote_sdp.opus.payload_type != 255) {
        codec_name  = "opus";
        sample_rate = session->remote_sdp.opus.sample_rate;
        channels    = session->remote_sdp.opus.channels;
    } else if (session->remote_sdp.l16.payload_type != 255) {
        codec_name  = "L16";
        sample_rate = session->remote_sdp.l16.sample_rate;
        channels    = session->remote_sdp.l16.channels;
    }

    alignas(64) uint8_t pool[2048];
    yyjson_alc          alc;
    yyjson_alc_pool_init(&alc, pool, sizeof(pool));

    yyjson_mut_doc *doc = yyjson_mut_doc_new(&alc);
    if (!doc)
        return 0;

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "type", "metadata");
    yyjson_mut_obj_add_uint(doc, root, "sample_rate", sample_rate);
    yyjson_mut_obj_add_str(doc, root, "codec", codec_name);
    yyjson_mut_obj_add_uint(doc, root, "channels", channels);
    yyjson_mut_obj_add_uint(doc, root, "ptime", ptime);
    yyjson_mut_obj_add_str(doc, root, "endianness", "big");

    yyjson_mut_val *call_id_val =
        yyjson_mut_strncpy(doc, session->call_id_buf, session->call_id_len);
    yyjson_mut_obj_add_val(doc, root, "call_id", call_id_val);

    size_t json_len = 0;
    char  *json_str = yyjson_mut_write_opts(doc, 0, &alc, &json_len, nullptr);
    if (!json_str)
        return 0;

    if (json_len >= max_len) {
        return 0;
    }

    __builtin_memcpy(buffer, json_str, json_len);
    buffer[json_len] = '\0';
    return json_len;
}

bool ws_connection_init(
    struct ws_connection               *conn,
    int                                 fd,
    struct wslay_event_callbacks const *callbacks,
    void                               *user_data) {
    if (conn == nullptr || callbacks == nullptr)
        return false;

    if (wslay_event_context_server_init(&conn->ctx, callbacks, user_data) != 0) {
        return false;
    }
    conn->fd = fd;
    return true;
}

void ws_connection_free(struct ws_connection *conn) {
    if (conn != nullptr && conn->ctx != nullptr) {
        wslay_event_context_free(conn->ctx);
        conn->ctx = nullptr;
    }
}
