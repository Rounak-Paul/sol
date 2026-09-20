# Application Installation

- Sol's runtime plugin discovery resolves `plugins` beside the actual executable.
- Linux installs the executable and bundled plugins under `lib/sol`, with a `bin/sol` launcher symlink so both desktop launchers and terminals reach the same runtime directory.
- macOS installs a `Sol.app` bundle; its executable and plugins live together in `Contents/MacOS`.
- Desktop metadata and the shared high-contrast solar image are installation artifacts, not runtime assets.
- `cmake --install` and CPack use the `Sol` component so bundled development dependencies do not become application payload.
- On macOS, `cpack --config build-release/CPackConfig.cmake` produces a DMG containing only `Sol.app`; staging verified the executable, all bundled plugins, `Info.plist`, and `Sol.icns`.

## 2026-09-20 scrollbar-chrome release install

- Reconfigured and built `build-release` as Release, then installed the `Sol`
  component with `cmake --install build-release --prefix /Applications --component Sol`.
- `/Applications/Sol.app/Contents/MacOS/Sol` byte-matches the fresh local
  bundle executable (`0fcd140dc7a56910cae800ce73dc66df0f34b6d866f6d8628ee383f5170c9429`).
- The installed bundle has the same 13 plugin module names and byte-identical
  contents as `bin/Sol.app`; it includes the scrollbar-chrome changes in
  Causality and the normal/Retro Sol styles.
