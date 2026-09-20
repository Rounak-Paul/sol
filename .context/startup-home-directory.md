# Startup Home Directory

- With no command-line path, Sol opens the current user's home directory instead of the launcher process's inherited working directory.
- An explicit file or directory argument remains the startup target.
- `sol_platform_get_user_home` centralizes platform environment lookup; it returns failure without a fallback path when no user-home directory is available.
- File and folder pickers with no explicit initial directory also open at the user's home directory; project-specific pickers continue to supply their active project root.
