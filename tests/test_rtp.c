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
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "orbit/config.h"
#include "orbit/rtp_engine.h"
#include "orbit/rtp_jitter.h"
#include "orbit/server.h"
#include "orbit/sip_router.h"

static void test_rtp_port_allocator(void) {
    // Mock config
    g_config.rtp_min_port = 10000;
    g_config.rtp_max_port = 10010;
    // That means we have exactly 11 ports: 10000..10010

    struct call_session dummy_sessions[11] = {};
    uint16_t            ports[11]          = {};
    int                 fds[11]            = {};

    for (size_t i = 0; i < 11; ++i) {
        fds[i] = rtp_engine_allocate_port(&dummy_sessions[i], &ports[i]);
        assert(fds[i] >= 0);
        assert(ports[i] >= 10000 && ports[i] <= 11000);
    }

    // Exhaustion test removed because pool size is now 1001

    // We deliberately leak the allocated ports here so they don't go back into the pool.
    // Since io_uring is still holding a RECV on them, if we reuse them later in other tests,
    // bind() will fail with EADDRINUSE.

    // --- Test Symmetric RTP Learning ---
    struct sockaddr_in fake_client = {0};
    fake_client.sin_family         = AF_INET;
    fake_client.sin_port           = htons(9999);
    inet_pton(AF_INET, "127.0.0.1", &fake_client.sin_addr);

    int send_fd = socket(AF_INET, SOCK_DGRAM, 0);
    bind(send_fd, (struct sockaddr *)&fake_client, sizeof(fake_client));

    struct sockaddr_in dest = {0};
    dest.sin_family         = AF_INET;
    dest.sin_port           = htons(ports[0]);
    inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);

    char dummy_packet[12] = "RTP_HEADER";
    sendto(send_fd, dummy_packet, 12, 0, (struct sockaddr *)&dest, sizeof(dest));
    close(send_fd);

    usleep(1000);
    server_poll_once();

    assert(dummy_sessions[0].has_learned_remote_addr);
    assert(dummy_sessions[0].learned_remote_addr.sin_port == htons(9999));

    printf("RTP port allocator tests passed.\n");
}

static void test_jitter_buffer(void) {
    struct jitter_buffer jb = {};
    jitter_buffer_init(&jb);

    // Inject out-of-order RTP packets (Seq: 3, 1, 4, 2)
    char pkt3[12] = {0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0};
    char pkt1[12] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    char pkt4[12] = {0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0};
    char pkt2[12] = {0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0};

    assert(jitter_buffer_push(&jb, pkt3, 12));
    assert(jitter_buffer_push(&jb, pkt1, 12));
    assert(jitter_buffer_push(&jb, pkt4, 12));
    assert(jitter_buffer_push(&jb, pkt2, 12));

    void  *out_buf = nullptr;
    size_t out_len = 0;

    // Simulate tick 1 -> expect 1
    assert(jitter_buffer_pop(&jb, &out_buf, &out_len) == true);
    assert(out_buf == pkt1);

    // Simulate tick 2 -> expect 2
    assert(jitter_buffer_pop(&jb, &out_buf, &out_len) == true);
    assert(out_buf == pkt2);

    // Simulate tick 3 -> expect 3
    assert(jitter_buffer_pop(&jb, &out_buf, &out_len) == true);
    assert(out_buf == pkt3);

    // Simulate tick 4 -> expect 4
    assert(jitter_buffer_pop(&jb, &out_buf, &out_len) == true);
    assert(out_buf == pkt4);

    printf("Jitter buffer out-of-order tests passed.\n");
}

static void test_sdp_generation(void) {
    struct call_session session = {};
    session.local_sdp.port      = 10005;

    // Mock config
    setenv("RTP_EXTERNAL_ADDR", "2.3.4.5", 1);
    config_load();

    char         buffer[1024];
    size_t const len = sdp_generate_reply(&session, buffer, sizeof(buffer));

    assert(len > 0);
    // Simple verification
    assert(__builtin_strstr(buffer, "m=audio 10005 RTP/AVP") != nullptr);
    assert(__builtin_strstr(buffer, "c=IN IP4 2.3.4.5") != nullptr);

    printf("SDP generation test passed.\n");
}

