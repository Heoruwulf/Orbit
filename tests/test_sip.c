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
#include <stdlib.h>
#include <string.h>
#include "orbit/server.h"
#include "orbit/sip_router.h"

extern bool __real_server_is_draining(void);
static bool g_mock_draining = false;

bool __wrap_server_is_draining(void) { return g_mock_draining; }

static void test_sip_verb_parser(void) {
    assert(sip_parse_verb(SV("INVITE sip:bob@example.com")) == SIP_VERB_INVITE);
    assert(sip_parse_verb(SV("ACK sip:bob@example.com")) == SIP_VERB_ACK);
    assert(sip_parse_verb(SV("CANCEL sip:bob@example.com")) == SIP_VERB_CANCEL);
    assert(sip_parse_verb(SV("BYE sip:bob@example.com")) == SIP_VERB_BYE);
    assert(sip_parse_verb(SV("OPTIONS sip:bob@example.com")) == SIP_VERB_OPTIONS);
    assert(sip_parse_verb(SV("SIP/2.0 200 OK")) == SIP_VERB_RESPONSE);
    assert(sip_parse_verb(SV("SIP/2.0 404 Not Found")) == SIP_VERB_RESPONSE);
    assert(sip_parse_verb(SV("INVALID sip:bob@example.com")) == SIP_VERB_UNKNOWN);
    printf("SIP verb parser tests passed.\n");
}

static void test_sip_message_parser(void) {
    char const raw_msg[] = "INVITE sip:bob@biloxi.com SIP/2.0\r\n"
                           "Via: SIP/2.0/UDP pc33.atlanta.com;branch=z9hG4bK776asdhds\r\n"
                           "Via: SIP/2.0/UDP 192.168.1.100:5060;branch=z9hG4bK776asdhds\r\n"
                           "Max-Forwards: 70\r\n"
                           "To: Bob <sip:bob@biloxi.com>\r\n"
                           "From: Alice <sip:alice@atlanta.com>;tag=1928301774\r\n"
                           "Call-ID: a84b4c76e66710@pc33.atlanta.com\r\n"
                           "CSeq: 314159 INVITE\r\n"
                           "Contact: <sip:alice@pc33.atlanta.com>\r\n"
                           "Content-Type: application/sdp\r\n"
                           "Content-Length: 142\r\n"
                           "\r\n"
                           "v=0\r\n"
                           "o=alice 2890844526 2890844526 IN IP4 host.anywhere.com\r\n";

    struct sip_message msg = {};
    // Ensure we don't include the null terminator in length
    bool const success = sip_parse_message(SV(raw_msg), &msg);
    assert(success);
    assert(msg.verb == SIP_VERB_INVITE);
    assert(sv_equals(msg.call_id, "a84b4c76e66710@pc33.atlanta.com"));
    assert(sv_equals(msg.from_tag, "Alice <sip:alice@atlanta.com>;tag=1928301774"));
    assert(sv_equals(msg.to_tag, "Bob <sip:bob@biloxi.com>"));
    assert(sv_equals(msg.cseq, "314159 INVITE"));
    assert(
        sv_equals(msg.body, "v=0\r\no=alice 2890844526 2890844526 IN IP4 host.anywhere.com\r\n"));
    printf("SIP message parser tests passed.\n");
}

static void test_sdp_parser(void) {
    char const sdp_body[] = "v=0\r\n"
                            "o=alice 2890844526 2890844526 IN IP4 host.anywhere.com\r\n"
                            "c=IN IP4 192.168.1.100\r\n"
                            "t=0 0\r\n"
                            "m=audio 49170 RTP/AVP 0 8 111 96\r\n"
                            "a=rtpmap:0 PCMU/8000\r\n"
                            "a=rtpmap:8 PCMA/8000\r\n"
                            "a=rtpmap:111 OPUS/48000/2\r\n"
                            "a=rtpmap:96 L16/16000/1\r\n"
                            "a=ptime:20\r\n";

    struct sdp_media_info info    = {};
    bool const            success = sdp_parse(SV(sdp_body), &info);
    assert(success);
    assert(sv_equals(info.ip_addr, "192.168.1.100"));
    assert(info.port == 49170);
    assert(info.ptime == 20);

    assert(info.pcmu.payload_type == 0);
    assert(info.pcmu.sample_rate == 8000);
    assert(info.pcmu.channels == 1);

    assert(info.pcma.payload_type == 8);
    assert(info.pcma.sample_rate == 8000);
    assert(info.pcma.channels == 1);

    assert(info.opus.payload_type == 111);
    assert(info.opus.sample_rate == 48000);
    assert(info.opus.channels == 2);

    assert(info.l16.payload_type == 96);
    assert(info.l16.sample_rate == 16000);
    assert(info.l16.channels == 1);
    printf("SDP parser tests passed.\n");
}

