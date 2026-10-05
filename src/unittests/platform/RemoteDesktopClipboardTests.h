/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/Arch.h"
#include "base/Log.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

class RemoteDesktopClipboardTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void init();
  void cleanupTestCase();
  void inputOnlyPortalCopiesInBothDirections();
  void nativeClipboardDoesNotUseHelpers();
  void disabledSharing_data();
  void disabledSharing();
  void slowClipboardDoesNotBlockClient();

private:
  void writeFile(const QString &name, const QByteArray &data);
  QByteArray readFile(const QString &name) const;
  Arch m_arch;
  Log m_log;
  QTemporaryDir m_dir;
  QProcess m_bus;
  QByteArray m_oldPath;
};
