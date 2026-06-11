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
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <threads.h>
#include <unistd.h>
#include "orbit/config.h"
#include "orbit/log.h"
#include "orbit/macros.h"
#include "orbit/thread.h"
#include "yyjson.h"

#ifdef ORBIT_WITH_REDIS
#include <hiredis/hiredis.h>
#include <sys/time.h>
#endif

#ifdef ORBIT_WITH_KAFKA
#include <librdkafka/rdkafka.h>
#endif

constexpr size_t EVENT_MAX_PAYLOAD = 4096;

/**
 * @brief Thread-safe event payload storage node.
 */
struct event_node {
    alignas(64) char payload[EVENT_MAX_PAYLOAD];
    size_t length;
};

/**
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) Ring Buffer.
 */
struct event_spsc_queue {
    struct event_node *ring;
    size_t             capacity;
    alignas(64) _Atomic size_t head;
    alignas(64) _Atomic size_t tail;
};

// Global variables for background dispatch architecture
static struct event_spsc_queue     *g_event_queues        = nullptr;
static int                          g_num_queues          = 0;
static int                          g_event_dispatcher_fd = -1;
static pthread_t                    g_dispatcher_thread   = {};
static _Atomic bool                 g_dispatcher_running  = false;
static struct event_provider const *g_event_provider      = nullptr;

// Thread-local variables for backwards-compatible/direct UDP mode
static thread_local int                event_fd   = -1;
static thread_local struct sockaddr_in event_addr = {};

// Forward declarations of providers
#ifdef ORBIT_WITH_UDP
extern struct event_provider const g_event_provider_udp;
#endif
#ifdef ORBIT_WITH_REDIS
extern struct event_provider const g_event_provider_redis;
#endif
#ifdef ORBIT_WITH_KAFKA
extern struct event_provider const g_event_provider_kafka;
#endif
extern struct event_provider const g_event_provider_mock;

// Find provider by name
static struct event_provider const *event_provider_find(char const *const name) {
    if (name == nullptr) {
#ifdef ORBIT_WITH_UDP
        return &g_event_provider_udp;
#else
        return nullptr;
#endif
    }
    if (strcmp(name, "mock") == 0) {
        return &g_event_provider_mock;
    }
#ifdef ORBIT_WITH_UDP
    if (strcmp(name, "udp") == 0) {
        return &g_event_provider_udp;
    }
#endif
#ifdef ORBIT_WITH_REDIS
    if (strcmp(name, "redis") == 0) {
        return &g_event_provider_redis;
    }
#endif
#ifdef ORBIT_WITH_KAFKA
    if (strcmp(name, "kafka") == 0) {
        return &g_event_provider_kafka;
    }
#endif
    return nullptr;
}

// Lock-Free SPSC Queue Operations
static bool event_queue_push(
    struct event_spsc_queue *const restrict q,
    char const *restrict const payload,
    size_t const len) {

    if (unlikely(q == nullptr || payload == nullptr || len >= EVENT_MAX_PAYLOAD)) {
        return false;
    }

    auto const current_tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    auto const current_head = atomic_load_explicit(&q->head, memory_order_acquire);

    if (current_tail - current_head >= q->capacity) {
        return false; // Queue full
    }

    auto const index = current_tail % q->capacity;
    auto const node  = &q->ring[index];

    for (size_t i = 0; i < len; ++i) {
        node->payload[i] = payload[i];
    }
    node->payload[len] = '\0';
    node->length       = len;

    atomic_store_explicit(&q->tail, current_tail + 1, memory_order_release);
    return true;
}

