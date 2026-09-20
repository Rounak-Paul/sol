# Application Installation

- Sol's runtime plugin discovery resolves `plugins` beside the actual executable.
- Linux installs the executable and bundled plugins under `lib/sol`, with a `bin/sol` launcher symlink so both desktop launchers and terminals reach the same runtime directory.
- macOS installs a `Sol.app` bundle; its executable and plugins live together in `Contents/MacOS`.
- Desktop metadata and the shared high-contrast solar image are installation artifacts, not runtime assets.
- `cmake --install` and CPack use the `Sol` component so bundled development dependencies do not become application payload.
- On macOS, `cpack --config build-release/CPackConfig.cmake` produces a DMG containing only `Sol.app`; staging verified the executable, all bundled plugins, `Info.plist`, and `Sol.icns`.
