/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "arch/Arch.h"
#include "base/Log.h"
#include <QTemporaryDir>
#include <QtTest>

class FileTransferTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void uriRoundTrip();
  void transferFilesAndFolders();
  void rejectUnsafeOffers();
  void rejectSymlinksAndSpecialFiles();
  void rejectChangedSource();
  void rejectCertificateAndToken();
  void cancellationDiscardsStaging();

private:
  void write(const QString &path, const QByteArray &bytes);
  Arch m_arch;
  Log m_log;
  QTemporaryDir m_dir;
  QString m_certificate;
};