// Dispatcher thread loop
static void *event_dispatcher_loop(void *const arg) {
    (void)arg;

    while (atomic_load_explicit(&g_dispatcher_running, memory_order_acquire)) {
        uint64_t      val = 0;
        ssize_t const r   = read(g_event_dispatcher_fd, &val, sizeof(val));
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
            continue;
        }

        bool drained_any = false;
        do {
            drained_any = false;
            for (int i = 0; i < g_num_queues; ++i) {
                struct event_spsc_queue *q = &g_event_queues[i];
                auto const current_head    = atomic_load_explicit(&q->head, memory_order_relaxed);
                auto const current_tail    = atomic_load_explicit(&q->tail, memory_order_acquire);

                if (current_head < current_tail) {
                    auto const               index = current_head % q->capacity;
                    struct event_node const *node  = &q->ring[index];

                    if (g_event_provider != nullptr && g_event_provider->publish != nullptr) {
                        g_event_provider->publish(node->payload, node->length);
                    }

                    atomic_store_explicit(&q->head, current_head + 1, memory_order_release);
                    drained_any = true;
                }
            }
        } while (drained_any && atomic_load_explicit(&g_dispatcher_running, memory_order_acquire));
    }

    // Final drain of all remaining elements in queues
    for (int i = 0; i < g_num_queues; ++i) {
        struct event_spsc_queue *q = &g_event_queues[i];
        while (true) {
            auto const current_head = atomic_load_explicit(&q->head, memory_order_relaxed);
            auto const current_tail = atomic_load_explicit(&q->tail, memory_order_acquire);
            if (current_head >= current_tail) {
                break;
            }
            auto const               index = current_head % q->capacity;
            struct event_node const *node  = &q->ring[index];
            if (g_event_provider != nullptr && g_event_provider->publish != nullptr) {
                g_event_provider->publish(node->payload, node->length);
            }
            atomic_store_explicit(&q->head, current_head + 1, memory_order_release);
        }
    }

    return nullptr;
}

// Global Lifecycle Functions
int event_global_init(void) {
    // Log build-time configuration of event providers
#ifdef ORBIT_WITH_UDP
    LOGINF("Event Build Option: UDP provider [ENABLED]");
#else
    LOGINF("Event Build Option: UDP provider [DISABLED]");
#endif

#ifdef ORBIT_WITH_REDIS
    LOGINF("Event Build Option: Redis provider [ENABLED]");
#else
    LOGINF("Event Build Option: Redis provider [DISABLED]");
#endif

#ifdef ORBIT_WITH_KAFKA
    LOGINF("Event Build Option: Kafka provider [ENABLED]");
#else
    LOGINF("Event Build Option: Kafka provider [DISABLED]");
#endif

    auto const provider_name = g_config.event_provider;
    if (provider_name == nullptr || strcmp(provider_name, "disabled") == 0) {
        return 0; // Disabled
    }

    g_event_provider = event_provider_find(provider_name);
    if (g_event_provider == nullptr) {
        LOGERR("Event System: Unknown or disabled provider '%s'", provider_name);
        return -1;
    }

    if (g_event_provider->init() < 0) {
        LOGERR("Event System: Failed to initialize provider '%s'", provider_name);
        return -1;
    }

    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs < 1) {
        nprocs = 1;
    }
    g_num_queues = (int)nprocs;

    g_event_queues = calloc((size_t)g_num_queues, sizeof(struct event_spsc_queue));
    if (g_event_queues == nullptr) {
        return -1;
    }

    for (int i = 0; i < g_num_queues; ++i) {
        g_event_queues[i].capacity = g_config.event_queue_capacity;
        g_event_queues[i].ring     = calloc(g_event_queues[i].capacity, sizeof(struct event_node));
        if (g_event_queues[i].ring == nullptr) {
            return -1;
        }
        atomic_init(&g_event_queues[i].head, 0);
        atomic_init(&g_event_queues[i].tail, 0);
    }

    g_event_dispatcher_fd = eventfd(0, EFD_CLOEXEC);
    if (g_event_dispatcher_fd < 0) {
        return -1;
    }

    atomic_store_explicit(&g_dispatcher_running, true, memory_order_release);
    if (pthread_create(&g_dispatcher_thread, nullptr, event_dispatcher_loop, nullptr) != 0) {
        atomic_store_explicit(&g_dispatcher_running, false, memory_order_relaxed);
        close(g_event_dispatcher_fd);
        g_event_dispatcher_fd = -1;
        return -1;
    }

    LOGINF(
        "Event System: Initialized globally with provider '%s' and %d queues",
        provider_name,
        g_num_queues);
    return 0;
}

