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

## 2026-09-20 terminal-fixes release install (later same day)

- Installed over the running app: this session's shell is itself hosted
  inside Sol's own integrated terminal, so the running process could not
  be quit first without killing the session. User explicitly chose to
  install anyway and accept the session ending once Sol restarts — see
  [[terminal_close_freeze_and_path_fix]] for what changed.
- `cmake --build build-release -j` then `cmake --install build-release
  --prefix /Applications --component Sol`; installed executable
  (`2e55b902b4a8af0973c2494a84bc812296ccf16f1fcf091c2bedcf132703ae89`)
  byte-matches the freshly built `bin/Sol.app` — confirmed via `shasum`.
- Old running process (PID still resident) keeps its old in-memory image
  until it exits; the fixes only take effect after the user quits and
  relaunches Sol.

## Linux icon registration — 2026-09-21

- `assets/sol.png` is 1254px square, so it must not be advertised as a
  `1024x1024` hicolor raster. The Linux installer now places it in
  `share/icons/hicolor/scalable/apps/com.sol.Sol.png`, allowing the desktop
  icon loader to downscale it for every requested size.
- Direct Linux installs refresh the hicolor icon cache and desktop database
  when `gtk-update-icon-cache` and `update-desktop-database` are available.
  Staged package installs (`DESTDIR`) deliberately skip host cache mutation.

## Rounded application icon — 2026-09-21

- `assets/sol-rounded.png` is the alpha-backed, rounded-corner counterpart to
  the original solar artwork. `assets/Sol-rounded.icns` is generated from its
  standard macOS iconset sizes. Both platform installers consume these files;
  macOS installs the ICNS under the existing `Sol.icns` runtime name.
- The local Release install at `/Applications/Sol.app` has the rounded ICNS
  byte-for-byte (`33ebd694f3ab7d00b7dec39c2abc3326c174ece607b854288a3443f2a3ff9e92`),
  the current executable, and all 13 bundled plugins. LaunchServices, Dock,
  and Finder were refreshed after verifying the hash.

## macOS rounded-icon correction — 2026-09-21

- macOS applies an application-icon mask itself. Installing a pre-rounded
  transparent ICNS makes that mask visible as a halo or border. Linux keeps
  the rounded PNG; macOS uses the original square `Sol.icns` so the system
  applies exactly one mask.
