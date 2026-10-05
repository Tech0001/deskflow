# Gitea: Deskflow client on Omarchy / Hyprland 0.56.2

Use a user-local `hypr-kdeconnect-portal` RemoteDesktop backend alongside the
existing Hyprland and GTK portal backends. This support branch is separate
from the Deskflow file-transfer proposal. The patch applies to:

- Source: https://github.com/iamnarayana/wayland-kdeconnect-fix
- Exact base: `ea55f66c8238235983d60d381bf2abe1fed50043`
- Original license: MIT, reproduced in `LICENSE.bridge`.

The bridge exposes only RemoteDesktop and requires libei/libeis 1.6+. It
provides input; it does not provide the Clipboard portal. The patch retains
`clipboard_enabled=false`. Current Deskflow retries that condition once,
then continues with input. Clipboard and file transfers on this new Linux
client are separate work.

## Patch contents and validation

- Accept the exact `org.deskflow.deskflow` app id. For native apps with no app
  id, retain frontend/session checks and verify the actual original caller's
  executable against the explicitly configured Deskflow core path. Do not
  accept every empty app id or arbitrary executables named deskflow-core.
- Handle libei 1.6 device requests and advertise absolute-pointer button,
  scroll and physical-scale capabilities.
- Convert v120 wheel units to whole notches with a remainder and preserve
  direction on both axes. Clear opposing remainders on reversal.
- Send XKB-derived modifier updates with virtual keyboard events. Repeated
  modifier presses do not accumulate. Release held input when emulation
  stops or a device/client disconnects.

A Release build and all five test suites passed on the server's Omarchy
installation (Qt 6.11.2, libei 1.6.0). The added test uses real libei/libeis
connections with a recording Wayland sink: device discovery, absolute motion,
wheel scale/direction, key/button delivery and cleanup. Separate XKB checks
cover Shift/Ctrl/Alt/Super and repeated presses. Tests never inject input into
the live desktop. Actual portal authorization, compositor input, monitor
geometry and desktop shortcuts still need validation on Gitea. The original
bridge uses the first advertised output; multi-monitor/scaled layouts need
particular attention in that live check.

## Instructions for the agent on Gitea

Proceed with the client installation and input backend below. Preserve existing
local source edits and back up any configuration before changing it. Keep the
current Hyprland and GTK portal providers. Do not install or restart this bridge
on the working Omarchy server.

### 1. Install the matching Deskflow build

In the existing Tech0001/deskflow checkout, fetch origin and use
`integrate/upstream-1.27.0`, commit `d7dcbe69b1bea72cd7e6534cff1db15a5c37f8f0`.
Use the existing working build dependencies. Build and install to the current
user's `.local` prefix:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel 6
cmake --install build
"$HOME/.local/bin/deskflow-core" --version
```

Expected core version: `1.27.0.13 (d7dcbe69)`. Keep GUI and core from the same
build. The older local build `1.26.0.475 (c137d227)` is not the integration build.
Do not build Deskflow from this support branch merely to obtain the bridge patch.

### 2. Build the pinned, patched bridge

From the Deskflow checkout, fetch the support branch and export its patch:

```sh
git fetch origin support/gitea-remotedesktop
git show origin/support/gitea-remotedesktop:deploy/linux/hyprland-remote/deskflow-bridge.patch > /tmp/deskflow-bridge.patch
```

The bridge needs `cmake`, a C++23 compiler, `pkgconf`, `qt6-base`, `wayland`,
`libxkbcommon`, `libei` version 1.6+, and `xdg-desktop-portal`. Check/install only
missing dependencies using the normal Omarchy package workflow. KDE Connect
itself is not required for Deskflow.

Use a new clone to avoid overwriting other work:

```sh
git clone https://github.com/iamnarayana/wayland-kdeconnect-fix "$HOME/hypr-deskflow-bridge"
cd "$HOME/hypr-deskflow-bridge"
git checkout --detach ea55f66c8238235983d60d381bf2abe1fed50043
git apply --check /tmp/deskflow-bridge.patch
git apply /tmp/deskflow-bridge.patch
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
  -DHKCF_DESKFLOW_EXECUTABLE="$HOME/.local/bin/deskflow-core"
cmake --build build --parallel 6
ctest --test-dir build --output-on-failure
cmake --install build
```

If Deskflow was deliberately installed elsewhere, set
`HKCF_DESKFLOW_EXECUTABLE` to the resolved absolute path of that installed
`deskflow-core` executable before building. A mismatched path will deny native
sessions. Do not disable caller checks to work around a wrong path.

### 3. Route only RemoteDesktop to the bridge

Inspect the effective portal configuration and `XDG_CURRENT_DESKTOP`. Preserve
its existing entries, including Hyprland screen sharing and GTK fallback.
Back up the relevant user configuration, then add this entry to its
`[preferred]` section:

```ini
org.freedesktop.impl.portal.RemoteDesktop=hypr-kdeconnect
```

If there is no user configuration, copy the effective desktop-specific portal
configuration to the equivalent filename under `~/.config/xdg-desktop-portal/`
and add the one entry. Do not replace the whole file with a generic example;
desktop-specific files take precedence over `portals.conf` in the same directory.
Ensure the installed `.portal` metadata and D-Bus service are discovered.

During initial setup on Gitea, with no screen-sharing session to interrupt:

```sh
systemctl --user daemon-reload
systemctl --user restart xdg-desktop-portal.service
systemctl --user start hypr-kdeconnect-portal.service
busctl --user introspect org.freedesktop.portal.Desktop /org/freedesktop/portal/desktop org.freedesktop.portal.RemoteDesktop
```

The public RemoteDesktop interface must expose `CreateSession`, `SelectDevices`,
`Start` and `ConnectToEIS`. If it is missing, inspect backend discovery/routing
and the portal journal before rebuilding anything.

### 4. Connect and verify

Launch the installed Deskflow GUI as the desktop user. Set computer name
`Gitea`, client mode, server `192.168.50.149`, and keep TLS and peer verification
enabled. Compare the server fingerprint before trusting it. The server agent
still needs the desired screen position to add Gitea to the external layout;
the existing Mac offset must be preserved.

Report the installed core version, public RemoteDesktop interface, bridge
service status, and any session errors. Filter operational logs, not typed
content. Once the server layout is ready, test pointer motion/clicks, letters,
Shift/Ctrl/Alt/Super press-and-release, scrolling both directions, crossing away
while holding a modifier, and reconnecting. Use a blank document for typing.
Test actual desktop shortcuts separately; do not rewrite the user's bindings
as an installation shortcut. Confirm cursor position on the client with
`hyprctl cursorpos` if movement is uncertain.

### Rollback

Stop the bridge's user service and restore the saved portal configuration.
Remove only the four files installed by this bridge (binary, `.portal` metadata,
D-Bus service and user systemd service), then reload user units and restart the
portal. Preserve the existing Hyprland/GTK providers and Deskflow settings.

Background discussion: https://github.com/omacom/omarchy/discussions/10097
This patch deliberately verifies native callers and keeps clipboard support
reported as unavailable rather than using the broader session workaround there.
