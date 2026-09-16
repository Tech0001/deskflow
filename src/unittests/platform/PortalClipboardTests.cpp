/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "PortalClipboardTests.h"

#include "platform/PortalClipboard.h"

#include <QElapsedTimer>

#include <future>
#include <unistd.h>

using deskflow::PortalClipboard;

namespace {
class Pipe
{
public:
  Pipe()
  {
    valid = pipe(fd) == 0;
  }
  ~Pipe()
  {
    closeRead();
    closeWrite();
  }
  void closeRead()
  {
    if (fd[0] >= 0)
      close(fd[0]);
    fd[0] = -1;
  }
  void closeWrite()
  {
    if (fd[1] >= 0)
      close(fd[1]);
    fd[1] = -1;
  }
  int fd[2] = {-1, -1};
  bool valid;
};
} // namespace

void PortalClipboardTests::initTestCase()
{
  m_arch.init();
}

void PortalClipboardTests::readPipe_completeSelection()
{
  Pipe pipe;
  QVERIFY(pipe.valid);
  const QByteArray data("clipboard text");
  QCOMPARE(write(pipe.fd[1], data.constData(), data.size()), data.size());
  pipe.closeWrite();
  QCOMPARE(PortalClipboard::readPipe(pipe.fd[0], 1024), data);
}

void PortalClipboardTests::readPipe_stalledWriterRejectsPartialData()
{
  Pipe pipe;
  QVERIFY(pipe.valid);
  QCOMPARE(write(pipe.fd[1], "partial", 7), ssize_t(7));
  // Keep the writer open without sending more. A blocking buffered read would
  // hang after poll() reports the first seven bytes as ready.
  QElapsedTimer timer;
  timer.start();
  QVERIFY(PortalClipboard::readPipe(pipe.fd[0], 1024).isEmpty());
  QVERIFY(timer.elapsed() < 1500);
}

void PortalClipboardTests::readPipe_sizeLimit_data()
{
  QTest::addColumn<qint64>("limit");
  QTest::addColumn<QByteArray>("expected");
  QTest::newRow("exact limit") << qint64(4) << QByteArray("data");
  QTest::newRow("over limit") << qint64(3) << QByteArray();
  QTest::newRow("disabled") << qint64(0) << QByteArray();
}

void PortalClipboardTests::readPipe_sizeLimit()
{
  QFETCH(qint64, limit);
  QFETCH(QByteArray, expected);
  Pipe pipe;
  QVERIFY(pipe.valid);
  QCOMPARE(write(pipe.fd[1], "data", 4), ssize_t(4));
  pipe.closeWrite();
  QCOMPARE(PortalClipboard::readPipe(pipe.fd[0], limit), expected);
}

void PortalClipboardTests::writePipe_deliversBytesBeforeReturning()
{
  Pipe pipe;
  QVERIFY(pipe.valid);
  const QByteArray data("selection transfer");
  QVERIFY(PortalClipboard::writePipe(pipe.fd[1], data));
  pipe.closeWrite();
  QCOMPARE(PortalClipboard::readPipe(pipe.fd[0], 1024), data);
}

void PortalClipboardTests::writePipe_stalledReaderTimesOut()
{
  Pipe pipe;
  QVERIFY(pipe.valid);
  const QByteArray data(4 * 1024 * 1024, 'x');
  QElapsedTimer timer;
  timer.start();
  QVERIFY(!PortalClipboard::writePipe(pipe.fd[1], data));
  QVERIFY(timer.elapsed() < 1500);
}

void PortalClipboardTests::writePipe_closedReaderFails()
{
  Pipe pipe;
  QVERIFY(pipe.valid);
  pipe.closeRead();
  QVERIFY(!PortalClipboard::writePipe(pipe.fd[1], QByteArray("data")));
}

void PortalClipboardTests::pipe_largeSelectionRoundTrip()
{
  Pipe pipe;
  QVERIFY(pipe.valid);
  const QByteArray data(512 * 1024, 'x');
  auto reader = std::async(std::launch::async, [&] { return PortalClipboard::readPipe(pipe.fd[0], data.size()); });
  const bool written = PortalClipboard::writePipe(pipe.fd[1], data);
  pipe.closeWrite();
  const auto received = reader.get();
  QVERIFY(written);
  QCOMPARE(received, data);
}

QTEST_MAIN(PortalClipboardTests)
