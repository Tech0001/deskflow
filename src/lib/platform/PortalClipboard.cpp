/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/PortalClipboard.h"

#include "base/Log.h"
#include "platform/EiClipboard.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <QBuffer>
#include <QByteArrayList>
#include <QDataStream>
#include <QDeadlineTimer>
#include <QImage>
#include <QList>
#include <QPair>
#include <QSet>
#include <QVarLengthArray>
#include <QtEndian>

namespace deskflow {

static constexpr int kBmpSignatureSize = 2;
static constexpr quint32 kBmpFileHeaderSize = 14;
static constexpr quint32 kMinDibHeaderSize = 12;

QByteArray PortalClipboard::formatMimeTypes(const char *const *mimeTypes)
{
  if (!mimeTypes || !mimeTypes[0])
    return QByteArrayLiteral("(none)");

  QByteArrayList parts;
  for (int i = 0; mimeTypes[i]; ++i)
    parts.append(mimeTypes[i]);
  return parts.join(", ");
}

const PortalClipboard::SupportedMime *PortalClipboard::findSupportedMime(const char *mime)
{
  if (!mime)
    return nullptr;

  for (const auto &entry : kSupportedMimes) {
    if (g_strcmp0(mime, entry.mime) == 0)
      return &entry;
  }

  return nullptr;
}

const PortalClipboard::SupportedMime *PortalClipboard::pickSupportedMime(const char *const *available)
{
  if (!available)
    return nullptr;

  for (const auto &entry : kSupportedMimes) {
    if (g_strv_contains(available, entry.mime))
      return &entry;
  }

  return nullptr;
}

QByteArray PortalClipboard::dibToBmp(const QByteArray &dib)
{
  if (dib.size() < static_cast<qint64>(sizeof(quint32)))
    return {};

  quint32 headerSize;
  std::memcpy(&headerSize, dib.constData(), sizeof(headerSize));
  headerSize = qFromLittleEndian(headerSize);
  if (headerSize < kMinDibHeaderSize || headerSize > static_cast<quint32>(dib.size()))
    return {};

  const auto fileSize = static_cast<quint32>(kBmpFileHeaderSize + dib.size());
  const quint32 pixelOffset = kBmpFileHeaderSize + headerSize;

  QByteArray bmp;
  QDataStream ds(&bmp, QIODevice::WriteOnly);
  ds.setByteOrder(QDataStream::LittleEndian);
  ds.writeRawData("BM", kBmpSignatureSize);
  ds << fileSize;
  ds << quint32(0);
  ds << pixelOffset;
  ds.writeRawData(dib.constData(), static_cast<int>(dib.size()));
  return bmp;
}

QByteArray PortalClipboard::bmpToDib(const QByteArray &bmp)
{
  if (bmp.size() < kBmpFileHeaderSize)
    return {};

  return bmp.mid(kBmpFileHeaderSize);
}

QByteArray PortalClipboard::encodeFormat(IClipboard::Format format, const QByteArray &data)
{
  if (data.isEmpty())
    return {};

  if (format == IClipboard::Format::Bitmap) {
    const auto bmpFile = dibToBmp(data);
    if (bmpFile.isEmpty()) {
      LOG_WARN("clipboard bitmap data is malformed");
      return {};
    }

    QImage image;
    if (!image.loadFromData(bmpFile, "BMP")) {
      LOG_WARN("failed to decode clipboard bitmap");
      return {};
    }

    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    if (!image.save(&buf, "PNG")) {
      LOG_WARN("failed to encode clipboard image as png");
      return {};
    }

    return png;
  }
  return data;
}

QByteArray PortalClipboard::decodeFormat(IClipboard::Format format, const QByteArray &bytes)
{
  if (bytes.isEmpty())
    return {};

  if (format == IClipboard::Format::Bitmap) {
    QImage image;
    if (!image.loadFromData(bytes, "PNG")) {
      LOG_WARN("failed to decode clipboard png");
      return {};
    }

    QByteArray bmp;
    QBuffer buf(&bmp);
    buf.open(QIODevice::WriteOnly);
    if (!image.save(&buf, "BMP")) {
      LOG_WARN("failed to encode clipboard image as bmp");
      return {};
    }

    return bmpToDib(bmp);
  }
  return bytes;
}

QByteArray PortalClipboard::readSelectionBytes(XdpSession *session, const char *mime, qint64 maxBytes)
{
  if (maxBytes <= 0)
    return {};
  const int fd = xdp_session_selection_read(session, mime);
  if (fd < 0) {
    LOG_ERR("failed to read clipboard selection: invalid fd");
    return {};
  }

  auto data = readPipe(fd, maxBytes);
  ::close(fd);
  return data;
}

QByteArray PortalClipboard::readPipe(int fd, qint64 maxBytes)
{
  if (maxBytes <= 0)
    return {};
  const int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return {};

  const QDeadlineTimer deadline(kReadTimeoutMs);
  QByteArray contents;
  contents.reserve(std::min<qint64>(maxBytes, kChunkBytes));
  std::array<char, kChunkBytes> buffer;
  while (!deadline.hasExpired()) {
    pollfd pfd{fd, POLLIN, 0};
    const auto ready = poll(&pfd, 1, static_cast<int>(deadline.remainingTime()));
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0 || (pfd.revents & (POLLERR | POLLNVAL)))
      return {};

    // Read one extra byte at the limit to distinguish EOF from an oversized
    // clipboard. POLLHUP can accompany the last readable bytes of a pipe.
    const auto remaining = maxBytes - contents.size();
    const auto size = remaining == 0 ? 1 : std::min<qint64>(buffer.size(), remaining);
    const auto count = ::read(fd, buffer.data(), size);
    if (count == 0)
      return contents;
    if (count < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        continue;
      return {};
    }
    if (count > remaining)
      return {};
    contents.append(buffer.data(), count);
  }
  LOG_WARN("timed out reading clipboard selection");
  return {};
}

