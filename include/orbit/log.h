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
#pragma once

#include <sched.h>
#include <signal.h>
#include <stdio.h>

/**
 * @file log.h
 * @brief Logging macros.
 *
 * Provides LOGINF, LOGERR, and LOGDBG macros.
 * LOGDBG is only active when DEBUG is defined.
 */

/**
 * @brief Resolves a signal number to its corresponding string name.
 *
 * @param signum The signal integer value (e.g., SIGINT, SIGTERM).
 * @return A static string pointer representing the signal name, or "UNKNOWN".
 */
static inline char const *log_get_signal_name(int signum) {
    switch (signum) {
    case SIGINT:
        return "SIGINT";
    case SIGTERM:
        return "SIGTERM";
    case SIGQUIT:
        return "SIGQUIT";
    case SIGHUP:
        return "SIGHUP";
    case SIGKILL:
        return "SIGKILL";
    case SIGUSR1:
        return "SIGUSR1";
    case SIGUSR2:
        return "SIGUSR2";
    default:
        return "UNKNOWN";
    }
}

#define LOGINF(...)                                                                                \
    do {                                                                                           \
        fprintf(stdout, "[Core %03d] [INFO] [%s:%d] ", sched_getcpu(), __FILE__, __LINE__);        \
        fprintf(stdout, __VA_ARGS__);                                                              \
        fprintf(stdout, "\n");                                                                     \
        fflush(stdout);                                                                            \
    } while (0)

#define LOGINF_LOC(file, line, ...)                                                                \
    do {                                                                                           \
        fprintf(stdout, "[Core %03d] [INFO] [%s:%d] ", sched_getcpu(), file, line);                \
        fprintf(stdout, __VA_ARGS__);                                                              \
        fprintf(stdout, "\n");                                                                     \
        fflush(stdout);                                                                            \
    } while (0)

#define LOGWRN(...)                                                                                \
    do {                                                                                           \
        fprintf(stdout, "[Core %03d] [WARN] [%s:%d] ", sched_getcpu(), __FILE__, __LINE__);        \
        fprintf(stdout, __VA_ARGS__);                                                              \
        fprintf(stdout, "\n");                                                                     \
        fflush(stdout);                                                                            \
    } while (0)

#define LOGWRN_LOC(file, line, ...)                                                                \
    do {                                                                                           \
        fprintf(stdout, "[Core %03d] [WARN] [%s:%d] ", sched_getcpu(), file, line);                \
        fprintf(stdout, __VA_ARGS__);                                                              \
        fprintf(stdout, "\n");                                                                     \
        fflush(stdout);                                                                            \
    } while (0)

#define LOGERR(...)                                                                                \
    do {                                                                                           \
        fprintf(stderr, "[Core %03d] [ERRO] [%s:%d] ", sched_getcpu(), __FILE__, __LINE__);        \
        fprintf(stderr, __VA_ARGS__);                                                              \
        fprintf(stderr, "\n");                                                                     \
        fflush(stderr);                                                                            \
    } while (0)

#define LOGERR_LOC(file, line, ...)                                                                \
    do {                                                                                           \
        fprintf(stderr, "[Core %03d] [ERRO] [%s:%d] ", sched_getcpu(), file, line);                \
        fprintf(stderr, __VA_ARGS__);                                                              \
        fprintf(stderr, "\n");                                                                     \
        fflush(stderr);                                                                            \
    } while (0)

#if defined(LOG_DEBUG)
#define LOGDBG(...)                                                                                \
    do {                                                                                           \
        fprintf(stdout, "[Core %03d] [DBUG] [%s:%d] ", sched_getcpu(), __FILE__, __LINE__);        \
        fprintf(stdout, __VA_ARGS__);                                                              \
        fprintf(stdout, "\n");                                                                     \
        fflush(stdout);                                                                            \
    } while (0)

#define LOGDBG_LOC(file, line, ...)                                                                \
    do {                                                                                           \
        fprintf(stdout, "[Core %03d] [DBUG] [%s:%d] ", sched_getcpu(), file, line);                \
        fprintf(stdout, __VA_ARGS__);                                                              \
        fprintf(stdout, "\n");                                                                     \
        fflush(stdout);                                                                            \
    } while (0)
#else
#define LOGDBG(...)                                                                                \
    do {                                                                                           \
    } while (0)

#define LOGDBG_LOC(file, line, ...)                                                                \
    do {                                                                                           \
    } while (0)
#endif
