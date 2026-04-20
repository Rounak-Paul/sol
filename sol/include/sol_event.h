#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
   EVENT TYPES
   ============================================================ */

typedef uint32_t Sol_EventType;

typedef enum Sol_EventType_Builtin {
    SOL_EVENT_NONE = 0,

    /* -- App lifecycle -- */
    SOL_EVENT_APP_INIT,   /* fired once after the app is fully initialised  */
    SOL_EVENT_APP_QUIT,   /* fired just before the app exits                */
    SOL_EVENT_APP_TICK,   /* fired every frame (before rendering)           */

    /* -- Window -- */
    SOL_EVENT_WINDOW_RESIZE,
    SOL_EVENT_WINDOW_FOCUS_GAINED,
    SOL_EVENT_WINDOW_FOCUS_LOST,

    /* -- Raw input (bridged from the underlying window system) -- */
    SOL_EVENT_KEY,
    SOL_EVENT_CHAR,
    SOL_EVENT_MOUSE_BUTTON,
    SOL_EVENT_MOUSE_MOVE,
    SOL_EVENT_MOUSE_SCROLL,

    /* -- Editor: buffer lifecycle -- */
    SOL_EVENT_BUFFER_OPEN,    /* a buffer (file) was opened          */
    SOL_EVENT_BUFFER_CLOSE,   /* a buffer was closed                 */
    SOL_EVENT_BUFFER_SAVE,    /* a buffer was saved to disk          */
    SOL_EVENT_BUFFER_CHANGE,  /* text inside a buffer was mutated    */

    /* -- Editor: cursor / selection -- */
    SOL_EVENT_CURSOR_MOVE,
    SOL_EVENT_SELECTION_CHANGE,

    /* -- Commands (for plugins to bind / emit named actions) -- */
    SOL_EVENT_COMMAND,

    SOL_EVENT__BUILTIN_COUNT   /* sentinel — do not use as an event type */
} Sol_EventType_Builtin;

/* ============================================================
   EVENT-SPECIFIC DATA  (embedded in Sol_Event via union)
   ============================================================ */

/* SOL_EVENT_WINDOW_RESIZE */
typedef struct {
    int width, height;  /* new window size in pixels */
} Sol_ResizeEvent;

/* SOL_EVENT_KEY
   Key/scancode/action/mods mirror GLFW constants (CA_PRESS / CA_RELEASE / CA_REPEAT). */
typedef struct {
    int key;
    int scancode;
    int action;  /* CA_PRESS, CA_RELEASE, CA_REPEAT */
    int mods;    /* modifier bit-mask (shift, ctrl, alt, super) */
} Sol_KeyEvent;

/* SOL_EVENT_CHAR */
typedef struct {
    uint32_t codepoint;  /* Unicode codepoint */
} Sol_CharEvent;

/* SOL_EVENT_MOUSE_BUTTON */
typedef struct {
    int    button;  /* mouse button index  */
    int    action;  /* CA_PRESS / CA_RELEASE */
    int    mods;    /* modifier bit-mask   */
    double x, y;    /* cursor position at the time of the click */
} Sol_MouseButtonEvent;

/* SOL_EVENT_MOUSE_MOVE */
typedef struct {
    double x, y;   /* new cursor position */
    double dx, dy; /* delta from previous position */
} Sol_MouseMoveEvent;

/* SOL_EVENT_MOUSE_SCROLL */
typedef struct {
    double dx, dy; /* scroll offsets (dy is the typical vertical scroll) */
} Sol_MouseScrollEvent;

/* SOL_EVENT_BUFFER_OPEN / BUFFER_CLOSE / BUFFER_SAVE */
typedef struct {
    uint32_t    buffer_id; /* stable, unique ID for this buffer */
    const char *path;      /* absolute file path, or NULL for unsaved buffers */
} Sol_BufferEvent;

