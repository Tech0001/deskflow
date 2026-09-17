/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/IClipboard.h"
#include "platform/FileTransfer.h"

#include <QString>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace deskflow {

// Fallback for compositors without the Clipboard portal. Clipboard helpers
// run on a worker with bounded I/O; input forwarding only accesses the cache.
class WaylandClipboard
{
public:
  explicit WaylandClipboard(
      std::function<void()> changed, QString copyCommand = {}, QString pasteCommand = {},
      std::unique_ptr<FileTransfer> files = {}
  );
  ~WaylandClipboard();

  bool available() const;
  void requestRead(qint64 maxBytes);
  bool copyTo(IClipboard *target) const;
  bool setClipboard(const IClipboard *source, qint64 maxBytes);

  static constexpr int kTimeoutMs = 500;

private:
  void run();
  std::optional<std::string> readSelection(qint64 maxBytes, uint64_t generation);
  bool writeSelection(const std::string &data, qint64 maxBytes, uint64_t generation);
  bool superseded(uint64_t generation) const;

  const QString m_copyCommand;
  const QString m_pasteCommand;
  const std::function<void()> m_changed;
  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  std::thread m_worker;
  bool m_stopping = false;
  bool m_readPending = false;
  std::optional<std::string> m_writePending;
  std::string m_cache;
  bool m_cacheOnClipboard = false;
  qint64 m_maxBytes = 0;
  uint64_t m_generation = 0;
  std::unique_ptr<FileTransfer> m_files;
};

} // namespace deskflow