static void test_sip_router(void) {
    assert(sip_router_init() == 0);

    struct sip_message invite = {
        .verb    = SIP_VERB_INVITE,
        .call_id = SV("12345@domain.com"),
        .via     = SV("SIP/2.0/UDP 192.168.1.100:5060;branch=z9hG4bK776asdhds"),
        .body    = SV("v=0\r\no=alice 2890844526 2890844526 IN IP4 host\r\nc=IN IP4 1.2.3.4\r\nt=0 "
                      "0\r\nm=audio 49170 RTP/AVP 0\r\n")};

    auto session = sip_router_process(&invite);
    assert(session != nullptr);
    assert(session->is_active);
    assert(sv_equals(invite.call_id, "12345@domain.com"));
    assert(sv_equals(invite.via, "SIP/2.0/UDP 192.168.1.100:5060;branch=z9hG4bK776asdhds"));
    assert(invite.body.length > 0);
    assert(session->remote_sdp.port == 49170);

    struct sip_message ack = {.verb = SIP_VERB_ACK, .call_id = SV("12345@domain.com")};

    auto ack_session = sip_router_process(&ack);
    assert(ack_session == session);

    struct sip_message options     = {.verb = SIP_VERB_OPTIONS, .call_id = SV("ping@domain.com")};
    auto               opt_session = sip_router_process(&options);
    assert(opt_session == nullptr); // OPTIONS doesn't create sessions

    struct sip_message bye         = {.verb = SIP_VERB_BYE, .call_id = SV("12345@domain.com")};
    auto               bye_session = sip_router_process(&bye);
    assert(bye_session == nullptr); // Router returns null on BYE and destroys session

    // Ensure session is gone
    auto const check = sip_router_process(&ack);
    assert(check == nullptr);

    printf("SIP router lifecycle tests passed.\n");
}

static void test_memory_exhaustion(void) {
    struct call_session *sessions[1024] = {0};
    int                  count          = 0;

    static char call_ids[1024]
                        [32]; // Use static 2D array so pointers are unique and outlive the loop

    for (int i = 0; i < 1024; ++i) {
        snprintf(call_ids[i], sizeof(call_ids[i]), "exhaust-%d", i);

        struct sip_message inv = {};
        inv.verb               = SIP_VERB_INVITE;
        inv.call_id.data       = call_ids[i];
        inv.call_id.length     = __builtin_strlen(call_ids[i]);

        sessions[count] = sip_router_process(&inv);
        if (sessions[count] == nullptr) {
            break;
        }
        count++;
    }

    // Ensure we actually filled up the pool (some might be taken by previous tests)
    assert(count > 1000);

    // Simulate one more call to ensure it fails
    struct sip_message inv_overload = {};
    inv_overload.verb               = SIP_VERB_INVITE;
    inv_overload.call_id.data       = "overload-1025";
    inv_overload.call_id.length     = 13;

    struct call_session *failed_session = sip_router_process(&inv_overload);
    assert(failed_session == nullptr); // Successfully rejected!

    // Clean up
    for (int i = 0; i < count; ++i) {
        struct sip_message bye = {};
        bye.verb               = SIP_VERB_BYE;
        bye.call_id.data       = call_ids[i];
        bye.call_id.length     = __builtin_strlen(call_ids[i]);
        sip_router_process(&bye);
    }

    printf("SIP memory exhaustion test passed.\n");
}

#include "orbit/config.h"

