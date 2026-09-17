/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QStringList>

#include <functional>
#include <memory>

namespace deskflow {

// File offers travel inside the authenticated clipboard. File bytes use a
// separate TLS connection, pinned to the certificate in that offer. These
// blocking methods belong on clipboard workers, never the input event thread.
class FileTransfer
{
public:
  using Cancelled = std::function<bool()>;
  static constexpr quint16 kPort = 24801;
  static constexpr qint64 kMaxBytes = 16LL * 1024 * 1024 * 1024;
  static constexpr int kMaxEntries = 4096;
  static constexpr int kMaxOfferBytes = 1024 * 1024;

  explicit FileTransfer(
      QString certificate = {}, quint16 port = kPort, QString cacheRoot = {}, QStringList addresses = {}
  );
  ~FileTransfer();
  FileTransfer(const FileTransfer &) = delete;
  FileTransfer &operator=(const FileTransfer &) = delete;

  static bool enabled();
  static QStringList pathsFromUris(const QByteArray &uris);
  static QByteArray urisFromPaths(const QStringList &paths);
  static bool validOffer(const QByteArray &offer);

  QByteArray offer(const QStringList &paths, const Cancelled &cancelled = {});
  QStringList receive(const QByteArray &offer, const Cancelled &cancelled = {});

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace deskflow