/* SOL_EVENT_BUFFER_CHANGE */
typedef struct {
    uint32_t    buffer_id;
    uint32_t    offset;       /* byte offset of the first changed byte           */
    uint32_t    removed_len;  /* how many bytes were removed at `offset`         */
    const char *inserted;     /* pointer to the bytes that replaced them         */
    uint32_t    inserted_len; /* length of `inserted`                            */
} Sol_ChangeEvent;

/* SOL_EVENT_CURSOR_MOVE */
typedef struct {
    uint32_t buffer_id;
    uint32_t line;    /* 0-based line number  */
    uint32_t column;  /* 0-based column (UTF-8 char index) */
    uint32_t offset;  /* byte offset from buffer start     */
} Sol_CursorEvent;

/* SOL_EVENT_SELECTION_CHANGE */
typedef struct {
    uint32_t buffer_id;
    uint32_t anchor_line, anchor_col, anchor_offset; /* fixed end of selection  */
    uint32_t active_line, active_col, active_offset; /* moving end of selection */
} Sol_SelectionEvent;

/* SOL_EVENT_COMMAND */
typedef struct {
    const char *name;  /* null-terminated command name, e.g. "editor.save"  */
    void       *args;  /* optional command-specific argument struct, or NULL */
} Sol_CommandEvent;

/* ============================================================
   THE EVENT
   ============================================================ */

typedef struct Sol_Event {
    Sol_EventType type;
    bool          consumed; /* set to true inside a handler to stop propagation */

    union {
        Sol_ResizeEvent      resize;
        Sol_KeyEvent         key;
        Sol_CharEvent        character;
        Sol_MouseButtonEvent mouse_button;
        Sol_MouseMoveEvent   mouse_move;
        Sol_MouseScrollEvent mouse_scroll;
        Sol_BufferEvent      buffer;
        Sol_ChangeEvent      change;
        Sol_CursorEvent      cursor;
        Sol_SelectionEvent   selection;
        Sol_CommandEvent     command;
        void                *custom_data; /* for plugin-registered event types */
    };
} Sol_Event;

/* ============================================================
   LISTENER
   ============================================================ */

/* Return type for sol_event_on — used to unsubscribe later. */
typedef uint32_t Sol_ListenerID;

#define SOL_INVALID_LISTENER ((Sol_ListenerID)0)

/* Callback signature.  Set event->consumed = true to stop further propagation. */
typedef void (*Sol_EventHandler)(Sol_Event *event, void *user_data);

/* ============================================================
   API
   ============================================================ */

/* Initialise the event bus.  Must be called once before any other sol_event_*
   function.  Typically called at the very start of main(). */
void sol_events_init(void);

/* Tear down the event bus and free all internal memory. */
void sol_events_shutdown(void);

/* Subscribe to an event type.
   Returns SOL_INVALID_LISTENER on failure (invalid type or NULL handler).
   The returned ID can be passed to sol_event_off() to unsubscribe. */
Sol_ListenerID sol_event_on(Sol_EventType type, Sol_EventHandler handler,
                            void *user_data);

/* Unsubscribe a previously registered listener.
   Safe to call from inside a handler — the removal is deferred until after the
   dispatch completes so iteration is not disrupted. */
void sol_event_off(Sol_ListenerID id);

/* Emit an event synchronously.
   All active listeners registered for `type` are called in registration order.
   Any listener may set event->consumed = true to prevent later listeners from
   receiving the event.
   `data` is a pointer to one of the Sol_*Event structs; it is copied into the
   Sol_Event union before dispatch so callers may stack-allocate it. */
void sol_event_emit(Sol_EventType type, const void *data);

/* Register a new custom event type identified by `name`.
   If a type with that name already exists its ID is returned unchanged.
   Returns SOL_EVENT_NONE if the type table is full. */
Sol_EventType sol_event_register_type(const char *name);

/* Look up a previously registered custom type by name.
   Returns SOL_EVENT_NONE if not found. */
Sol_EventType sol_event_find_type(const char *name);

#ifdef __cplusplus
}
#endif
