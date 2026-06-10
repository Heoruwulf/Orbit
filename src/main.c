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
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include "orbit/config.h"
#include "orbit/event.h"
#include "orbit/log.h"
#include "orbit/rtp_engine.h"
#include "orbit/server.h"
#include "orbit/sip_router.h"
#include "orbit/thread.h"
#include "orbit/version.h"
#include "orbit/ws_bridge.h"

#ifdef NDEBUG
constexpr char const BUILD_TYPE[] = "release";
#else
constexpr char const BUILD_TYPE[] = "debug";
#endif

static void print_version(void) {
    printf("Orbit %s (%s build)\n", ORBIT_VERSION, BUILD_TYPE);
    printf("Built on: %s %s\n", ORBIT_BUILD_DATE, ORBIT_BUILD_TIME);
}

static void parse_cli_args(int const argc, char *const argv[]) {
    for (int i = 1; i < argc; ++i) {
        auto const arg = argv[i];
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            print_version();
            exit(EXIT_SUCCESS);
        }
    }
}

static void log_version_info(void) {
    LOGINF(
        "Orbit %s (%s build, built on %s at %s).",
        ORBIT_VERSION,
        BUILD_TYPE,
        ORBIT_BUILD_DATE,
        ORBIT_BUILD_TIME);
}

int main(int argc, char *argv[]) {
    parse_cli_args(argc, argv);

    // Tune glibc memory allocator to never use mmap (force brk)
    // and never trim the heap. This ensures that the wslay warmup
    // memory stays allocated and prefaulted in the process.
    mallopt(M_MMAP_MAX, 0);
    mallopt(M_TRIM_THRESHOLD, -1);

    log_version_info();

    if (config_load() < 0) {
        LOGERR("Failed to load configuration. Check environment variables.");
        return EXIT_FAILURE;
    }

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rlim_t const required_fds = (g_config.max_calls * 4) + 1024;
        if (rl.rlim_cur < required_fds) {
            if (rl.rlim_max >= required_fds) {
                rl.rlim_cur = required_fds;
                setrlimit(RLIMIT_NOFILE, &rl);
                LOGINF("Increased RLIMIT_NOFILE to %lu", (unsigned long)rl.rlim_cur);
            } else {
                rl.rlim_cur = rl.rlim_max;
                setrlimit(RLIMIT_NOFILE, &rl);
                LOGERR(
                    "WARNING: RLIMIT_NOFILE max (%lu) is less than required (%lu). You may "
                    "experience 'Too many open files' under high load.",
                    (unsigned long)rl.rlim_max,
                    (unsigned long)required_fds);
            }
        }
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
        LOGERR("Supervisor: Failed to set sigprocmask");
        return EXIT_FAILURE;
    }

    if (worker_pool_init() < 0) {
        LOGERR("Supervisor: Failed to initialize worker pool.");
        return EXIT_FAILURE;
    }

    LOGINF("Starting orbit supervisor...");

    int sig_fd = signalfd(-1, &mask, 0);
    if (sig_fd >= 0) {
        struct signalfd_siginfo siginfo;
        ssize_t                 s = read(sig_fd, &siginfo, sizeof(struct signalfd_siginfo));
        if (s == sizeof(struct signalfd_siginfo)) {
            LOGINF(
                "Supervisor: Caught signal %d. Signaling all workers to gracefully shut down.",
                siginfo.ssi_signo);
        }
        close(sig_fd);
    }

    LOGINF("Shutting down workers...");
    worker_pool_stop_all();
    worker_pool_cleanup();

    LOGINF("orbit supervisor stopped.");
    return EXIT_SUCCESS;
}
