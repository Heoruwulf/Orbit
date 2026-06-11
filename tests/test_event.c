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
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "orbit/config.h"
#include "orbit/event.h"
#include "orbit/sip_router.h"
#include "orbit/string_view.h"

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

extern _Atomic size_t g_mock_published_count;

static void test_event_config(void) {
    setenv("EVENT_PROVIDER", "mock", 1);
    setenv("EVENT_QUEUE_CAPACITY", "1024", 1);
    setenv("EVENT_REDIS_HOST", "10.0.0.1", 1);
    setenv("EVENT_REDIS_PORT", "16379", 1);
    setenv("EVENT_REDIS_CHANNEL", "test_channel", 1);
    setenv("EVENT_REDIS_USERNAME", "test_user", 1);
    setenv("EVENT_REDIS_PASSWORD", "test_pass", 1);
    setenv("EVENT_REDIS_DATABASE", "2", 1);
    setenv("EVENT_KAFKA_BROKERS", "10.0.0.2:9092", 1);
    setenv("EVENT_KAFKA_TOPIC", "test_topic", 1);

    auto const res = config_load();
    assert(res == 0);
    assert(strcmp(g_config.event_provider, "mock") == 0);
    assert(g_config.event_queue_capacity == 1024);
    assert(strcmp(g_config.event_redis_host, "10.0.0.1") == 0);
    assert(g_config.event_redis_port == 16379);
    assert(strcmp(g_config.event_redis_channel, "test_channel") == 0);
    assert(strcmp(g_config.event_redis_username, "test_user") == 0);
    assert(strcmp(g_config.event_redis_password, "test_pass") == 0);
    assert(g_config.event_redis_db == 2);
    assert(strcmp(g_config.event_kafka_brokers, "10.0.0.2:9092") == 0);
    assert(strcmp(g_config.event_kafka_topic, "test_topic") == 0);

    printf("test_event_config passed.\n");
}

static void test_mock_provider_flow(void) {
    // 1. Prepare configuration
    setenv("EVENT_PROVIDER", "mock", 1);
    setenv("EVENT_QUEUE_CAPACITY", "2048", 1);
    auto const config_res = config_load();
    assert(config_res == 0);

    // 2. Initialize global event system (malloc allowed during boot)
    auto const init_res = event_global_init();
    assert(init_res == 0);

    // Lock memory allocation - from here on, no allocations are allowed
    g_boot_complete = true;

    // 3. Publish mock event
    struct sip_message msg = {
        .call_id  = {.data = "test-call-id-123", .length = 16},
        .from_tag = {.data = "alice-tag", .length = 9},
        .to_tag   = {.data = "bob-tag", .length = 7}};
    struct string_view const internal_id = {.data = "internal-uuid-456", .length = 17};

    event_publish_call_answered(&msg, internal_id);

    // Wait for dispatcher thread to consume the event and invoke mock provider
    int attempts = 0;
    while (atomic_load_explicit(&g_mock_published_count, memory_order_relaxed) < 1 &&
           attempts < 100)
    {
        usleep(1000);
        attempts++;
    }

    assert(atomic_load_explicit(&g_mock_published_count, memory_order_relaxed) == 1);

    // Unlock memory allocations for shutdown
    g_boot_complete = false;

    // 4. Clean up global event system
    event_global_cleanup();

    printf("test_mock_provider_flow passed.\n");
}

int main(void) {
    test_event_config();
    test_mock_provider_flow();
    printf("All test_event tests passed!\n");
    return 0;
}
