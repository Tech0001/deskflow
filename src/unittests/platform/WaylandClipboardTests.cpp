/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "WaylandClipboardTests.h"

#include "deskflow/Clipboard.h"
#include "platform/PortalClipboard.h"
#include "platform/WaylandClipboard.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QImage>

#include <atomic>

using deskflow::PortalClipboard;
using deskflow::WaylandClipboard;

namespace {
void setText(Clipboard &clipboard, const QByteArray &text)
{
  clipboard.open(0);
  clipboard.empty();
  clipboard.add(IClipboard::Format::Text, text.toStdString());
  clipboard.close();
}

QByteArray getData(const Clipboard &clipboard, IClipboard::Format format = IClipboard::Format::Text)
{
  clipboard.open(0);
  const auto data = QByteArray::fromStdString(clipboard.get(format));
  clipboard.close();
  return data;
}
} // namespace

void WaylandClipboardTests::writeFile(const QString &name, const QByteArray &data)
{
  QFile file(m_dir.filePath(name));
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write(data), data.size());
}

QByteArray WaylandClipboardTests::readFile(const QString &name) const
{
  QFile file(m_dir.filePath(name));
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return file.readAll();
}

QString WaylandClipboardTests::pasteCommand() const
{
  return m_dir.filePath("paste");
}

QString WaylandClipboardTests::copyCommand() const
{
  return m_dir.filePath("copy");
}

