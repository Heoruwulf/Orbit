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
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include "orbit/config.h"
#include "orbit/memory.h"
#include "orbit/server.h"
#include "orbit/sip_router.h"

static_assert(offsetof(struct call_session, rtp_fd) < 64, "Hot fields must be in L1 cache line 0");

extern void *__real_malloc(size_t size);
extern void  __real_free(void *ptr);
extern void *__real_calloc(size_t nmemb, size_t size);
extern void *__real_realloc(void *ptr, size_t size);

bool g_boot_complete = false;

void *__wrap_malloc(size_t const size) {
    if (g_boot_complete) {
        fprintf(stderr, "FATAL: malloc called after boot complete!\n");
        abort();
    }
    return __real_malloc(size);
}

void __wrap_free(void *const ptr) {
    if (g_boot_complete) {
        fprintf(stderr, "FATAL: free called after boot complete!\n");
        abort();
    }
    __real_free(ptr);
}

void *__wrap_calloc(size_t const nmemb, size_t const size) {
    if (g_boot_complete) {
        fprintf(stderr, "FATAL: calloc called after boot complete!\n");
        abort();
    }
    return __real_calloc(nmemb, size);
}

void *__wrap_realloc(void *const ptr, size_t const size) {
    if (g_boot_complete) {
        fprintf(stderr, "FATAL: realloc called after boot complete!\n");
        abort();
    }
    return __real_realloc(ptr, size);
}

struct call_context {
    int  id;
    char data[128];
};

struct rtp_packet_node {
    int  seq;
    char payload[160];
};

static void test_config(void) {
    unsetenv("SIP_LISTEN_ADDR");
    unsetenv("SIP_EXTERNAL_ADDR");
    unsetenv("RTP_EXTERNAL_ADDR");
    unsetenv("RTP_MIN_PORT");
    unsetenv("RTP_MAX_PORT");
    unsetenv("WS_LISTEN_PORT");
    unsetenv("WS_EXTERNAL_PORT");

    auto res = config_load();
    assert(res == 0 && "Config should succeed with defaults");
    assert(g_config.rtp_min_port == 16000);
    assert(g_config.rtp_max_port == 32000);
    assert(g_config.ws_listen_port == 8080);
    assert(g_config.ws_external_port == 8080);

    setenv("SIP_LISTEN_ADDR", "0.0.0.0", 1);
    setenv("SIP_EXTERNAL_ADDR", "1.2.3.4", 1);
    setenv("RTP_EXTERNAL_ADDR", "1.2.3.4", 1);
    setenv("RTP_MIN_PORT", "10000", 1);
    setenv("RTP_MAX_PORT", "20000", 1);
    setenv("WS_LISTEN_PORT", "9000", 1);

    res = config_load();
    assert(res == 0 && "Config should succeed with env vars");

    assert(g_config.rtp_min_port == 10000);
    assert(g_config.rtp_max_port == 20000);
    assert(g_config.ws_listen_port == 9000);
    assert(g_config.ws_external_port == 9000);

    setenv("WS_EXTERNAL_PORT", "9500", 1);
    res = config_load();
    assert(res == 0 && "Config should succeed with explicit WS_EXTERNAL_PORT");
    assert(g_config.ws_listen_port == 9000);
    assert(g_config.ws_external_port == 9500);

    unsetenv("WS_LISTEN_PORT");
    unsetenv("WS_EXTERNAL_PORT");

    printf("Config test passed.\n");
}

static void test_memory_pool(void) {
    constexpr size_t POOL_SIZE = 10000;
    auto const       call_pool = pool_create(
        (struct pool_config){.object_size = sizeof(struct call_context), .count = POOL_SIZE});
    auto const rtp_pool = pool_create(
        (struct pool_config){.object_size = sizeof(struct rtp_packet_node), .count = POOL_SIZE});
    assert(call_pool != nullptr);
    assert(rtp_pool != nullptr);

    g_boot_complete = true;

    struct call_context *calls[POOL_SIZE];
    __builtin_memset(calls, 0, sizeof(calls));

    struct rusage usage_before = {};
    getrusage(RUSAGE_SELF, &usage_before);
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        calls[i] = (struct call_context *)pool_alloc(call_pool);
        assert(calls[i] != nullptr);
        assert(((uintptr_t)calls[i] % 64) == 0);
        calls[i]->id = (int)i;
    }

    struct rusage usage_after = {};
    getrusage(RUSAGE_SELF, &usage_after);
    assert(usage_after.ru_minflt - usage_before.ru_minflt == 0);

    assert(pool_alloc(call_pool) == nullptr);

    for (size_t i = 0; i < POOL_SIZE; ++i) {
        pool_free(call_pool, calls[i]);
    }

    auto const c = (struct call_context *)pool_alloc(call_pool);
    assert(c != nullptr);
    pool_free(call_pool, c);

    g_boot_complete = false;

    pool_destroy(call_pool);
    pool_destroy(rtp_pool);
    printf("Memory pool test passed.\n");
}

struct thread_arg {
    int event_fd;
};

static void *server_thread(void *const arg) {
    auto const targ = (struct thread_arg const *)arg;
    assert(server_init() == 0);
    assert(sip_router_init() == 0);
    g_boot_complete = true;
    server_run(targ->event_fd);
    g_boot_complete = false;
    sip_router_cleanup();
    server_cleanup();
    return nullptr;
}

static void test_graceful_shutdown_waits_for_calls(void) {
    g_config.max_calls = 10;

    auto const event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(event_fd >= 0);

    struct thread_arg targ = {.event_fd = event_fd};
    pthread_t         tid  = {};
    pthread_create(&tid, nullptr, server_thread, &targ);

    usleep(100000);

    // Inject a call via UDP INVITE
    auto const sock = socket(AF_INET, SOCK_DGRAM, 0);
    assert(sock >= 0);
    struct sockaddr_in addr = {};
    addr.sin_family         = AF_INET;
    addr.sin_port           = htons(5060);
    addr.sin_addr.s_addr    = inet_addr("127.0.0.1");

    char const invite_str[] = "INVITE sip:alice@domain.com SIP/2.0\r\n"
                              "Via: SIP/2.0/UDP 127.0.0.1:5060\r\n"
                              "From: <sip:bob@domain.com>\r\n"
                              "To: <sip:alice@domain.com>\r\n"
                              "Call-ID: graceful-test\r\n"
                              "CSeq: 1 INVITE\r\n\r\n";
    sendto(
        sock,
        invite_str,
        sizeof(invite_str) - 1,
        0,
        (struct sockaddr const *)&addr,
        sizeof(addr));

    usleep(100000);

    // Send draining signal to the worker via eventfd
    uint64_t const stop_val = 1;
    write(event_fd, &stop_val, sizeof(stop_val));

    usleep(100000);

    // Send BYE via UDP to terminate the call
    char const bye_str[] = "BYE sip:alice@domain.com SIP/2.0\r\n"
                           "Call-ID: graceful-test\r\n"
                           "CSeq: 2 BYE\r\n\r\n";
    sendto(sock, bye_str, sizeof(bye_str) - 1, 0, (struct sockaddr const *)&addr, sizeof(addr));
    close(sock);

    pthread_join(tid, nullptr);
    close(event_fd);
    printf("Graceful shutdown wait test passed.\n");
}

int main(void) {
    test_config();
    test_memory_pool();
    test_graceful_shutdown_waits_for_calls();
    printf("All test_core passed!\n");
    return 0;
}
