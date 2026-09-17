/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/Arch.h"
#include "base/Log.h"

#include <QTemporaryDir>
#include <QTest>

class WaylandClipboardTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void init();
  void readTextPreservesUtf8AndNewlines();
  void nonFileUrisStillCopyText_data();
  void nonFileUrisStillCopyText();
  void writeTextUsesStdin();
  void failedWriteCanBeRetried();
  void imageRoundTrip();
  void oversizedSelectionLeavesCacheIntact();
  void slowReaderDoesNotBlockCaller();
  void slowWriterDoesNotBlockCaller();
  void remoteWriteSupersedesPendingRead();
  void copiedFilesRoundTrip();

private:
  void writeFile(const QString &name, const QByteArray &data);
  QByteArray readFile(const QString &name) const;
  QString pasteCommand() const;
  QString copyCommand() const;
  Arch m_arch;
  Log m_log;
  QTemporaryDir m_dir;
};
