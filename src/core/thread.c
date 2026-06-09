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
#include "orbit/thread.h"
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <threads.h>
#include <unistd.h>
#include "orbit/event.h"
#include "orbit/log.h"
#include "orbit/rtp_engine.h"
#include "orbit/server.h"
#include "orbit/sip_router.h"
#include "orbit/ws_bridge.h"

static int                g_num_workers    = 0;
static pthread_t         *g_worker_threads = nullptr;
static struct worker_ctx *g_worker_ctxs    = nullptr;

thread_local int t_worker_id = -1;

static void *worker_thread_main(void *arg) {
    struct worker_ctx *ctx = (struct worker_ctx *)arg;
    t_worker_id            = ctx->worker_id;

    // Pin thread to specific core
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ctx->core_id, &cpuset);

    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        LOGERR("[Worker %d] Failed to pin to core %d", ctx->worker_id, ctx->core_id);
    } else {
        LOGINF("[Worker %d] Pinned to core %d", ctx->worker_id, ctx->core_id);
    }

    // Initialize thread-local server context
    if (server_init() < 0) {
        LOGERR("[Worker %d] Failed to initialize io_uring server", ctx->worker_id);
        return nullptr;
    }

    if (sip_router_init() < 0) {
        LOGERR("[Worker %d] Failed to initialize SIP router", ctx->worker_id);
        return nullptr;
    }

    if (rtp_engine_init() < 0) {
        LOGERR("[Worker %d] Failed to initialize RTP engine", ctx->worker_id);
        return nullptr;
    }

    if (ws_bridge_init() < 0) {
        LOGERR("[Worker %d] Failed to initialize WS bridge", ctx->worker_id);
        return nullptr;
    }

    event_init();

    // Start the event loop
    server_run(ctx->event_fd);

    // Cleanup
    ws_bridge_cleanup();
    rtp_engine_cleanup();
    sip_router_cleanup();
    server_cleanup();

    return nullptr;
}

int worker_pool_init(void) {
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs < 1) {
        nprocs = 1;
    }

    g_num_workers    = (int)nprocs;
    g_worker_threads = calloc((size_t)g_num_workers, sizeof(pthread_t));
    g_worker_ctxs    = calloc((size_t)g_num_workers, sizeof(struct worker_ctx));

    if (!g_worker_threads || !g_worker_ctxs) {
        LOGERR("Failed to allocate worker thread arrays");
        return -1;
    }

    LOGINF("Supervisor: Spawning %d worker threads", g_num_workers);

    for (int i = 0; i < g_num_workers; ++i) {
        g_worker_ctxs[i].worker_id = i;
        g_worker_ctxs[i].core_id   = i; // simple 1:1 mapping
        g_worker_ctxs[i].event_fd  = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

        if (g_worker_ctxs[i].event_fd < 0) {
            LOGERR("Failed to create eventfd for worker %d", i);
            return -1;
        }

        if (pthread_create(&g_worker_threads[i], nullptr, worker_thread_main, &g_worker_ctxs[i]) !=
            0)
        {
            LOGERR("Failed to create thread for worker %d", i);
            return -1;
        }
    }

    return 0;
}

void worker_pool_stop_all(void) {
    uint64_t stop_val = 1;
    for (int i = 0; i < g_num_workers; ++i) {
        if (g_worker_ctxs[i].event_fd >= 0) {
            write(g_worker_ctxs[i].event_fd, &stop_val, sizeof(stop_val));
        }
    }
}

void worker_pool_cleanup(void) {
    for (int i = 0; i < g_num_workers; ++i) {
        pthread_join(g_worker_threads[i], nullptr);
        if (g_worker_ctxs[i].event_fd >= 0) {
            close(g_worker_ctxs[i].event_fd);
        }
    }

    free(g_worker_threads);
    free(g_worker_ctxs);
    g_worker_threads = nullptr;
    g_worker_ctxs    = nullptr;
    g_num_workers    = 0;
}

int worker_get_id(void) { return t_worker_id; }

int worker_get_count(void) { return g_num_workers; }
