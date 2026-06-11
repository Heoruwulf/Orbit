#pragma once

#ifndef ORBIT_MACROS_H
#define ORBIT_MACROS_H

/**
 * Branch prediction hints for the compiler.
 *
 * Examples:
 *
 *   if (likely(ptr != nullptr)) {
 *       // The compiler will optimize for this path
 *   }
 *
 *   if (unlikely(error_occurred)) {
 *       // The compiler will move this block out of the hot path
 *   }
 */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#endif // ORBIT_MACROS_H