void event_global_cleanup(void) {
    if (!atomic_load_explicit(&g_dispatcher_running, memory_order_relaxed)) {
        return;
    }

    atomic_store_explicit(&g_dispatcher_running, false, memory_order_release);

    // Wake up dispatcher thread
    uint64_t const val = 1;
    ssize_t const  w   = write(g_event_dispatcher_fd, &val, sizeof(val));
    (void)w;

    pthread_join(g_dispatcher_thread, nullptr);

    if (g_event_dispatcher_fd >= 0) {
        close(g_event_dispatcher_fd);
        g_event_dispatcher_fd = -1;
    }

    if (g_event_queues != nullptr) {
        for (int i = 0; i < g_num_queues; ++i) {
            free(g_event_queues[i].ring);
        }
        free(g_event_queues);
        g_event_queues = nullptr;
    }
    g_num_queues = 0;

    if (g_event_provider != nullptr && g_event_provider->cleanup != nullptr) {
        g_event_provider->cleanup();
    }
    g_event_provider = nullptr;

    LOGINF("Event System: Cleaned up globally");
}

// Local thread init (for backwards-compatible direct mode)
void event_init(void) {
#ifdef ORBIT_WITH_UDP
    if (g_config.event_udp_listen_addr == nullptr || g_config.event_udp_listen_port == 0) {
        return;
    }

    event_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (event_fd < 0) {
        LOGERR("Failed to create UDP event socket");
        return;
    }

    event_addr.sin_family = AF_INET;
    event_addr.sin_port   = htons(g_config.event_udp_listen_port);
    inet_pton(AF_INET, g_config.event_udp_listen_addr, &event_addr.sin_addr);
#endif
}

// Publish event
void event_publish_call_answered(
    struct sip_message const *restrict const msg,
    struct string_view const internal_id) {

    if (msg == nullptr) {
        return;
    }

    if (g_event_queues == nullptr && event_fd < 0) {
        return;
    }

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
        g_config.ws_external_port,
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

    if (json_len > 1400) {
        LOGWRN(
            "Event JSON is large (%zu bytes) for call-id %.*s, consider reducing the amount of "
            "data included",
            json_len,
            (int)msg->call_id.length,
            msg->call_id.data);
    }

    LOGDBG("Serialized event JSON: %.*s", (int)json_len, json_str);

    if (g_event_queues != nullptr) {
        int worker_id = worker_get_id();
        if (worker_id < 0 || worker_id >= g_num_queues) {
            worker_id = 0;
        }
        auto const q = &g_event_queues[worker_id];
        if (unlikely(!event_queue_push(q, json_str, json_len))) {
            LOGWRN("Event System: Queue %d full, dropping event", worker_id);
        } else {
            uint64_t const val = 1;
            ssize_t const  w   = write(g_event_dispatcher_fd, &val, sizeof(val));
            (void)w;
        }
    } else if (event_fd >= 0) {
#ifdef ORBIT_WITH_UDP
        ssize_t const ret = sendto(
            event_fd,
            json_str,
            json_len,
            0,
            (struct sockaddr const *)&event_addr,
            sizeof(event_addr));
        if (ret < 0) {
            LOGERR("Failed to send UDP event: %s", strerror(errno));
        } else {
            LOGINF("Sent UDP event: %.*s", (int)json_len, json_str);
        }
#else
        (void)event_fd;
#endif
    }
}

// UDP Provider Implementation
#ifdef ORBIT_WITH_UDP
static int                udp_provider_fd   = -1;
static struct sockaddr_in udp_provider_addr = {};

static int udp_provider_init(void) {
    if (g_config.event_udp_listen_addr == nullptr || g_config.event_udp_listen_port == 0) {
        return -1;
    }
    udp_provider_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (udp_provider_fd < 0) {
        return -1;
    }
    udp_provider_addr.sin_family = AF_INET;
    udp_provider_addr.sin_port   = htons(g_config.event_udp_listen_port);
    inet_pton(AF_INET, g_config.event_udp_listen_addr, &udp_provider_addr.sin_addr);
    return 0;
}

static int udp_provider_publish(char const *restrict const payload, size_t const len) {
    if (udp_provider_fd < 0) {
        return -1;
    }
    ssize_t const ret = sendto(
        udp_provider_fd,
        payload,
        len,
        0,
        (struct sockaddr const *)&udp_provider_addr,
        sizeof(udp_provider_addr));
    if (ret < 0) {
        LOGERR("UDP Provider: Failed to send UDP event: %s", strerror(errno));
        return -1;
    }
    LOGINF("UDP Provider: Sent event: %.*s", (int)len, payload);
    return 0;
}

static void udp_provider_cleanup(void) {
    if (udp_provider_fd >= 0) {
        close(udp_provider_fd);
        udp_provider_fd = -1;
    }
}

