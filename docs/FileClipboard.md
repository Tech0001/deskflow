# File copy/paste (experimental)

This fork supports copying regular files and folders between Finder on macOS
and Nautilus on Hyprland with the wl-clipboard fallback. Both computers must
run this build. New version 2 offers are incompatible with the previous eager
transfer build. Windows, X11, and the Clipboard portal are not integrated with
file transfer yet.

In Settings, enable **Share copied files and folders** under encrypted
connections on both computers. Keep TLS, certificate verification, and ordinary
clipboard sharing enabled. Saving after enabling files opens **Set up file
sharing**. The same setup is available through the **Set up file sharing…**
button for an existing installation or when adding another computer.
Linux requires libfuse3, `/dev/fuse`, and `fusermount3`. macOS additionally needs
the signed **Deskflow Files** companion and its enabled File Provider extension;
see [the Mac build instructions](FileClipboardMac.md).

On Hyprland, install `wl-clipboard` on both Linux computers. Deskflow uses its
asynchronous fallback for an InputCapture server or a RemoteDesktop client when
the session has no Clipboard portal support. An input-only portal bridge does
not provide clipboard access itself; the receiving Deskflow build must include
the RemoteDesktop fallback too. Ordinary text copy/paste uses TCP 24800 and
does not require the file-sharing checkbox or TCP 24801.

## Firewall setup

Files need incoming TCP **24801** on the computer they are copied from. Run setup
on both computers for copying in both directions. The usual mouse/keyboard
connection still uses TCP 24800.

On Linux, choose the local network address and select the connected computers
that may fetch files. An offline computer can be added by its IPv4 address.
**Allow file connections** asks for administrator authorization and adds
persistent rules using UFW or firewalld. UFW rules are limited to the selected
interface, local address, remote address, and TCP 24801. Firewalld uses the
matching source/interface zone and a rule limited to both addresses and TCP
24801, saved separately to the permanent and runtime configuration. Setup does
not enable an inactive firewall or reload unrelated rules. Unsupported firewall
setups display manual instructions. Review the results: denied authorization or
a failed command does not count as a successful setup.

On macOS, setup provides **Open System Settings** and instructions for allowing
Deskflow (`deskflow-core`) in Network → Firewall → Options. The macOS application
firewall controls access by application, rather than by selected IP address.

**Check connection** tests this computer's access to the selected computers on
TCP 24801. Copy a file on the other computer first to start its file service.
For the opposite direction, run the check on the other computer. A reachable
port confirms TCP connectivity only; it does not verify certificates, the Mac
companion, or a complete file transfer. Finish by copying and pasting a file.

Opening setup does not change firewall rules until **Allow file connections**
is pressed. Rules remain after disabling file sharing and must be removed in
the firewall if no longer needed. Run setup again after an address or network
change; old address-specific rules are not automatically migrated or removed.

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
  The signed Mac build has passed native File Provider tests with a local TLS
  sender, including 1 GiB, cached reads, and changed-source rejection. Paired
  Linux/Mac clipboard publication, Finder Paste, and user cancellation still
  require acceptance testing.

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
`RemoteDesktopClipboardTests` uses a private D-Bus session and a simulated portal
to exercise client startup, incoming publication, outgoing clipboard changes,
disabled sharing, and slow helpers through `EiComputer`. It also checks that a
session with native clipboard support continues using the portal. These tests
require `dbus-daemon` and Python 3 and never access the real desktop clipboard.

`FileSharingDialogTests` covers offering setup on save, cancelling Settings,
existing installations, and connected-computer selection. On Linux,
`FileSharingFirewallTests` runs fake firewall/authorization commands to check
address scoping, inactive firewalls, denied authorization, partial failures,
cancellation, and source-zone selection. Its connection checks use real local
TCP sockets without sending file data. These tests never modify the host's
firewall. `SocketPeerAddressTests` checks accepted-peer address discovery and
preservation through the stream wrapper using a local TCP connection.

```sh
DESKFLOW_TEST_GIB=1 QT_QPA_PLATFORM=minimal ctest --test-dir build/src/unittests --output-on-failure
```

The integrated Mac Objective-C++ adapter, Swift companion, and receiver helper
have been built and signed. Native File Provider reads passed against the real
C++ TLS sender, including a verified 1 GiB file. The safe Mac test suites passed
with the canonical temporary-path test correction. These local tests did not
publish to the live clipboard or invoke Finder Paste. Keyboard/menu Paste and
user cancellation against the matching Linux build remain paired acceptance
checks; passing local tests alone does not establish that end-to-end behavior.
