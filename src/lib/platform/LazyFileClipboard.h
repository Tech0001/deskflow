/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "platform/FileTransfer.h"

namespace deskflow {
// A private read-only filesystem. Metadata requests never download contents.
// A read waits for a complete verified cache file, off the input event thread.
class LazyFileClipboard
{
public:
  explicit LazyFileClipboard(FileTransfer &files, QString mountRoot = {});
  ~LazyFileClipboard();
  QStringList publish(const QByteArray &offer);
  QByteArray offerForPaths(const QStringList &paths) const;
  bool available() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
} // namespace deskflow