struct event_provider const g_event_provider_udp = {
    .name    = "udp",
    .init    = udp_provider_init,
    .publish = udp_provider_publish,
    .cleanup = udp_provider_cleanup};
#endif

_Atomic size_t g_mock_published_count = 0;

// Mock Provider Implementation
static int mock_provider_init(void) {
    LOGINF("Mock Provider: Initialized");
    atomic_store_explicit(&g_mock_published_count, 0, memory_order_relaxed);
    return 0;
}

static int mock_provider_publish(char const *restrict const payload, size_t const len) {
    LOGINF("Mock Provider: Published event (len=%zu): %s", len, payload);
    atomic_fetch_add_explicit(&g_mock_published_count, 1, memory_order_relaxed);
    return 0;
}

static void mock_provider_cleanup(void) { LOGINF("Mock Provider: Cleaned up"); }

struct event_provider const g_event_provider_mock = {
    .name    = "mock",
    .init    = mock_provider_init,
    .publish = mock_provider_publish,
    .cleanup = mock_provider_cleanup};

#ifdef ORBIT_WITH_REDIS
static redisContext *g_redis_ctx = nullptr;

static int redis_reconnect(void) {
    if (g_redis_ctx != nullptr) {
        redisFree(g_redis_ctx);
        g_redis_ctx = nullptr;
    }

    struct timeval const timeout = {.tv_sec = 1, .tv_usec = 500000};
    g_redis_ctx =
        redisConnectWithTimeout(g_config.event_redis_host, g_config.event_redis_port, timeout);
    if (unlikely(g_redis_ctx == nullptr || g_redis_ctx->err != 0)) {
        LOGERR(
            "Redis Provider: Connection failed: %s",
            g_redis_ctx ? g_redis_ctx->errstr : "Allocation failed");
        if (g_redis_ctx != nullptr) {
            redisFree(g_redis_ctx);
            g_redis_ctx = nullptr;
        }
        return -1;
    }

    // Authenticate if password is provided
    if (g_config.event_redis_password != nullptr) {
        redisReply *reply = nullptr;
        if (g_config.event_redis_username != nullptr) {
            reply = redisCommand(
                g_redis_ctx,
                "AUTH %s %s",
                g_config.event_redis_username,
                g_config.event_redis_password);
        } else {
            reply = redisCommand(g_redis_ctx, "AUTH %s", g_config.event_redis_password);
        }

        if (unlikely(reply == nullptr || reply->type == REDIS_REPLY_ERROR)) {
            LOGERR("Redis Provider: Authentication failed: %s", reply ? reply->str : "No reply");
            if (reply != nullptr) {
                freeReplyObject(reply);
            }
            redisFree(g_redis_ctx);
            g_redis_ctx = nullptr;
            return -1;
        }
        freeReplyObject(reply);
    }

    // Select database if DB is positive
    if (g_config.event_redis_db > 0) {
        redisReply *reply = redisCommand(g_redis_ctx, "SELECT %d", g_config.event_redis_db);
        if (unlikely(reply == nullptr || reply->type == REDIS_REPLY_ERROR)) {
            LOGERR(
                "Redis Provider: Selecting database %d failed: %s",
                g_config.event_redis_db,
                reply ? reply->str : "No reply");
            if (reply != nullptr) {
                freeReplyObject(reply);
            }
            redisFree(g_redis_ctx);
            g_redis_ctx = nullptr;
            return -1;
        }
        freeReplyObject(reply);
    }

    return 0;
}

static int redis_provider_init(void) {
    if (redis_reconnect() < 0) {
        return -1;
    }
    LOGINF(
        "Redis Provider: Initialized and connected to %s:%d",
        g_config.event_redis_host,
        g_config.event_redis_port);
    return 0;
}

static int redis_provider_publish(char const *restrict const payload, size_t const len) {
    if (unlikely(g_redis_ctx == nullptr)) {
        if (redis_reconnect() < 0) {
            return -1;
        }
    }

    redisReply *reply =
        redisCommand(g_redis_ctx, "PUBLISH %s %b", g_config.event_redis_channel, payload, len);
    if (unlikely(reply == nullptr || g_redis_ctx->err != 0)) {
        LOGWRN("Redis Provider: Publish failed, attempting reconnect...");
        if (reply != nullptr) {
            freeReplyObject(reply);
            reply = nullptr;
        }
        if (redis_reconnect() == 0) {
            reply = redisCommand(
                g_redis_ctx,
                "PUBLISH %s %b",
                g_config.event_redis_channel,
                payload,
                len);
        }
    }

    if (unlikely(reply == nullptr)) {
        LOGERR("Redis Provider: Publish failed permanently for event length %zu", len);
        return -1;
    }

    freeReplyObject(reply);
    return 0;
}