bool PortalClipboard::writePipe(int fd, const QByteArray &data)
{
  const int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return false;

  const QDeadlineTimer deadline(kWriteTimeoutMs);
  qint64 written = 0;
  while (written < data.size() && !deadline.hasExpired()) {
    pollfd pfd{fd, POLLOUT, 0};
    const auto ready = poll(&pfd, 1, static_cast<int>(deadline.remainingTime()));
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
      return false;
    const auto count = ::write(fd, data.constData() + written, data.size() - written);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
      continue;
    if (count <= 0)
      return false;
    written += count;
  }
  return written == data.size();
}

void PortalClipboard::claimOwnership(EiClipboard *cache, XdpSession *session)
{
  if (!cache || !session)
    return;

  cache->open(0);
  QVarLengthArray<const char *, std::size(kSupportedMimes) + 1> mimeTypes;
  for (const auto &entry : kSupportedMimes) {
    if (cache->has(entry.format))
      mimeTypes.append(entry.mime);
  }
  cache->close();

  if (mimeTypes.isEmpty()) {
    LOG_DEBUG("clipboard cache empty, nothing to claim");
    return;
  }
  mimeTypes.append(nullptr);

  LOG_DEBUG("claiming clipboard, mimes: %s", formatMimeTypes(mimeTypes.data()).constData());
  xdp_session_set_selection(session, mimeTypes.data());
}

void PortalClipboard::serveSelectionTransfer(EiClipboard *cache, XdpSession *session, const char *mime, uint32_t serial)
{
  LOG_DEBUG("clipboard selection transfer requested, mime: %s, serial: %u", mime, serial);

  const auto *requested = findSupportedMime(mime);
  if (!requested || !cache) {
    LOG_DEBUG("rejecting clipboard selection, unsupported mime: %s", mime);
    xdp_session_selection_write_done(session, serial, false);
    return;
  }

  cache->open(0);
  QByteArray raw;
  const bool hasFormat = cache->has(requested->format);
  if (hasFormat)
    raw = QByteArray::fromStdString(cache->get(requested->format));
  cache->close();

  const auto data = encodeFormat(requested->format, raw);
  if (data.isEmpty()) {
    LOG_DEBUG("clipboard has no data for mime: %s", mime);
    xdp_session_selection_write_done(session, serial, false);
    return;
  }

  const int fd = xdp_session_selection_write(session, serial);
  if (fd < 0) {
    LOG_WARN("failed to open clipboard selection write fd");
    xdp_session_selection_write_done(session, serial, false);
    return;
  }

  const bool success = writePipe(fd, data);
  ::close(fd);
  xdp_session_selection_write_done(session, serial, success);
  if (success)
    LOG_DEBUG("clipboard selection transfer complete, bytes: %lld", static_cast<long long>(data.size()));
  else
    LOG_WARN("clipboard selection transfer failed or timed out");
}

bool PortalClipboard::readSelectionIntoCache(
    EiClipboard *cache, XdpSession *session, const char *const *mimeTypes, qint64 maxBytes
)
{
  if (!cache || !session || !mimeTypes || !mimeTypes[0])
    return false;

  if (!pickSupportedMime(mimeTypes)) {
    LOG_DEBUG("clipboard no supported mime types: %s", formatMimeTypes(mimeTypes).constData());
    return false;
  }

  QList<QPair<IClipboard::Format, QByteArray>> reads;
  QSet<IClipboard::Format> seen;
  for (const auto &entry : kSupportedMimes) {
    if (seen.contains(entry.format))
      continue;
    if (!g_strv_contains(mimeTypes, entry.mime))
      continue;

    auto bytes = readSelectionBytes(session, entry.mime, maxBytes);
    if (bytes.isEmpty()) {
      LOG_DEBUG("clipboard read returned no data for mime: %s", entry.mime);
      continue;
    }

    if (entry.format != IClipboard::Format::Bitmap) {
      while (bytes.endsWith('\0'))
        bytes.chop(1);
      bytes.replace("\r\n", "\n");
    }

    auto data = decodeFormat(entry.format, bytes);
    if (data.isEmpty())
      continue;

    reads.append({entry.format, std::move(data)});
    seen.insert(entry.format);
  }

  if (reads.isEmpty()) {
    LOG_DEBUG("clipboard read produced no data, leaving existing clipboard intact");
    return false;
  }

  cache->open(0);
  cache->empty();
  for (const auto &[format, data] : reads)
    cache->add(format, data.toStdString());
  cache->close();

  LOG_DEBUG("clipboard read local selection, formats: %lld", static_cast<long long>(reads.size()));
  return true;
}

} // namespace deskflow
