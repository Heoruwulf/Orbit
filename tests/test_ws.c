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
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "orbit/ws_handler.h"

static void test_ws_handshake(void) {
    char const         req[]  = "GET /media?id=550e8400-e29b-41d4-a716-446655440000 HTTP/1.1\r\n";
    struct string_view req_sv = SV(req);

    struct string_view internal_id = {};

    assert(ws_parse_handshake_url(req_sv, &internal_id) == true);

    assert(internal_id.length == 36);
    assert(strncmp(internal_id.data, "550e8400-e29b-41d4-a716-446655440000", 36) == 0);

    struct call_session session      = {};
    char                callid_str[] = "abc123xyz";
    size_t const        copy_len     = sizeof(callid_str) - 1;
    __builtin_memcpy(session.call_id_buf, callid_str, copy_len);
    session.call_id_len = copy_len;
    // Default config values (255 represents undefined/empty in our struct logic usually, but here
    // we set to 0 to simulate active)
    session.remote_sdp.pcmu.payload_type = 0;
    session.remote_sdp.pcmu.sample_rate  = 8000;
    session.remote_sdp.pcmu.channels     = 1;
    session.remote_sdp.ptime             = 20;

    // Set other codecs to "inactive" (255) so the parser picks PCMU
    session.remote_sdp.pcma.payload_type = 255;
    session.remote_sdp.opus.payload_type = 255;
    session.remote_sdp.l16.payload_type  = 255;

    char         buffer[256] = {};
    size_t const len         = ws_generate_metadata_json(&session, buffer, sizeof(buffer));

    assert(len > 0);
    assert(
        strcmp(
            buffer,
            "{\"type\":\"metadata\",\"sample_rate\":8000,\"codec\":\"PCMU\",\"channels\":1,"
            "\"ptime\":20,\"endianness\":\"big\",\"call_id\":\"abc123xyz\"}") == 0);

    printf("Websocket handshake and metadata JSON tests passed.\n");
}

static ssize_t mock_recv_callback(
    wslay_event_context_ptr ctx,
    uint8_t                *buf,
    size_t                  len,
    int                     flags,
    void                   *user_data) {
    (void)ctx;
    (void)flags;
    struct string_view *sv = (struct string_view *)user_data;
    if (sv->length == 0) {
        wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        return -1;
    }

    size_t copy_len = len < sv->length ? len : sv->length;
    __builtin_memcpy(buf, sv->data, copy_len);
    sv->data += copy_len;
    sv->length -= copy_len;
    return (ssize_t)copy_len;
}

static ssize_t mock_send_callback(
    wslay_event_context_ptr ctx,
    const uint8_t          *data,
    size_t                  len,
    int                     flags,
    void                   *user_data) {
    (void)ctx;
    (void)flags;
    (void)user_data;
    return (ssize_t)len;
}

static void mock_msg_recv_callback(
    wslay_event_context_ptr                   ctx,
    const struct wslay_event_on_msg_recv_arg *arg,
    void                                     *user_data) {
    (void)ctx;
    (void)user_data;
    assert(arg->msg_length == 5);
    assert(arg->msg[0] == 'H');
    assert(arg->msg[4] == 'o');
}

static void test_wslay_integration(void) {
    struct wslay_event_callbacks callbacks = {
        mock_recv_callback,
        mock_send_callback,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        mock_msg_recv_callback};

    uint8_t mock_ws_frame[] = {
        0x82,
        0x85, // FIN+Bin, Masked, len=5
        0x37,
        0xfa,
        0x21,
        0x3d, // Masking Key
        0x7f,
        0x9f,
        0x4d,
        0x51,
        0x58 // Masked Payload
    }; // "Hello"

    struct string_view sv = SV_INIT_LEN((char *)mock_ws_frame, sizeof(mock_ws_frame));

    struct ws_connection conn = {};
    assert(ws_connection_init(&conn, -1, &callbacks, &sv) == true);

    assert(wslay_event_recv(conn.ctx) == 0);

    ws_connection_free(&conn);

    printf("wslay integration tests passed.\n");
}

static void test_ws_dtmf_json(void) {
    uint8_t digit = 255;

    assert(ws_parse_dtmf_json(SV("{\"type\":\"dtmf\",\"digit\":\"9\"}"), &digit) == true);
    assert(digit == 9);

    assert(ws_parse_dtmf_json(SV("{\"type\":\"dtmf\",\"digit\":\"*\"}"), &digit) == true);
    assert(digit == 10);

    assert(ws_parse_dtmf_json(SV("{\"type\":\"dtmf\",\"digit\":\"#\"}"), &digit) == true);
    assert(digit == 11);

    assert(ws_parse_dtmf_json(SV("{\"type\":\"dtmf\",\"digit\":\"A\"}"), &digit) == true);
    assert(digit == 12);

    assert(ws_parse_dtmf_json(SV("{\"type\":\"dtmf\",\"digit\":\"D\"}"), &digit) == true);
    assert(digit == 15);

    assert(ws_parse_dtmf_json(SV("{\"type\":\"dtmf\",\"digit\":\"X\"}"), &digit) == false);
    assert(ws_parse_dtmf_json(SV("HELLO"), &digit) == false);

    printf("Websocket DTMF JSON parser tests passed.\n");
}

int main(void) {
    test_ws_handshake();
    test_wslay_integration();
    test_ws_dtmf_json();
    return 0;
}