static void redis_provider_cleanup(void) {
    if (g_redis_ctx != nullptr) {
        redisFree(g_redis_ctx);
        g_redis_ctx = nullptr;
    }
    LOGINF("Redis Provider: Cleaned up");
}

struct event_provider const g_event_provider_redis = {
    .name    = "redis",
    .init    = redis_provider_init,
    .publish = redis_provider_publish,
    .cleanup = redis_provider_cleanup};
#endif

#ifdef ORBIT_WITH_KAFKA
static rd_kafka_t       *g_kafka_rk  = nullptr;
static rd_kafka_topic_t *g_kafka_rkt = nullptr;

static void kafka_dr_msg_cb(rd_kafka_t *rk, const rd_kafka_message_t *rkmessage, void *opaque) {
    (void)rk;
    (void)opaque;
    if (unlikely(rkmessage->err)) {
        LOGERR("Kafka Provider: Message delivery failed: %s", rd_kafka_err2str(rkmessage->err));
    }
}

static int kafka_provider_init(void) {
    char             errstr[512];
    rd_kafka_conf_t *conf = rd_kafka_conf_new();
    if (unlikely(conf == nullptr)) {
        LOGERR("Kafka Provider: Failed to create configuration object");
        return -1;
    }

    if (unlikely(
            rd_kafka_conf_set(
                conf,
                "bootstrap.servers",
                g_config.event_kafka_brokers,
                errstr,
                sizeof(errstr)) != RD_KAFKA_CONF_OK))
    {
        LOGERR("Kafka Provider: Failed to set bootstrap.servers: %s", errstr);
        rd_kafka_conf_destroy(conf);
        return -1;
    }

    rd_kafka_conf_set_dr_msg_cb(conf, kafka_dr_msg_cb);

    g_kafka_rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
    if (unlikely(g_kafka_rk == nullptr)) {
        LOGERR("Kafka Provider: Failed to create producer: %s", errstr);
        return -1;
    }

    g_kafka_rkt = rd_kafka_topic_new(g_kafka_rk, g_config.event_kafka_topic, nullptr);
    if (unlikely(g_kafka_rkt == nullptr)) {
        LOGERR("Kafka Provider: Failed to create topic object");
        rd_kafka_destroy(g_kafka_rk);
        g_kafka_rk = nullptr;
        return -1;
    }

    LOGINF(
        "Kafka Provider: Initialized and connected to brokers: %s",
        g_config.event_kafka_brokers);
    return 0;
}

static int kafka_provider_publish(char const *restrict const payload, size_t const len) {
    if (unlikely(g_kafka_rk == nullptr || g_kafka_rkt == nullptr)) {
        return -1;
    }

    int const err = rd_kafka_produce(
        g_kafka_rkt,
        RD_KAFKA_PARTITION_UA,
        RD_KAFKA_MSG_F_COPY,
        (void *)payload,
        len,
        nullptr,
        0,
        nullptr);

    if (unlikely(err == -1)) {
        auto const err_code = rd_kafka_last_error();
        LOGERR("Kafka Provider: Failed to produce message: %s", rd_kafka_err2str(err_code));
        return -1;
    }

    rd_kafka_poll(g_kafka_rk, 0);
    return 0;
}

static void kafka_provider_cleanup(void) {
    if (g_kafka_rkt != nullptr) {
        rd_kafka_topic_destroy(g_kafka_rkt);
        g_kafka_rkt = nullptr;
    }
    if (g_kafka_rk != nullptr) {
        LOGINF("Kafka Provider: Flushing outbound queue...");
        rd_kafka_flush(g_kafka_rk, 2000);
        rd_kafka_destroy(g_kafka_rk);
        g_kafka_rk = nullptr;
    }
    LOGINF("Kafka Provider: Cleaned up");
}

struct event_provider const g_event_provider_kafka = {
    .name    = "kafka",
    .init    = kafka_provider_init,
    .publish = kafka_provider_publish,
    .cleanup = kafka_provider_cleanup};
#endif