static void test_sip_response(void) {
    char const raw[] = "INVITE sip:bob@domain.com SIP/2.0\r\n"
                       "Via: SIP/2.0/UDP 192.168.1.100:5060\r\n"
                       "From: <sip:alice@domain.com>\r\n"
                       "To: <sip:bob@domain.com>\r\n"
                       "Call-ID: abcdef12345\r\n"
                       "CSeq: 1 INVITE\r\n\r\n";

    struct sip_message msg = {};
    assert(sip_parse_message(SV(raw), &msg));

    setenv("SIP_EXTERNAL_ADDR", "5.6.7.8", 1);
    config_load();

    char         buffer[1024];
    size_t const len = sip_generate_response(
        &msg,
        200,
        "OK",
        SV("v=0\r\nm=audio 4000 RTP/AVP 0\r\n"),
        buffer,
        sizeof(buffer));
    assert(len > 0);

    assert(__builtin_strstr(buffer, "SIP/2.0 200 OK") != nullptr);
    assert(__builtin_strstr(buffer, "Via: SIP/2.0/UDP 192.168.1.100:5060") != nullptr);
    assert(__builtin_strstr(buffer, "To: <sip:bob@domain.com>;tag=wsrtp-1234") != nullptr);
    assert(__builtin_strstr(buffer, "Contact: <sip:orbit@5.6.7.8>") != nullptr);
    assert(__builtin_strstr(buffer, "Content-Type: application/sdp") != nullptr);
    assert(__builtin_strstr(buffer, "v=0\r\nm=audio 4000 RTP/AVP 0") != nullptr);

    // Test a stateless 200 OK for OPTIONS with no body
    char const raw_opt[] = "OPTIONS sip:bob@domain.com SIP/2.0\r\n"
                           "Via: SIP/2.0/UDP 192.168.1.100:5060\r\n"
                           "From: <sip:alice@domain.com>\r\n"
                           "To: <sip:bob@domain.com>\r\n"
                           "Call-ID: opt123\r\n"
                           "CSeq: 2 OPTIONS\r\n\r\n";

    struct sip_message msg_opt = {};
    assert(sip_parse_message(SV(raw_opt), &msg_opt));

    char         caps_buf[512];
    size_t const caps_len = sdp_generate_capabilities_reply(caps_buf, sizeof(caps_buf));
    assert(caps_len > 0);

    size_t const len_opt = sip_generate_response(
        &msg_opt,
        200,
        "OK",
        SV_LEN(caps_buf, caps_len),
        buffer,
        sizeof(buffer));
    assert(len_opt > 0);
    assert(__builtin_strstr(buffer, "SIP/2.0 200 OK") != nullptr);
    assert(__builtin_strstr(buffer, "Call-ID: opt123") != nullptr);
    assert(__builtin_strstr(buffer, "Allow: INVITE, ACK, CANCEL, OPTIONS, BYE") != nullptr);
    assert(__builtin_strstr(buffer, "Accept: application/sdp") != nullptr);
    assert(__builtin_strstr(buffer, "Content-Type: application/sdp") != nullptr);
    assert(__builtin_strstr(buffer, "m=audio 0 RTP/AVP") != nullptr);

    printf("SIP response generator tests passed.\n");
}

static void test_sip_draining_refusal(void) {
    g_mock_draining = true;

    char const raw[] = "INVITE sip:alice@domain.com SIP/2.0\r\n"
                       "Via: SIP/2.0/UDP 192.168.1.100:5060\r\n"
                       "From: <sip:bob@domain.com>\r\n"
                       "To: <sip:alice@domain.com>\r\n"
                       "Call-ID: draining-test\r\n"
                       "CSeq: 1 INVITE\r\n\r\n";

    struct io_event_ctx ctx = {};
    ctx.buffer              = (void *)raw;

    size_t before = sip_router_active_call_count();

    sip_router_process_recv(&ctx, sizeof(raw) - 1);

    assert(sip_router_active_call_count() == before);

    g_mock_draining = false;
    printf("SIP draining refusal test passed.\n");
}

static void test_sip_message_whitespace_parsing(void) {
    char const raw_msg[] = "INVITE sip:bob@biloxi.com SIP/2.0\r\n"
                           "Via:  SIP/2.0/UDP pc33.atlanta.com\r\n"
                           "To: \tBob <sip:bob@biloxi.com>\r\n"
                           "From:   Alice <sip:alice@atlanta.com>;tag=1928301774\r\n"
                           "Call-ID:\ta84b4c76e66710\r\n"
                           "CSeq:\t 314159 INVITE\r\n"
                           "X-Custom-Header: \t Value123 \r\n"
                           "\r\n";

    struct sip_message msg     = {};
    bool const         success = sip_parse_message(SV(raw_msg), &msg);
    assert(success);
    assert(msg.verb == SIP_VERB_INVITE);
    assert(sv_equals(msg.call_id, "a84b4c76e66710"));
    assert(sv_equals(msg.from_tag, "Alice <sip:alice@atlanta.com>;tag=1928301774"));
    assert(sv_equals(msg.to_tag, "Bob <sip:bob@biloxi.com>"));
    assert(sv_equals(msg.cseq, "314159 INVITE"));
    assert(sv_equals(msg.via, "SIP/2.0/UDP pc33.atlanta.com"));
    assert(msg.custom_header_count == 1);
    assert(sv_equals(msg.custom_headers[0].name, "X-Custom-Header"));
    assert(sv_equals(msg.custom_headers[0].value, "Value123 "));
    printf("SIP message whitespace parsing tests passed.\n");
}

int main(void) {
    test_sip_verb_parser();
    test_sip_message_parser();
    test_sip_message_whitespace_parsing();
    test_sdp_parser();

    g_config.max_calls = 1024;
    server_init(); // Required before sip_router_init can submit io_uring events

    test_sip_router();
    test_memory_exhaustion();
    test_sip_response();
    test_sip_draining_refusal();
    sip_router_cleanup();
    server_cleanup();
    return 0;
}
