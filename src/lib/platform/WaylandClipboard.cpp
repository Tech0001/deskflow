/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/WaylandClipboard.h"

#include "base/Log.h"
#include "deskflow/Clipboard.h"
#include "platform/FileTransfer.h"
#include "platform/PortalClipboard.h"

#include <QDeadlineTimer>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>

namespace deskflow {
namespace {
class SelectionWatch
{
public:
  QProcess process;
  bool initialChange = false;
  explicit SelectionWatch(const QString &paste)
  {
    process.setStandardErrorFile(QProcess::nullDevice());
    // No clipboard data is printed or persisted. Each selection change runs
    // a fixed printf command, whose output cancels an obsolete file download.
    process.start(paste, {"--watch", "/usr/bin/printf", "changed\\n"});
    process.waitForStarted(500);
    process.waitForReadyRead(200);
    initialChange = process.readAllStandardOutput().count('\n') > 1;
  }
  ~SelectionWatch()
  {
    process.kill();
    process.waitForFinished(500);
  }
  bool changed()
  {
    process.waitForReadyRead(0);
    return initialChange || process.state() == QProcess::NotRunning || !process.readAllStandardOutput().isEmpty();
  }
};

std::optional<QByteArray> runHelper(
    const QString &program, const QStringList &args, const QByteArray &input, qint64 maxOutput,
    const QDeadlineTimer &deadline
)
{
  if (deadline.hasExpired())
    return std::nullopt;

  QProcess process;
  process.setStandardErrorFile(QProcess::nullDevice());
  process.start(program, args);
  if (!process.waitForStarted(static_cast<int>(deadline.remainingTime())))
    return std::nullopt;
  process.write(input);
  process.closeWriteChannel();

  QByteArray output;
  bool complete = false;
  while (!deadline.hasExpired()) {
    // Read one extra byte to reject oversized selections without truncating.
    output += process.read(maxOutput - output.size() + 1);
    if (output.size() > maxOutput)
      break;
    if (process.state() == QProcess::NotRunning) {
      complete = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
      break;
    }
    process.waitForReadyRead(static_cast<int>(deadline.remainingTime()));
  }
  if (process.state() != QProcess::NotRunning) {
    process.kill();
    process.waitForFinished(100);
  }
  if (!complete)
    return std::nullopt;
  return output;
}
} // namespace

WaylandClipboard::WaylandClipboard(
    std::function<void()> changed, QString copyCommand, QString pasteCommand, std::unique_ptr<FileTransfer> files
)
    : m_copyCommand(copyCommand.isEmpty() ? QStandardPaths::findExecutable("wl-copy") : std::move(copyCommand)),
      m_pasteCommand(pasteCommand.isEmpty() ? QStandardPaths::findExecutable("wl-paste") : std::move(pasteCommand)),
      m_changed(std::move(changed)),
      m_files(std::move(files))
{
  if (available())
    m_worker = std::thread([this] { run(); });
}

WaylandClipboard::~WaylandClipboard()
{
  {
    std::scoped_lock lock(m_mutex);
    m_stopping = true;
    ++m_generation;
  }
  m_wake.notify_one();
  if (m_worker.joinable())
    m_worker.join();
}

bool WaylandClipboard::available() const
{
  return !m_copyCommand.isEmpty() && !m_pasteCommand.isEmpty();
}

void WaylandClipboard::requestRead(qint64 maxBytes)
{
  if (!available() || maxBytes <= 0)
    return;
  {
    std::scoped_lock lock(m_mutex);
    m_maxBytes = maxBytes;
    m_readPending = true;
    ++m_generation;
  }
  m_wake.notify_one();
}

bool WaylandClipboard::copyTo(IClipboard *target) const
{
  std::scoped_lock lock(m_mutex);
  if (m_cache.empty())
    return false;
  IClipboard::unmarshall(target, m_cache, 0);
  return true;
}

bool WaylandClipboard::setClipboard(const IClipboard *source, qint64 maxBytes)
{
  if (!available() || maxBytes <= 0)
    return false;
  auto data = IClipboard::marshall(source);
  if (data.size() > static_cast<size_t>(maxBytes))
    return false;
  {
    std::scoped_lock lock(m_mutex);
    if (data == m_cache && m_cacheOnClipboard)
      return true;
    m_cache = data;
    m_cacheOnClipboard = false;
    m_writePending = std::move(data);
    m_maxBytes = maxBytes;
    m_readPending = false;
    ++m_generation;
  }
  m_wake.notify_one();
  return true;
}

void WaylandClipboard::run()
{
  std::unique_lock lock(m_mutex);
  while (true) {
    m_wake.wait(lock, [this] { return m_stopping || m_readPending || m_writePending.has_value(); });
    if (m_stopping)
      return;
    const auto generation = m_generation;
    const auto maxBytes = m_maxBytes;
    auto write = std::move(m_writePending);
    m_writePending.reset();
    if (!write)
      m_readPending = false;
    lock.unlock();

    std::optional<std::string> selection;
    bool published = false;
    if (write) {
      published = writeSelection(*write, maxBytes, generation);
      if (!published)
        LOG_DEBUG("Wayland clipboard fallback could not publish selection");
    } else {
      selection = readSelection(maxBytes, generation);
    }

    lock.lock();
    // A newer remote write or screen transition supersedes an in-flight read.
    if (!m_stopping && generation == m_generation) {
      if (write)
        m_cacheOnClipboard = published;
      if (selection) {
        m_cacheOnClipboard = true;
        if (*selection != m_cache) {
          m_cache = std::move(*selection);
          LOG_DEBUG("Wayland clipboard fallback read local selection, bytes: %zu", m_cache.size());
          m_changed();
        }
      }
    }
  }
}

bool WaylandClipboard::superseded(uint64_t generation) const
{
  std::scoped_lock lock(m_mutex);
  return m_stopping || generation != m_generation;
}

std::optional<std::string> WaylandClipboard::readSelection(qint64 maxBytes, uint64_t generation)
{
  const QDeadlineTimer deadline(kTimeoutMs);
  const auto types = runHelper(m_pasteCommand, {"--list-types"}, {}, 16 * 1024, deadline);
  if (!types)
    return std::nullopt;
  const auto offered = types->split('\n');
  Clipboard clipboard;
  clipboard.open(0);
  clipboard.empty();
  // File-manager selections must be handled before their text/image previews.
  // Never send another computer's absolute paths as ordinary clipboard text.
  if (offered.contains("text/uri-list") || offered.contains("x-special/gnome-copied-files")) {
    const bool gnome = offered.contains("x-special/gnome-copied-files");
    const auto raw = runHelper(
        m_pasteCommand, {"--no-newline", "--type", gnome ? "x-special/gnome-copied-files" : "text/uri-list"}, {},
        FileTransfer::kMaxOfferBytes, deadline
    );
    if (!raw)
      return std::nullopt;
    auto uris = *raw;
    if (gnome) {
      const auto newline = uris.indexOf('\n');
      if (newline < 0 || (uris.left(newline) != "copy" && uris.left(newline) != "cut"))
        return std::nullopt;
      uris.remove(0, newline + 1); // A remote cut is always a copy; never delete source files.
    }
    bool hasFiles = gnome;
    for (const auto &line : uris.split('\n'))
      hasFiles = hasFiles || line.trimmed().toLower().startsWith("file:");
    if (hasFiles) {
      if (!FileTransfer::enabled())
        return std::nullopt;
      if (!m_files)
        m_files = std::make_unique<FileTransfer>();
      const auto offer =
          m_files->offer(FileTransfer::pathsFromUris(uris), [this, generation] { return superseded(generation); });
      if (offer.isEmpty() || offer.size() + 12 > maxBytes)
        return std::nullopt;
      clipboard.add(IClipboard::Format::Files, offer.toStdString());
      clipboard.close();
      return clipboard.marshall();
    }
    // Browsers also offer URI lists for web links and images. Preserve their
    // normal text/image representations when there are no local file URLs.
  }
  QSet<IClipboard::Format> seen;
  for (const auto &entry : PortalClipboard::kSupportedMimes) {
    if (seen.contains(entry.format) || !offered.contains(entry.mime))
      continue;
    const auto bytes = runHelper(m_pasteCommand, {"--no-newline", "--type", entry.mime}, {}, maxBytes, deadline);
    if (!bytes)
      return std::nullopt;
    auto data = PortalClipboard::decodeFormat(entry.format, *bytes);
    if (entry.format == IClipboard::Format::Text) {
      while (data.endsWith('\0'))
        data.chop(1);
      data.replace("\r\n", "\n");
    }
    if (data.size() > maxBytes)
      return std::nullopt;
    if (data.isEmpty() && !bytes->isEmpty())
      continue;
    clipboard.add(entry.format, data.toStdString());
    seen.insert(entry.format);
  }
  clipboard.close();
  if (seen.isEmpty())
    return std::nullopt;
  auto result = clipboard.marshall();
  if (result.size() > static_cast<size_t>(maxBytes))
    return std::nullopt;
  return result;
}

bool WaylandClipboard::writeSelection(const std::string &data, qint64 maxBytes, uint64_t generation)
{
  Clipboard clipboard;
  clipboard.unmarshall(data, 0);
  clipboard.open(0);
  if (clipboard.has(IClipboard::Format::Files)) {
    if (!FileTransfer::enabled())
      return false;
    if (!m_files)
      m_files = std::make_unique<FileTransfer>();
    SelectionWatch watch(m_pasteCommand);
    bool selectionChanged = false;
    const auto obsolete = [&] {
      selectionChanged = selectionChanged || watch.changed();
      return selectionChanged || superseded(generation) || !FileTransfer::enabled();
    };
    const auto paths = m_files->receive(QByteArray::fromStdString(clipboard.get(IClipboard::Format::Files)), obsolete);
    if (paths.isEmpty() || obsolete())
      return false;
    return runHelper(
               m_copyCommand, {"--type", "text/uri-list"}, FileTransfer::urisFromPaths(paths), 0,
               QDeadlineTimer(kTimeoutMs)
    )
        .has_value();
  }
  // wl-copy offers one format; prefer PNG for images, otherwise UTF-8 text.
  for (const auto &entry : PortalClipboard::kSupportedMimes) {
    if (!clipboard.has(entry.format))
      continue;
    const auto bytes =
        PortalClipboard::encodeFormat(entry.format, QByteArray::fromStdString(clipboard.get(entry.format)));
    if (bytes.size() > maxBytes || (entry.format == IClipboard::Format::Bitmap && bytes.isEmpty()))
      return false;
    const QDeadlineTimer deadline(kTimeoutMs);
    // Payload goes over stdin, never through a shell or the command line.
    const bool success = runHelper(m_copyCommand, {"--type", entry.mime}, bytes, 0, deadline).has_value();
    if (success)
      LOG_DEBUG(
          "Wayland clipboard fallback published %s, bytes: %lld", entry.mime, static_cast<long long>(bytes.size())
      );
    return success;
  }
  return false;
}

} // namespace deskflow
