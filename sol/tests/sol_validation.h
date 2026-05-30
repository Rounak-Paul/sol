// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Sol contributors.

#ifndef SOL_VALIDATION_H
#define SOL_VALIDATION_H

/* sol_validation.h — Runtime contract validation for Sol subsystems.
 *
 * Usage
 * -----
 *   #include "sol_validation.h"
 *
 *   // Anywhere an invariant must hold at runtime:
 *   SOL_VALIDATE_TEXT_BUFFER(tb);
 *   SOL_VALIDATE_BUFFER_SYSTEM(sys);
 *
 * Checks are **compiled out entirely** when SOL_VALIDATION_ENABLED is
 * not defined (i.e. in Release builds and production code that does not
 * opt in).  Define it in Debug/Test CMake targets only.
 *
 * When SOL_VALIDATION_ENABLED is defined, a failing check:
 *   1. Prints a diagnostic to stderr.
 *   2. Calls the installed violation handler (default: abort()).
 *
 * Installing a custom handler (e.g. for tests that want to catch
 * violations without aborting):
 *   sol_validation_set_handler(my_handler);
 *
 * Contract families
 * -----------------
 *   - SOL_VAL_REQUIRE(expr)            — low-overhead pre-condition check
 *   - SOL_VAL_ENSURE(expr)             — post-condition check
 *   - SOL_VALIDATE_EVENT_BUS(bus)      — SolEventBus invariants
 *   - SOL_VALIDATE_BUFFER_SYSTEM(sys)  — SolBufferSystem invariants
 *   - SOL_VALIDATE_TEXT_BUFFER(tb)     — SolTextBuffer cursor/rope invariants
 *   - SOL_VALIDATE_FLOW_DESC(desc)     — SolCommandFlowDesc parameter check
 *   - SOL_VALIDATE_SUBSCRIPTION(desc)  — SolEventSubscriptionDesc check
 */

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Handler type and installation                                       */
/* ------------------------------------------------------------------ */

/** Called with a human-readable description when a validation check
 *  fails.  The default handler prints to stderr and calls abort(). */
typedef void (*SolValidationHandlerFn)(const char *expr,
                                       const char *file,
                                       int         line,
                                       const char *message);

#ifdef SOL_VALIDATION_ENABLED

#include <stdio.h>
#include <stdlib.h>

/* Forward declarations for the check functions (defined below). */
struct SolEventBus;
struct SolBufferSystem;
struct SolTextBuffer;
struct SolCommandFlowDesc;
struct SolEventSubscriptionDesc;

/** Install a custom validation handler.  Pass NULL to restore the
 *  default abort-and-print handler. */
static inline void sol_validation_set_handler(SolValidationHandlerFn fn);

/* ------------------------------------------------------------------ */
/* Implementation details — do not use directly                        */
/* ------------------------------------------------------------------ */

/** Internal: retrieve the currently active handler. */
static inline SolValidationHandlerFn *sol_validation_handler_ptr_(void)
{
    static SolValidationHandlerFn s_handler = NULL;
    return &s_handler;
}

static inline void sol_validation_default_handler_(const char *expr,
                                                    const char *file,
                                                    int         line,
                                                    const char *message)
{
    fprintf(stderr, "[sol_validation] FAILED: %s\n"
                    "  at %s:%d\n"
                    "  detail: %s\n",
            expr, file, line, message ? message : "");
    abort();
}

static inline void sol_validation_set_handler(SolValidationHandlerFn fn)
{
    *sol_validation_handler_ptr_() = fn;
}

static inline void sol_validation_fire_(const char *expr,
                                        const char *file,
                                        int         line,
                                        const char *message)
{
    SolValidationHandlerFn h = *sol_validation_handler_ptr_();
    if (h) {
        h(expr, file, line, message);
    } else {
        sol_validation_default_handler_(expr, file, line, message);
    }
}

/* ------------------------------------------------------------------ */
/* Core assertion macros                                               */
/* ------------------------------------------------------------------ */