void WaylandClipboardTests::initTestCase()
{
  m_arch.init();
  QVERIFY(m_dir.isValid());
  writeFile("paste", R"PY(#!/usr/bin/env python3
import pathlib, sys, time
root = pathlib.Path(__file__).parent
if '--list-types' in sys.argv:
    sys.stdout.buffer.write((root / 'types').read_bytes())
else:
    mode = (root / 'mode').read_text()
    (root / 'started').touch()
    if mode == 'slow-read':
        time.sleep(5)
    if mode == 'blocked-read':
        while not (root / 'release').exists():
            time.sleep(0.005)
    sys.stdout.buffer.write((root / 'data').read_bytes())
)PY");
  writeFile("copy", R"PY(#!/usr/bin/env python3
import pathlib, sys, time
root = pathlib.Path(__file__).parent
(root / 'started').touch()
if (root / 'mode').read_text() == 'slow-write':
    time.sleep(5)
if (root / 'mode').read_text() == 'fail-write':
    (root / 'failed').touch()
    sys.exit(1)
data = sys.stdin.buffer.read()
(root / 'args').write_text('\n'.join(sys.argv[1:]))
(root / 'output').write_bytes(data)
)PY");
  for (const auto &path : {pasteCommand(), copyCommand()})
    QVERIFY(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
}

void WaylandClipboardTests::init()
{
  writeFile("types", "text/plain;charset=utf-8\ntext/plain\n");
  writeFile("mode", "");
  writeFile("data", "local clipboard");
  for (const auto &name : {"output", "args", "started", "release", "failed"})
    QFile::remove(m_dir.filePath(name));
}

void WaylandClipboardTests::readTextPreservesUtf8AndNewlines()
{
  const QByteArray text("line one\nline two: caf\xc3\xa9\n");
  writeFile("data", text);
  std::atomic<int> changed = 0;
  WaylandClipboard fallback([&] { ++changed; }, copyCommand(), pasteCommand());
  fallback.requestRead(1024);
  QTRY_COMPARE(changed.load(), 1);
  Clipboard clipboard;
  QVERIFY(fallback.copyTo(&clipboard));
  QCOMPARE(getData(clipboard), text);
  // Reading our cached content again must not claim a new clipboard owner.
  fallback.requestRead(1024);
}

void WaylandClipboardTests::writeTextUsesStdin()
{
  const QByteArray text("quotes '\" and $(touch should-not-run)\nsecond line\n");
  Clipboard clipboard;
  setText(clipboard, text);
  WaylandClipboard fallback([] {}, copyCommand(), pasteCommand());
  QVERIFY(fallback.setClipboard(&clipboard, 1024));
  QTRY_COMPARE(readFile("output"), text);
  QCOMPARE(readFile("args"), QByteArray("--type\ntext/plain;charset=utf-8"));
}

void WaylandClipboardTests::failedWriteCanBeRetried()
{
  writeFile("mode", "fail-write");
  Clipboard clipboard;
  setText(clipboard, "retry clipboard");
  WaylandClipboard fallback([] {}, copyCommand(), pasteCommand());
  QVERIFY(fallback.setClipboard(&clipboard, 1024));
  QTRY_VERIFY(QFile::exists(m_dir.filePath("failed")));
  QTest::qWait(100);
  writeFile("mode", "");
  QVERIFY(fallback.setClipboard(&clipboard, 1024));
  QTRY_COMPARE(readFile("output"), QByteArray("retry clipboard"));
}

void WaylandClipboardTests::imageRoundTrip()
{
  QImage original(8, 8, QImage::Format_RGB32);
  original.fill(Qt::red);
  QByteArray png;
  QBuffer buffer(&png);
  QVERIFY(buffer.open(QIODevice::WriteOnly));
  QVERIFY(original.save(&buffer, "PNG"));
  writeFile("types", "image/png\n");
  writeFile("data", png);
  std::atomic<int> changed = 0;
  WaylandClipboard reader([&] { ++changed; }, copyCommand(), pasteCommand());
  reader.requestRead(4096);
  QTRY_COMPARE(changed.load(), 1);
  Clipboard clipboard;
  QVERIFY(reader.copyTo(&clipboard));
  WaylandClipboard writer([] {}, copyCommand(), pasteCommand());
  QVERIFY(writer.setClipboard(&clipboard, 4096));
  QTRY_VERIFY(!readFile("output").isEmpty());
  QCOMPARE(QImage::fromData(readFile("output"), "PNG"), original);
  QCOMPARE(readFile("args"), QByteArray("--type\nimage/png"));
}

void WaylandClipboardTests::oversizedSelectionLeavesCacheIntact()
{
  Clipboard clipboard;
  std::atomic<int> changed = 0;
  {
    WaylandClipboard fallback([&] { ++changed; }, copyCommand(), pasteCommand());
    fallback.requestRead(1024);
    QTRY_COMPARE(changed.load(), 1);
    writeFile("data", QByteArray(2048, 'x'));
    QFile::remove(m_dir.filePath("started"));
    fallback.requestRead(1024);
    QTRY_VERIFY(QFile::exists(m_dir.filePath("started")));
    QTest::qWait(WaylandClipboard::kTimeoutMs + 50);
    QVERIFY(fallback.copyTo(&clipboard));
  }
  QCOMPARE(changed.load(), 1);
  QCOMPARE(getData(clipboard), QByteArray("local clipboard"));
}

void WaylandClipboardTests::slowReaderDoesNotBlockCaller()
{
  writeFile("mode", "slow-read");
  std::atomic<int> changed = 0;
  QElapsedTimer timer;
  timer.start();
  {
    WaylandClipboard fallback([&] { ++changed; }, copyCommand(), pasteCommand());
    fallback.requestRead(1024);
    QVERIFY(timer.elapsed() < 200);
    QTRY_VERIFY(QFile::exists(m_dir.filePath("started")));
  }
  QVERIFY(timer.elapsed() < 2000);
  QCOMPARE(changed.load(), 0);
}

void WaylandClipboardTests::slowWriterDoesNotBlockCaller()
{
  writeFile("mode", "slow-write");
  Clipboard clipboard;
  setText(clipboard, QByteArray(512 * 1024, 'x'));
  QElapsedTimer timer;
  timer.start();
  {
    WaylandClipboard fallback([] {}, copyCommand(), pasteCommand());
    QVERIFY(fallback.setClipboard(&clipboard, 1024 * 1024));
    QVERIFY(timer.elapsed() < 200);
    QTRY_VERIFY(QFile::exists(m_dir.filePath("started")));
  }
  QVERIFY(timer.elapsed() < 2000);
  QVERIFY(!QFile::exists(m_dir.filePath("output")));
}

void WaylandClipboardTests::remoteWriteSupersedesPendingRead()
{
  writeFile("mode", "blocked-read");
  std::atomic<int> changed = 0;
  WaylandClipboard fallback([&] { ++changed; }, copyCommand(), pasteCommand());
  fallback.requestRead(1024);
  QTRY_VERIFY(QFile::exists(m_dir.filePath("started")));
  Clipboard remote;
  setText(remote, "new remote clipboard");
  QVERIFY(fallback.setClipboard(&remote, 1024));
  writeFile("release", "");
  QTRY_COMPARE(readFile("output"), QByteArray("new remote clipboard"));
  Clipboard cached;
  QVERIFY(fallback.copyTo(&cached));
  QCOMPARE(getData(cached), QByteArray("new remote clipboard"));
  QCOMPARE(changed.load(), 0);
}

QTEST_GUILESS_MAIN(WaylandClipboardTests)
