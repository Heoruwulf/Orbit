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
#include "orbit/event.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdalign.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>
#include <unistd.h>
#include "orbit/config.h"
#include "orbit/log.h"
#include "yyjson.h"

static thread_local int                event_fd   = -1;
static thread_local struct sockaddr_in event_addr = {};

void event_init(void) {
    if (g_config.event_listen_addr == nullptr || g_config.event_listen_port == 0) {
        return; // Disabled
    }

    event_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (event_fd < 0) {
        LOGERR("Failed to create UDP event socket");
        return;
    }

    event_addr.sin_family = AF_INET;
    event_addr.sin_port   = htons(g_config.event_listen_port);
    inet_pton(AF_INET, g_config.event_listen_addr, &event_addr.sin_addr);
}

void event_publish_call_answered(
    struct sip_message const *restrict const msg,
    struct string_view const internal_id) {
    if (event_fd < 0 || msg == nullptr)
        return;

    // Create JSON using zero-allocation stack buffer pool
    alignas(8) char buffer[4096];
    yyjson_alc      alc;
    yyjson_alc_pool_init(&alc, buffer, sizeof(buffer));

    auto const mut_doc = yyjson_mut_doc_new(&alc);
    auto const root    = yyjson_mut_obj(mut_doc);
    yyjson_mut_doc_set_root(mut_doc, root);

    yyjson_mut_obj_add_str(mut_doc, root, "event", "call_answered");
    yyjson_mut_obj_add_strncpy(mut_doc, root, "call_id", msg->call_id.data, msg->call_id.length);

    if (msg->from_tag.length > 0)
        yyjson_mut_obj_add_strncpy(mut_doc, root, "from", msg->from_tag.data, msg->from_tag.length);
    if (msg->to_tag.length > 0)
        yyjson_mut_obj_add_strncpy(mut_doc, root, "to", msg->to_tag.data, msg->to_tag.length);

    if (msg->custom_header_count > 0) {
        auto const x_headers = yyjson_mut_obj_add_obj(mut_doc, root, "x_headers");
        for (size_t i = 0; i < msg->custom_header_count; ++i) {
            auto const h = &msg->custom_headers[i];
            auto const k = yyjson_mut_strncpy(mut_doc, h->name.data, h->name.length);
            auto const v = yyjson_mut_strncpy(mut_doc, h->value.data, h->value.length);
            yyjson_mut_obj_add(x_headers, k, v);
        }
    }

    char      ws_url[256];
    int const len = snprintf(
        ws_url,
        sizeof(ws_url),
        "ws://%s:%d/media?id=%.*s",
        g_config.ws_external_addr ? g_config.ws_external_addr : "127.0.0.1",
        g_config.ws_port,
        (int)internal_id.length,
        internal_id.data);

    if (len > 0 && (size_t)len < sizeof(ws_url)) {
        yyjson_mut_obj_add_str(mut_doc, root, "ws_url", ws_url);
    }

    size_t json_len = 0;
    char  *json_str = yyjson_mut_write_opts(mut_doc, 0, &alc, &json_len, nullptr);

    if (json_str == nullptr) {
        LOGERR("Failed to serialize event JSON");
        return;
    }

    // does the json exceed typical MTU size? If so, log a warning since it may be truncated by the
    // network
    if (json_len > 1400) {
        LOGWRN(
            "Event JSON is large (%zu bytes) for call-id %.*s, consider reducing the amount of "
            "data included",
            json_len,
            (int)msg->call_id.length,
            msg->call_id.data);
    }

    LOGDBG("Serialized event JSON: %.*s", (int)json_len, json_str);

    ssize_t ret =
        sendto(event_fd, json_str, json_len, 0, (struct sockaddr *)&event_addr, sizeof(event_addr));
    if (ret < 0) {
        LOGERR("Failed to send UDP event: %s", strerror(errno));
    } else {
        LOGINF("Sent UDP event: %.*s", (int)json_len, json_str);
    }
}
