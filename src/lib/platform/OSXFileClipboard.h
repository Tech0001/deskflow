/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <functional>
#include <memory>

class IClipboard;

namespace deskflow {

class OSXFileClipboard
{
public:
  explicit OSXFileClipboard(std::function<void()> changed);
  ~OSXFileClipboard();
  // Called on the normal clipboard/event thread. File I/O runs on a worker;
  // all NSPasteboard reads and writes stay on this calling thread.
  bool poll();
  bool getClipboard(IClipboard *target);
  bool setClipboard(const IClipboard *source);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace deskflow
