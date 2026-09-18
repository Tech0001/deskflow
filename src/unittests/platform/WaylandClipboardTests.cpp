/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "WaylandClipboardTests.h"

#include "common/Settings.h"
#include "deskflow/Clipboard.h"
#include "platform/FileTransfer.h"
#include "platform/PortalClipboard.h"
#include "platform/WaylandClipboard.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QImage>
#include <QProcess>

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
  Settings::setSettingsFile(m_dir.filePath("settings.conf"));
  writeFile("paste", R"PY(#!/usr/bin/env python3
import pathlib, sys, time
root = pathlib.Path(__file__).parent
if '--list-types' in sys.argv:
    sys.stdout.buffer.write((root / 'types').read_bytes())
elif '--watch' in sys.argv:
    print('changed', flush=True)
    while True:
        time.sleep(0.1)
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
  Settings::setValue(Settings::Security::ShareFiles, false);
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

void WaylandClipboardTests::nonFileUrisStillCopyText_data()
{
  QTest::addColumn<bool>("shareFiles");
  QTest::newRow("file sharing disabled") << false;
  QTest::newRow("file sharing enabled") << true;
}

void WaylandClipboardTests::nonFileUrisStillCopyText()
{
  QFETCH(bool, shareFiles);
  Settings::setValue(Settings::Security::ShareFiles, shareFiles);
  Settings::setValue(Settings::Security::TlsEnabled, true);
  Settings::setValue(Settings::Security::CheckPeers, true);
  Settings::setValue(Settings::Server::EnableClipboard, true);
  const QByteArray url("https://example.com/copied-link");
  writeFile("types", "text/uri-list\ntext/plain;charset=utf-8\n");
  writeFile("data", url);
  std::atomic<int> changed = 0;
  WaylandClipboard fallback([&] { ++changed; }, copyCommand(), pasteCommand());
  fallback.requestRead(1024);
  QTRY_COMPARE(changed.load(), 1);
  Clipboard clipboard;
  QVERIFY(fallback.copyTo(&clipboard));
  QCOMPARE(getData(clipboard), url);
  QVERIFY(getData(clipboard, IClipboard::Format::Files).isEmpty());
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

void WaylandClipboardTests::copiedFilesRoundTrip()
{
  Settings::setSettingsFile(m_dir.filePath("settings.conf"));
  Settings::setValue(Settings::Security::ShareFiles, true);
  Settings::setValue(Settings::Security::TlsEnabled, true);
  Settings::setValue(Settings::Security::CheckPeers, true);
  Settings::setValue(Settings::Server::EnableClipboard, true);
  QProcess openssl;
  openssl.start(
      "openssl", {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=Clipboard-file-test",
                  "-keyout", m_dir.filePath("key.pem"), "-out", m_dir.filePath("cert.pem")}
  );
  QVERIFY(openssl.waitForFinished(10000));
  QCOMPARE(openssl.exitCode(), 0);
  writeFile("identity.pem", readFile("key.pem") + readFile("cert.pem"));
  Settings::setValue(Settings::Security::Certificate, m_dir.filePath("identity.pem"));
  writeFile("copied file.txt", "real file contents\n");
  const QStringList source{m_dir.filePath("copied file.txt")};
  writeFile("types", "text/uri-list\ntext/plain\n");
  writeFile("data", deskflow::FileTransfer::urisFromPaths(source));
  std::atomic<int> changes = 0;
  {
    auto files = std::make_unique<deskflow::FileTransfer>(
        m_dir.filePath("identity.pem"), 0, m_dir.filePath("reader-cache"), QStringList{"127.0.0.1"}
    );
    WaylandClipboard reader([&] { ++changes; }, copyCommand(), pasteCommand(), std::move(files));
    reader.requestRead(1024 * 1024);
    QTRY_COMPARE(changes.load(), 1);
    Clipboard copied;
    QVERIFY(reader.copyTo(&copied));
    const auto offer = getData(copied, IClipboard::Format::Files);
    QVERIFY(deskflow::FileTransfer::validOffer(offer));
    QTemporaryDir target;
    deskflow::FileTransfer receiver(m_dir.filePath("identity.pem"), 0, target.path(), {"127.0.0.1"});
    const auto received = receiver.receive(offer);
    QCOMPARE(received.size(), 1);
    QFile file(received[0]);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("real file contents\n"));
  }
  {
    deskflow::FileTransfer sender(m_dir.filePath("identity.pem"), 0, {}, {"127.0.0.1"});
    const auto offer = sender.offer(source);
    Clipboard remote;
    remote.open(0);
    remote.empty();
    remote.add(IClipboard::Format::Files, offer.toStdString());
    remote.close();
    auto files = std::make_unique<deskflow::FileTransfer>(
        m_dir.filePath("identity.pem"), 0, m_dir.filePath("writer-cache"), QStringList{"127.0.0.1"}
    );
    WaylandClipboard writer([] {}, copyCommand(), pasteCommand(), std::move(files));
    QVERIFY(writer.setClipboard(&remote, 1024 * 1024));
    QTRY_VERIFY_WITH_TIMEOUT(readFile("output").startsWith("file://"), 10000);
    QCOMPARE(readFile("args"), QByteArray("--type\ntext/uri-list"));
    const auto paths = deskflow::FileTransfer::pathsFromUris(readFile("output"));
    QCOMPARE(paths.size(), 1);
    // Publishing and stat/open of the virtual file must not start a download.
    QVERIFY(QDir(m_dir.filePath("writer-cache")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    QCOMPARE(QFileInfo(paths[0]).size(), 19);
    QFile file(paths[0]);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(QDir(m_dir.filePath("writer-cache")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    QCOMPARE(file.readAll(), QByteArray("real file contents\n"));
    QCOMPARE(QDir(m_dir.filePath("writer-cache")).entryList({"transfer-*"}, QDir::Dirs).size(), 1);
    file.close();
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("real file contents\n"));
    QCOMPARE(QDir(m_dir.filePath("writer-cache")).entryList({"transfer-*"}, QDir::Dirs).size(), 1);
  }
  Settings::setValue(Settings::Security::ShareFiles, false);
}

QTEST_GUILESS_MAIN(WaylandClipboardTests)
