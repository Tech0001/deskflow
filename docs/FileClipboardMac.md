# Build the integrated Mac file clipboard

This is the actual Deskflow implementation, not another standalone API probe.
The Linux build has passed its 33 test suites, including a real 1 GiB encrypted
FUSE transfer. The Mac integration has also been compiled and exercised through
the installed File Provider with local TLS transfers. Paired Linux/Mac acceptance
testing is still required before calling the feature ready. Do not push changes yet.

## Source and configuration

The handoff contains a full source snapshot and a patch against commit
`63f911f144cb19007a56738f7b7c02ea0d428f77`. Use either:

- Extract `deskflow-source.tar.gz` to a new directory and build there; no Git
  changes or network fetch are needed.
- Apply `deskflow-on-demand-files.patch` to a clean checkout of that commit after
  `git apply --check`. Preserve any local Mac changes before applying it.

Use the existing Mac compiler, Qt 6.7+ / OpenSSL 3+ dependencies, signing identity,
and Deskflow settings. The previously successful File Provider probe used
macOS 26.6.2, Xcode 26.6, arm64, and the local Developer ID identity in team
`HXKQ77VWW2`. The companion targets macOS 15+. Do not reuse the old probe's
bundle identifier or domains.

Set `DESKFLOW_SIGN_ID` to the existing full signing identity shown by
`security find-identity -v -p codesigning`, and set `DESKFLOW_TEAM_ID` to its team.
Set `DESKFLOW_QT_PREFIX` to the existing Qt prefix, for example the value printed
by `brew --prefix qt` when using Homebrew. The following commands assume the
source directory is the current directory:

```sh
cmake -S . -B build-lazy -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$DESKFLOW_QT_PREFIX" \
  -DAPPLE_CODESIGN_DEV="$DESKFLOW_SIGN_ID" \
  -DBUILD_TESTS=ON
cmake --build build-lazy --parallel 8
```

Use the Mac's existing OpenSSL prefix option if required by its setup. Keep a
single architecture consistent across Deskflow, Qt, the receiver helper, and
Swift companion. Omit the optional 1 GiB FUSE test environment variable on macOS.

`build-lazy/bin/deskflow-file-transfer` is the shared C++ TLS receiver used by the
extension. Its Linux integration test includes a large manifest sent through a
pipe, cancellation, and checksum validation. The helper must be built from this
source; do not substitute the old eager-transfer core.

## Build and sign the companion

```sh
python3 deploy/mac/build-file-provider.py \
  --helper build-lazy/bin/deskflow-file-transfer \
  --identity "$DESKFLOW_SIGN_ID" \
  --team "$DESKFLOW_TEAM_ID" \
  --arch arm64 \
  --macdeployqt "$DESKFLOW_QT_PREFIX/bin/macdeployqt" \
  --output build-lazy/companion
```

Use `--arch x86_64` for an Intel build. The script compiles the Swift host and
replicated File Provider extension, bundles the C++ receiver and Qt libraries,
and signs the result inside-out. The app and extension share the team-prefixed
app group. They have sandbox and outbound-network entitlements; the receiver
inherits the extension sandbox. The script does not install or register anything.

The receiver, frameworks, Qt plugins, and `qt.conf` must live inside the extension
bundle. The provider resolves the receiver with `Bundle.main.url(forAuxiliaryExecutable:)`
and starts it with the shared storage directory as its working directory. A
receiver placed only in the containing app failed to launch in the native provider
test, even though compilation and signing succeeded.

With split Homebrew Qt packages, add `--qt-libpath /opt/homebrew/opt/qtsvg/lib`
(adjust the prefix on Intel). The script completes Qt deployment before embedding
the extension, repeats dependency discovery for copied plugins, and signs after
all load-command rewrites. Verify the final app with `codesign --verify --deep
--strict` and check `otool -L` dependencies; no Homebrew paths or missing bundled
frameworks should remain. Deploy the main app with both `deskflow-core` and
`deskflow-file-transfer` passed as extra executables to `macdeployqt`.

