# Recent project sessions

## Scope

Persist the bounded most-recent project roots outside the live multi-project
runtime, expose them as fast-open rows in the empty-buffer welcome surface, and
mirror the same entries in the `Sol > Recents` submenu.

## Ownership

- The application host owns durable recent project paths and records only
  successfully created path-backed project sessions.
- The UI receives a snapshot plus an open callback; it owns presentation and
  menu rebuilds, but never parses or writes the session store.
- Empty projects and unavailable paths are not persisted. Opening an unavailable
  saved item simply removes it from the visible list after the host rejects it.

## Contract

Use a small bounded, atomic, corruption-tolerant store under `.sol`. A recent
entry always opens a new isolated project runtime, preserving the existing
multi-project lifecycle boundary. The initial welcome screen is shown when the
active project has no buffers; it lists Recents above shortcuts without hiding
new/open actions.

## Implemented

- `$HOME/.sol/recent_sessions` stores up to 12 existing project directories in
  most-recent-first order. Writes are flushed, synced, and atomically replaced;
  malformed, stale, empty, or newline-containing entries are ignored safely.
- Opening a session is queued through the existing project lifecycle boundary,
  so it creates a new isolated project rather than mutating the active one.
- `Sol > Recents` and the no-buffer welcome screen both consume the same
  UI-owned snapshot and invoke the same host callback. Glass and Retro style
  the welcome rows as normal controls.

## Validation

`sol`, `sol_style_tests`, and `sol_project_native_tests` build successfully;
`sol_style_tests` passes and root/submodule diff checks are clean. The complete
CTest suite was also run: 19/20 passed, with the pre-existing Git plugin test
failing because its tracked-diff fixture expected output for
`.context/application-installation.md` but received an empty diff.
