# File copy/paste (experimental)

This fork supports copying regular files and folders between Finder on macOS
and Nautilus on Hyprland with the wl-clipboard fallback. Both computers must
run this build. New version 2 offers are incompatible with the previous eager
transfer build. Windows, X11, and the Clipboard portal are not integrated with
file transfer yet.

In Settings, enable **Share copied files and folders** under encrypted
connections on both computers. Keep TLS, certificate verification, and ordinary
clipboard sharing enabled. Allow incoming TCP **24801** from the other computer
on both hosts; the usual mouse/keyboard connection still uses TCP 24800.
Linux requires libfuse3, `/dev/fuse`, and `fusermount3`. macOS additionally needs
the signed **Deskflow Files** companion and its enabled File Provider extension;
see [the Mac build instructions](FileClipboardMac.md).

## Copy and paste

Copy the selected files and move the pointer to the other screen. Deskflow
sends a small selection manifest and publishes native file references. File
contents are not read, hashed, or transferred just to prepare that offer.
The previous file selection is replaced before downloading contents, so an
early Paste cannot accidentally paste the previous file.

Paste normally in Finder or Nautilus. When the file manager reads a referenced
file, Deskflow downloads it into a private cache and verifies its SHA-256
checksum. Only then can the native file manager read its contents and finish
the paste. Filename collisions are handled by the file manager as usual.
Linux shows a nonmodal receiving dialog with progress and Cancel; macOS uses
File Provider progress and cancellation through Finder.

This is **download on content access**, not an exclusive Paste event hook.
Metadata queries and opening a Linux file without reading do not download it.
Previews, indexing, clipboard tools, or any other app that reads file contents
can trigger a download before an intentional Paste. Deskflow does not change
other applications' settings to prevent that.

Cancelling or failing a download discards its incomplete staging file. Try Paste
again to retry from the beginning. Verified files are reused while available.
Changing the clipboard does not interrupt an already accepted file read. A
folder copy proceeds file by file; a later failure can leave earlier, complete
files in the chosen destination. Native file managers may also create an empty
destination before reporting a failed read. Cut selections are treated as copies;
source files are never deleted.

## Current limits

- Up to 16 GiB and 4096 entries per selection; directory depth up to 32.
- IPv4; at most four outgoing connections per source process. Incoming reads
  are serialized in each Linux receiver / Mac provider process.
- No resume, drag/drop, move semantics, or metadata-preserving backup behavior.
  Executable bits, timestamps, extended attributes, and resource forks are not
  preserved. Destination copies have ordinary owner read/write permissions.
- Symlinks, devices, sockets, FIFOs, ambiguous case-insensitive names, and unsafe
  or nonportable path components reject the entire selection. App bundles
  containing symlinks are therefore not supported.
- Offers expire after 30 minutes; a source retains up to 64 recent offers.
  Recopy/cross screens to make a fresh offer if it expires or the source changes.
  The source computer must be reachable until the requested file is downloaded.
- A file has to fit in the receiving cache as well as the chosen destination.
  An individual download has a 15-minute deadline and a 10-second idle timeout.
- Linux virtual references exist for the core process's lifetime and up to 64
  active selections. Verified files are cached under Qt's cache location in
  `file-transfers/`. Cache reuse in Linux lasts for that receiver and valid offer;
  old cache data and abandoned staging areas are removed after 24 hours when a
  new download starts. Files already pasted to a chosen destination are independent.
- macOS uses one File Provider domain per selection and removes expired domains
  when publishing a new selection. The provider uses a sandboxed receiver helper
  and its private Qt cache. macOS also maintains its own materialized provider
  files; repeat pastes normally read those without another network transfer.
- The companion currently requires macOS 15+, a local signing identity, and
  extension registration. It is not part of the normal upstream app installer.
  This source is implemented but still requires a native Mac build and paired
  Finder testing; Linux validation does not establish macOS support.

## Wire format and trust

This is original code under the repository's existing license. It incorporates
no LocalSend implementation or dependencies.

Clipboard format ID 3 (`Files`) contains UTF-8 JSON, capped at 1 MiB. Version 2
has a 256-bit random token, source IPv4 addresses, TCP port, SHA-256 TLS
certificate fingerprint, expiry (milliseconds since epoch as a string), and
ordered entries. Each entry has a relative `path`, `directory` boolean, and
`size` as a decimal string. Files have a `revision` digest over stat metadata
(device, inode, size, and nanosecond modification/change times). Directories
precede children. Absolute source paths are never sent. File contents do not
pass through Deskflow's keyboard/mouse clipboard stream.

The manifest travels over Deskflow's authenticated, encrypted clipboard channel.
A receiver opens a separate TLS 1.2+ connection and checks the exact offered
certificate fingerprint **before** sending the token. The source certificate
is the same certificate configured for Deskflow.

The request is `DFT2 <token> <entry-index>\n`. The source reopens every path
component without following symlinks and checks the recorded revision. It
replies `OK <size>\n`, followed by that many bytes, then `SHA256 <hex>\n` only
if the source revision still matches after reading. Refused requests receive
`ERROR\n`. The receiver checks the streamed digest before atomically committing
the cache file. A request cannot name an arbitrary path. Buffers are bounded;
file I/O, hashing, and network waits run outside input event handling.
The receiver still understands legacy version 1 manifests / `DFT1`, but emits
only version 2 offers. Both computers must be updated together.

## Development validation

`FileTransferTests` covers real TLS transfers, metadata-only offers, files,
folders, empty files, Unicode/spaces, changes before/during transfer,
symlinks/FIFOs, wrong certificates/tokens, invalid manifests, cancellation,
retry, cache reuse, staging cleanup, and the receiver helper's pipe protocol.

`LazyFileClipboardTests` mounts a real private FUSE filesystem. It checks large
folder listings, no transfer on metadata/open, content reads, native `cp`
permissions, retained accepted reads, cancellation/retry, and an optional 1 GiB
end-to-end transfer with every returned byte verified. Ordinary FUSE tests skip
on hosts without usable FUSE; the requested 1 GiB test fails if FUSE is unavailable.
`WaylandClipboardTests` verifies URI publication through controlled wl-clipboard
helpers. Tests use ephemeral TLS ports and temporary identities/cache directories.

```sh
DESKFLOW_TEST_GIB=1 QT_QPA_PLATFORM=minimal ctest --test-dir build/src/unittests --output-on-failure
```

macOS must additionally compile the Objective-C++ adapter, Swift companion,
and receiver helper, then test keyboard/menu Paste in Finder against this Linux
build. The earlier standalone File Provider probe established API feasibility;
it did not test this integrated Deskflow implementation.
