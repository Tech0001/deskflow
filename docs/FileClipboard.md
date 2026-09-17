# File copy/paste (experimental)

This fork supports copying regular files and folders between Finder on macOS
and Nautilus on Hyprland with the wl-clipboard fallback. Both computers must
run this build. Windows, X11, and the newer Clipboard portal are not integrated
with file transfer yet. Existing releases ignore the new clipboard format.

In Settings, enable **Share copied files and folders** under encrypted
connections on both computers. Keep TLS, certificate verification, and ordinary
clipboard sharing enabled. Allow incoming TCP **24801** from the other computer
on both hosts; the usual mouse/keyboard connection still uses TCP 24800.

Copy the selected files, move the pointer to the other screen, and wait for
`file clipboard transfer complete` in the log before pasting in Finder or
Nautilus. Transfers start on the clipboard/screen transition, rather than on
the Paste command. Copying something new cancels an obsolete incoming selection.
Cut selections are treated as copies; source files are never deleted.

Files are downloaded into a private cache directory and published to the local
clipboard only after the entire selection has passed SHA-256 checks. A failed
transfer does not publish partial files. The native file manager performs the
final paste and handles destination name collisions normally.

## Current limits

- Up to 16 GiB and 4096 entries per copied selection; directory depth up to 32.
- IPv4; one outgoing file connection at a time per source process.
- No resume, drag/drop, move semantics, or live GUI progress bar yet. Progress
  and failures appear in Deskflow's existing log panel.
- Symlinks, devices, sockets, FIFOs, ambiguous case-insensitive names, and
  unsafe/nonportable path components are rejected. A rejected item rejects
  the whole selection. This is not a metadata-preserving backup tool:
  executable bits, timestamps, extended attributes, and resource forks are
  not copied. App bundles containing symlinks are therefore not supported.
- Offers expire after 30 minutes. Recopy/cross screens to create a fresh offer.
  Successful receive caches survive restarts. Caches older than 24 hours are
  removed when a new file transfer starts; failed staging areas are removed
  immediately. Cache storage is under Qt's per-application cache location in
  `file-transfers/`.
- The destination clipboard is not ready until the transfer completes. Files
  already copied to a chosen destination no longer depend on the cache.

## Wire format and trust

This implementation is original code under the repository's existing license;
it incorporates no LocalSend implementation or dependencies.

Clipboard format ID 3 (`Files`) contains UTF-8 JSON, capped at 1 MiB. Version 1
offers have a 256-bit random token, source IPv4 addresses, TCP port, SHA-256 TLS
certificate fingerprint, expiry (milliseconds since epoch, encoded as a string),
and an ordered array of entries. Each entry has a relative `path`, `directory`
boolean, `size` as a decimal string, and, for files, a `sha256` hex digest.
Directory entries precede their children. Absolute local source paths are never
sent. Existing clipboard chunk limits still apply to the small offer, not file
contents. Unknown clipboard format IDs are skipped by older peers.

The offer is carried by Deskflow's authenticated, encrypted clipboard channel.
The receiving worker opens a separate TLS 1.2+ connection and checks the exact
certificate fingerprint *before* sending the token. CA/hostname validation is
replaced by this explicit certificate pin, not by accepting an arbitrary peer.
The source certificate is the same certificate configured for Deskflow.

After TLS authentication the request is `DFT1 <token> <entry-index>\n`.
The source replies `OK <size>\n` followed by exactly that many file bytes, or
`ERROR\n`. A request cannot name a path. Only registered selections are served,
and all file path components are reopened without following symlinks. Both
sender and receiver use bounded buffers. Each received file is checksummed;
the selection is made visible only when every file is complete. File I/O and
network waits run on clipboard workers, outside mouse/keyboard event handling.

This is a fork extension pending upstream design review, not an upstream
Deskflow or LocalSend protocol guarantee.

## Development validation

`FileTransferTests` uses real TLS sockets and temporary files to test files,
folders, empty files, Unicode/spaces, source changes, symlinks/FIFOs, wrong
certificates/tokens, invalid manifests, cancellation, and staging cleanup.
`WaylandClipboardTests::copiedFilesRoundTrip` tests the adapter's offer and
native URI publication through real transfers with controlled clipboard helpers.
The adapter also tests browser URL selections with file sharing on and off.
Transfer tests use temporary identities, cache directories, and ephemeral ports
so they can run alongside an installed Deskflow instance.
`ClipboardTests` covers the new format and rejects signed-overflow format IDs.

Run `QT_QPA_PLATFORM=minimal ctest --test-dir build/src/unittests --output-on-failure`.
macOS must additionally build `OSXFileClipboard.mm` and perform the real Finder
copy/paste test; passing Linux tests alone does not establish macOS support.

Native API references: [Qt QSslSocket](https://doc.qt.io/qt-6/qsslsocket.html)
and [Apple NSPasteboard](https://developer.apple.com/documentation/appkit/nspasteboard/).