Preserve the existing main app's signing identity when replacing it so its
designated requirement remains the same. The separate companion can use the
available Developer ID identity; `--team` must match that companion identity.

Keep the prior working Deskflow app as a rollback copy. Package/sign the new main
Deskflow app using the same process that worked for the previous fork build; its
core is under `build-lazy/bin/Deskflow.app/Contents/MacOS/deskflow-core`. CMake's
normal install step can stage the main app in a separate output directory:

```sh
cmake --install build-lazy --prefix "$PWD/build-lazy/staged"
```

Check Qt deployment, signatures, and the existing macOS Accessibility/Input
Monitoring permissions for the final main app path. Do not replace the active
app or interrupt the user's remote input until the matching pair is ready.

## Register and enable the companion

After successful compilation, install the companion at the path Deskflow knows:

```sh
mkdir -p "$HOME/Applications"
ditto "build-lazy/companion/Deskflow Files.app" "$HOME/Applications/Deskflow Files.app"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
  -f "$HOME/Applications/Deskflow Files.app"
pluginkit -a "$HOME/Applications/Deskflow Files.app/Contents/PlugIns/ClipboardProvider.appex"
pluginkit -e use -i org.deskflow.files.provider
```

If macOS requires user consent, enable **Deskflow Files** in System Settings →
General → Login Items & Extensions → File Providers. Reuse the successful native
registration/signing procedure from the earlier Mac probe if this OS presents
an additional prompt. Do not disable unrelated clipboard apps or change Raycast.

Deskflow finds the companion under `~/Applications` or `/Applications`.
`DESKFLOW_FILE_PROVIDER_HELPER` can override the host executable path for local
debugging. Keep ordinary clipboard sharing, **Share copied files and folders**,
TLS, and certificate checking enabled. Existing peer trust and TCP 24801 access
should remain as configured.

## Paired acceptance tests

Activate matching new Deskflow builds on both machines. New version 2 offers
are incompatible with the previous eager-transfer receiver. Check:

1. Copy a new file on either machine, cross screens, then immediately Paste with
   both the keyboard shortcut and the file manager's menu. The new file must be
   selected instead of the previously copied file.
2. Repeat both directions with a file of at least 1 GiB. Compare source and
   destination SHA-256 digests (`shasum -a 256` on Mac; `sha256sum` on Linux).
   Observe Finder progress on Mac and the Deskflow receiving dialog on Linux.
3. Cancel during transfer, verify incomplete cache staging is removed, then
   Paste again successfully. Native destination cleanup follows Finder/Nautilus
   behavior; a folder copy can retain earlier complete files.
4. Copy multiple files, a nested folder, an empty folder/file, and Unicode/spaced
   names. Verify the destination files are writable by the owner.
5. Paste the same selection again; verify reuse of already materialized contents.
6. Prepare an offer, change the source before pasting, and verify an error rather
   than a silently different file. Recopy the source and Paste successfully.
7. Copy files, only cross screens and return: no contents should transfer from
   metadata alone. Record separately if a preview or background reader accesses
   contents; this implementation is lazy on file access, not a global Paste hook.
8. Confirm text, images, mouse, and keyboard still work; keep logging at INFO.

Report integrated build/runtime errors with the relevant source fix and concise
logs. Do not run more Raycast or generic File Provider research experiments.

## Rollback / cleanup

Stop the new Deskflow before restoring the prior main app. To remove only this
companion's provider domains, run:

```sh
"$HOME/Applications/Deskflow Files.app/Contents/MacOS/DeskflowFiles" remove-all
pluginkit -r "$HOME/Applications/Deskflow Files.app/Contents/PlugIns/ClipboardProvider.appex"
```

Then move **Deskflow Files.app** out of Applications. Existing completed files in
user-selected destinations are independent of these domains. Do not remove the
user's Deskflow configuration or peer certificates.