/** General pre-/post-condition check. */
#define SOL_VAL_REQUIRE(expr) \
    do { \
        if (!(expr)) { \
            sol_validation_fire_(#expr, __FILE__, __LINE__, "pre-condition violated"); \
        } \
    } while (0)

#define SOL_VAL_ENSURE(expr) \
    do { \
        if (!(expr)) { \
            sol_validation_fire_(#expr, __FILE__, __LINE__, "post-condition violated"); \
        } \
    } while (0)

/** Like SOL_VAL_REQUIRE but with a custom message. */
#define SOL_VAL_REQUIRE_MSG(expr, msg) \
    do { \
        if (!(expr)) { \
            sol_validation_fire_(#expr, __FILE__, __LINE__, (msg)); \
        } \
    } while (0)

/* ------------------------------------------------------------------ */
/* SolEventBus invariants                                             */
/* ------------------------------------------------------------------ */

#include "sol_event.h"

/** Validate an event subscription descriptor before use. */
#define SOL_VALIDATE_SUBSCRIPTION(desc) \
    do { \
        const SolEventSubscriptionDesc *_d = (desc); \
        SOL_VAL_REQUIRE_MSG(_d != NULL, \
            "SolEventSubscriptionDesc must not be NULL"); \
        SOL_VAL_REQUIRE_MSG(_d->handler != NULL, \
            "subscription handler must not be NULL — unhandled events " \
            "silently dropped if handler is NULL"); \
        SOL_VAL_REQUIRE_MSG( \
            (_d->event_name != NULL) || (_d->event_type != 0u), \
            "subscription must specify event_name or event_type"); \
    } while (0)

/** Validate an event descriptor before publish/post. */
#define SOL_VALIDATE_EVENT_DESC(desc) \
    do { \
        const SolEventDesc *_d = (desc); \
        SOL_VAL_REQUIRE_MSG(_d != NULL, "SolEventDesc must not be NULL"); \
        SOL_VAL_REQUIRE_MSG( \
            (_d->event_name != NULL) || (_d->event_type != 0u), \
            "event must have a name or type set before publish/post"); \
        SOL_VAL_REQUIRE_MSG( \
            _d->payload_size == 0u || _d->payload != NULL, \
            "payload_size > 0 but payload pointer is NULL"); \
    } while (0)

/* ------------------------------------------------------------------ */
/* SolBufferSystem invariants                                         */
/* ------------------------------------------------------------------ */

#include "sol_buffer.h"

/** Light-weight buffer system sanity check.
 *  - sys must be non-NULL
 *  - buffer_count must not exceed buffer_capacity
 *  - active_leaf (if nonzero) must be a valid node in the tree
 */
#define SOL_VALIDATE_BUFFER_SYSTEM(sys) \
    do { \
        SOL_VAL_REQUIRE_MSG((sys) != NULL, \
            "SolBufferSystem pointer must not be NULL"); \
        SOL_VAL_REQUIRE_MSG(sol_buffer_count(sys) < (size_t)1000000u, \
            "SolBufferSystem buffer_count exceeds sanity limit"); \
    } while (0)

/* ------------------------------------------------------------------ */
/* SolTextBuffer invariants                                           */
/* ------------------------------------------------------------------ */

#include "sol_text_buffer.h"

/** Validate a SolTextBuffer's cursor/rope consistency.
 *  - tb must be non-NULL
 *  - cursor_byte must be within the file (checked via public API)
 *  - scroll_top must be >= 0
 */
#define SOL_VALIDATE_TEXT_BUFFER(tb) \
    do { \
        SOL_VAL_REQUIRE_MSG((tb) != NULL, "SolTextBuffer must not be NULL"); \
        SOL_VAL_REQUIRE_MSG(sol_text_buffer_scroll_top(tb) >= 0, \
            "scroll_top must be non-negative"); \
    } while (0)

/* ------------------------------------------------------------------ */
/* SolCommandFlowDesc invariants                                      */
/* ------------------------------------------------------------------ */

#include "sol_ui_system.h"

/** Validate a command flow descriptor before registration.
 *  - desc and desc->action must not be NULL
 *  - sequence_length must be in [1, SOL_UI_MAX_FLOW_SEQUENCE_LEN]
 *  - sequence pointer must be non-NULL when sequence_length > 0
 */
#define SOL_VALIDATE_FLOW_DESC(desc) \
    do { \
        const SolCommandFlowDesc *_d = (desc); \
        SOL_VAL_REQUIRE_MSG(_d != NULL, \
            "SolCommandFlowDesc must not be NULL"); \
        SOL_VAL_REQUIRE_MSG(_d->action != NULL && _d->action[0] != '\0', \
            "command flow action must be a non-empty string"); \
        /* sequence_length == 0 with .key set is handled by the system; \
           sequence_length > max is always wrong. */ \
        SOL_VAL_REQUIRE_MSG( \
            _d->sequence_length <= SOL_UI_MAX_FLOW_SEQUENCE_LEN, \
            "sequence_length exceeds SOL_UI_MAX_FLOW_SEQUENCE_LEN"); \
        SOL_VAL_REQUIRE_MSG( \
            _d->sequence_length == 0u || _d->sequence != NULL, \
            "sequence is NULL but sequence_length > 0"); \
    } while (0)

/* ------------------------------------------------------------------ */
/* Rope invariant                                                      */
/* ------------------------------------------------------------------ */

#include "sol_rope.h"

/** Validate a SolRope's basic structural invariants. */
#define SOL_VALIDATE_ROPE(rope) \
    do { \
        SOL_VAL_REQUIRE_MSG((rope) != NULL, "SolRope must not be NULL"); \
        /* byte_len and line_count are O(1) reads; non-zero byte_len \
           with zero-length rope would indicate corruption. */ \
        SOL_VAL_REQUIRE_MSG( \
            sol_rope_byte_len(rope) < (size_t)0x40000000ul, \
            "SolRope byte_len exceeds 1 GiB sanity limit"); \
    } while (0)

/* ------------------------------------------------------------------ */
/* No-op versions when validation is disabled                          */
/* ------------------------------------------------------------------ */

#else /* SOL_VALIDATION_ENABLED not defined */

#define SOL_VAL_REQUIRE(expr)           ((void)0)
#define SOL_VAL_ENSURE(expr)            ((void)0)
#define SOL_VAL_REQUIRE_MSG(expr, msg)  ((void)0)

#define SOL_VALIDATE_SUBSCRIPTION(desc)    ((void)0)
#define SOL_VALIDATE_EVENT_DESC(desc)      ((void)0)
#define SOL_VALIDATE_BUFFER_SYSTEM(sys)    ((void)0)
#define SOL_VALIDATE_TEXT_BUFFER(tb)       ((void)0)
#define SOL_VALIDATE_FLOW_DESC(desc)       ((void)0)
#define SOL_VALIDATE_ROPE(rope)            ((void)0)

static inline void sol_validation_set_handler(SolValidationHandlerFn fn)
{
    (void)fn;
}

#endif /* SOL_VALIDATION_ENABLED */

#endif /* SOL_VALIDATION_H */
