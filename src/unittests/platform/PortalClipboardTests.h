/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/Arch.h"
#include "base/Log.h"

#include <QTest>

class PortalClipboardTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void readPipe_completeSelection();
  void readPipe_stalledWriterRejectsPartialData();
  void readPipe_sizeLimit_data();
  void readPipe_sizeLimit();
  void writePipe_deliversBytesBeforeReturning();
  void writePipe_stalledReaderTimesOut();
  void writePipe_closedReaderFails();
  void pipe_largeSelectionRoundTrip();

private:
  Arch m_arch;
  Log m_log;
};