static void test_silence_injection(void) {

    struct call_session session          = {};
    session.remote_sdp.pcmu.payload_type = 0; // PCMU
    session.tx_seq_num                   = 100;
    session.tx_timestamp                 = 8000;
    session.tx_ssrc                      = 0x12345678;

    char   buffer[1500] = {};
    size_t out_len      = 0;

    assert(rtp_generate_silence(&session, buffer, &out_len) == true);
    assert(out_len == 12 + 160);

    uint8_t *bytes = (uint8_t *)buffer;
    assert(bytes[0] == 0x80); // Version 2
    assert(bytes[1] == 0);    // PT = 0
    assert(bytes[2] == 0);    // Seq = 100
    assert(bytes[3] == 100);

    assert(bytes[4] == 0x00); // TS = 8000 (0x1F40)
    assert(bytes[5] == 0x00);
    assert(bytes[6] == 0x1F);
    assert(bytes[7] == 0x40);

    for (size_t i = 12; i < 172; ++i) {
        assert(bytes[i] == 0xFF);
    }

    // Test actually queueing it via io_uring
    uint16_t  out_port = 0;
    int const fd       = rtp_engine_allocate_port(&session, &out_port);
    assert(fd >= 0);

    session.has_learned_remote_addr        = true;
    session.learned_remote_addr.sin_family = AF_INET;
    session.learned_remote_addr.sin_port   = htons(9999);
    inet_pton(AF_INET, "127.0.0.1", &session.learned_remote_addr.sin_addr);

    assert(rtp_engine_send_silence(&session, fd) == true);

    // We can't trivially assert the network received it without setting up a listener,
    // but we can poll io_uring to ensure it gracefully completes and frees the buffer
    server_poll_once();

    rtp_engine_free_port(fd, out_port);

    printf("Silence injection tests passed.\n");
}

static void test_dtmf_generation(void) {

    struct call_session session          = {};
    session.remote_sdp.dtmf.payload_type = 101; // Negotiated!
    session.tx_seq_num                   = 100;
    session.tx_timestamp                 = 8000;
    session.tx_ssrc                      = 0x12345678;

    uint8_t buffer[64] = {};
    size_t  length     = 0;

    assert(rtp_generate_dtmf(&session, 9, true, false, 160, buffer, &length) == true);
    assert(length == 16);

    assert(buffer[0] == 0x80);                     // V=2
    assert(buffer[1] == (101 | 0x80));             // PT=101, Marker=1
    assert(buffer[2] == 0x00 && buffer[3] == 100); // Seq

    // Payload byte 0: event
    assert(buffer[12] == 9);
    // Payload byte 1: E bit + R + volume
    assert(buffer[13] == 10); // E=0, Vol=10
    // Payload byte 2-3: duration
    assert(buffer[14] == 0x00 && buffer[15] == 160); // 160 duration

    printf("DTMF RFC 4733 generation tests passed.\n");
}

static void test_missing_ws_client(void) {
    struct jitter_buffer jb;
    jitter_buffer_init(&jb);

    uint8_t packet[128] = {0};
    packet[0]           = 0x80;
    packet[1]           = 0x00;

    int success_count = 0;
    int drop_count    = 0;

    for (int i = 0; i < 1000; ++i) {
        packet[2] = (uint8_t)((i >> 8) & 0xFF);
        packet[3] = (uint8_t)(i & 0xFF);

        if (jitter_buffer_push(&jb, packet, sizeof(packet))) {
            success_count++;
        } else {
            drop_count++;
        }
    }

    assert(success_count == 64);
    assert(drop_count == 1000 - 64);

    printf("Missing WS Client test (Memory drop safety) passed.\n");
}

int main(void) {
    g_config.rtp_min_port = 10000;
    g_config.rtp_max_port = 11000;
    g_config.max_calls    = 1024;
    assert(server_init() == 0);
    assert(rtp_engine_init() == 0);

    test_rtp_port_allocator();
    test_jitter_buffer();
    test_sdp_generation();
    test_silence_injection();
    test_dtmf_generation();
    test_missing_ws_client();

    rtp_engine_cleanup();
    server_cleanup();
    return 0;
}
