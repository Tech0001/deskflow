/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileTransferTests.h"
#include "common/Settings.h"
#include "platform/FileTransfer.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <sys/stat.h>
#include <utime.h>

using deskflow::FileTransfer;

void FileTransferTests::write(const QString &path, const QByteArray &bytes)
{
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write(bytes), bytes.size());
}

void FileTransferTests::initTestCase()
{
  m_arch.init();
  QVERIFY(m_dir.isValid());
  Settings::setSettingsFile(m_dir.filePath("settings.conf"));
  QProcess openssl;
  openssl.start(
      "openssl", {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=Deskflow-file-test",
                  "-keyout", m_dir.filePath("key.pem"), "-out", m_dir.filePath("cert.pem")}
  );
  QVERIFY(openssl.waitForFinished(10000));
  QCOMPARE(openssl.exitCode(), 0);
  QFile key(m_dir.filePath("key.pem"));
  QFile cert(m_dir.filePath("cert.pem"));
  QVERIFY(key.open(QIODevice::ReadOnly));
  QVERIFY(cert.open(QIODevice::ReadOnly));
  m_certificate = m_dir.filePath("identity.pem");
  write(m_certificate, key.readAll() + cert.readAll());
}

void FileTransferTests::uriRoundTrip()
{
  const QStringList paths{"/tmp/a file.txt", QString::fromUtf8("/tmp/caf\xc3\xa9 #1.png")};
  QCOMPARE(FileTransfer::pathsFromUris(FileTransfer::urisFromPaths(paths)), paths);
  QVERIFY(FileTransfer::pathsFromUris("https://example.com/file\n").isEmpty());
  QVERIFY(FileTransfer::pathsFromUris("file://another-host/etc/passwd\n").isEmpty());
  QVERIFY(FileTransfer::pathsFromUris("file:///tmp/a?query\n").isEmpty());
}

void FileTransferTests::transferFilesAndFolders()
{
  QTemporaryDir source;
  QTemporaryDir target;
  const auto folder = source.filePath(QString::fromUtf8("caf\xc3\xa9 files"));
  QVERIFY(QDir().mkpath(folder + "/empty"));
  const QByteArray data(4 * 1024 * 1024 + 13, '\xa5');
  write(folder + "/a # file.bin", data);
  write(folder + "/zero.txt", {});
  write(source.filePath("single.txt"), "a standalone file\n");
  FileTransfer sender(m_certificate, 0, source.filePath("cache"), {"127.0.0.1"});
  FileTransfer receiver(m_certificate, 0, target.path(), {"127.0.0.1"});
  const QStringList selected{folder, source.filePath("single.txt")};
  const auto offer = sender.offer(selected);
  QVERIFY(FileTransfer::validOffer(offer));
  QCOMPARE(sender.offer(selected), offer);
  const auto received = receiver.receive(offer);
  QCOMPARE(received.size(), 2);
  QFile actual(received[0] + "/a # file.bin");
  QVERIFY(actual.open(QIODevice::ReadOnly));
  QCOMPARE(actual.readAll(), data);
  QVERIFY(QFileInfo(received[0] + "/empty").isDir());
  QCOMPARE(QFileInfo(received[0] + "/zero.txt").size(), 0);
  QCOMPARE(receiver.receive(offer), received);
  QVERIFY(!QFileInfo(received[1]).permission(QFile::ReadOther));
  QVERIFY(QDir(target.path()).entryList({".incoming-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void FileTransferTests::rejectUnsafeOffers()
{
  const auto path = m_dir.filePath("manifest.txt");
  write(path, "manifest test");
  FileTransfer sender(m_certificate, 0, {}, {"127.0.0.1"});
  const auto base = QJsonDocument::fromJson(sender.offer({path})).object();
  QVERIFY(!base.isEmpty());
  for (const auto &pathName :
       {"../escape", "/absolute", "dir/../../escape", "dir\\escape", "C:drive", "missing-parent/file", "bad\nname"}) {
    auto o = base;
    auto entries = o["entries"].toArray();
    auto entry = entries[0].toObject();
    entry["path"] = pathName;
    entries[0] = entry;
    o["entries"] = entries;
    QVERIFY(!FileTransfer::validOffer(QJsonDocument(o).toJson()));
  }
  auto o = base;
  o["version"] = 99;
  QVERIFY(!FileTransfer::validOffer(QJsonDocument(o).toJson()));
  o = base;
  auto entries = o["entries"].toArray();
  auto duplicate = entries[0].toObject();
  duplicate["path"] = duplicate["path"].toString().toUpper();
  entries.append(duplicate);
  o["entries"] = entries;
  QVERIFY(!FileTransfer::validOffer(QJsonDocument(o).toJson()));
  o = base;
  o["addresses"] = QJsonArray{"attacker.example"};
  QVERIFY(!FileTransfer::validOffer(QJsonDocument(o).toJson()));
  o = base;
  o["expires"] = "0";
  QVERIFY(!FileTransfer::validOffer(QJsonDocument(o).toJson()));
}

void FileTransferTests::rejectSymlinksAndSpecialFiles()
{
  QTemporaryDir dir;
  write(dir.filePath("file"), "source");
  QVERIFY(QFile::link(dir.filePath("file"), dir.filePath("link")));
  FileTransfer sender(m_certificate, 0, {}, {"127.0.0.1"});
  QVERIFY(sender.offer({dir.filePath("link")}).isEmpty());
  QVERIFY(sender.offer({dir.path()}).isEmpty());
  const auto fifo = QFile::encodeName(dir.filePath("pipe"));
  QCOMPARE(::mkfifo(fifo.constData(), 0600), 0);
  QVERIFY(sender.offer({dir.filePath("pipe")}).isEmpty());
}

void FileTransferTests::rejectChangedSource()
{
  QTemporaryDir dir;
  QTemporaryDir target;
  write(dir.filePath("changing.txt"), "before");
  FileTransfer sender(m_certificate, 0, {}, {"127.0.0.1"});
  FileTransfer receiver(m_certificate, 0, target.path(), {"127.0.0.1"});
  const auto offer = sender.offer({dir.filePath("changing.txt")});
  write(dir.filePath("changing.txt"), "after!");
  QVERIFY(receiver.receive(offer).isEmpty());
  QVERIFY(QDir(target.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
  QVERIFY(QFile::remove(dir.filePath("changing.txt")));
  QVERIFY(QFile::link(m_certificate, dir.filePath("changing.txt")));
  QVERIFY(receiver.receive(offer).isEmpty());
}

void FileTransferTests::rejectCertificateAndToken()
{
  QTemporaryDir target;
  FileTransfer sender(m_certificate, 0, {}, {"127.0.0.1"});
  FileTransfer receiver(m_certificate, 0, target.path(), {"127.0.0.1"});
  const auto base = QJsonDocument::fromJson(sender.offer({m_dir.filePath("cert.pem")})).object();
  auto offer = base;
  offer["fingerprint"] = QString(64, '0');
  QVERIFY(receiver.receive(QJsonDocument(offer).toJson()).isEmpty());
  offer = base;
  offer["token"] = QString(64, '0');
  QVERIFY(receiver.receive(QJsonDocument(offer).toJson()).isEmpty());
  QVERIFY(QDir(target.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void FileTransferTests::cancellationDiscardsStaging()
{
  QTemporaryDir dir;
  QTemporaryDir target;
  write(dir.filePath("large.bin"), QByteArray(8 * 1024 * 1024, 'x'));
  FileTransfer sender(m_certificate, 0, {}, {"127.0.0.1"});
  FileTransfer receiver(m_certificate, 0, target.path(), {"127.0.0.1"});
  QVERIFY(sender.offer({dir.filePath("large.bin")}, [] { return true; }).isEmpty());
  const auto offer = sender.offer({dir.filePath("large.bin")});
  int polls = 0;
  QVERIFY(receiver.receive(offer, [&] { return ++polls > 30; }).isEmpty());
  QVERIFY(QDir(target.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void FileTransferTests::metadataOnlyOffer()
{
  QTemporaryDir dir;
  const auto path = dir.filePath("large-sparse.bin");
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QVERIFY(file.resize(1024LL * 1024 * 1024));
  file.close();
  struct utimbuf times{1, 100};
  QCOMPARE(::utime(QFile::encodeName(path).constData(), &times), 0);
  FileTransfer sender(m_certificate, 0, {}, {"127.0.0.1"});
  const auto offer = sender.offer({path});
  QVERIFY(FileTransfer::validOffer(offer));
  const auto object = QJsonDocument::fromJson(offer).object();
  QCOMPARE(object["version"].toInt(), 2);
  const auto entry = object["entries"].toArray().first().toObject();
  QVERIFY(!entry.contains("sha256"));
  QCOMPARE(entry["revision"].toString().size(), 64);
  struct stat st{};
  QCOMPARE(::stat(QFile::encodeName(path).constData(), &st), 0);
  QCOMPARE(st.st_atime, 1); // Even a single source read would update this atime.
  QCOMPARE(sender.localPaths(offer), QStringList{QFileInfo(path).canonicalFilePath()});
}

void FileTransferTests::individualFileCancellationRetryAndCache()
{
  QTemporaryDir source, cache;
  const auto path = source.filePath("file.bin");
  const QByteArray data(8 * 1024 * 1024, 'r');
  write(path, data);
  FileTransfer sender(m_certificate, 0, {}, {"127.0.0.1"});
  FileTransfer receiver({}, 0, cache.path(), {}, false);
  const auto offer = sender.offer({path});
  bool cancelled = false;
  QVERIFY(receiver.receiveFile(
                      offer, 0, [&] { return cancelled; }, [&](qint64 done, qint64) { cancelled = done > 0; }
  ).isEmpty());
  QVERIFY(QDir(cache.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
  qint64 received = 0;
  const auto result = receiver.receiveFile(offer, 0, {}, [&](qint64 done, qint64) { received = done; });
  QCOMPARE(received, data.size());
  QVERIFY(!result.isEmpty());
  QFile copied(result);
  QVERIFY(copied.open(QIODevice::ReadOnly));
  QCOMPARE(copied.readAll(), data);
  QVERIFY(QFile::remove(path));
  received = 0;
  QCOMPARE(receiver.receiveFile(offer, 0, {}, [&](qint64 done, qint64) { received = done; }), result);
  QCOMPARE(received, 0); // A cached paste works even after the original disappears.
  QVERIFY(receiver.receiveFile(offer, -1).isEmpty());
  QVERIFY(receiver.receiveFile(offer, 100).isEmpty());
}

void FileTransferTests::sourceChangesDuringTransfer()
{
  QTemporaryDir source, cache;
  const auto path = source.filePath("changing.bin");
  write(path, QByteArray(32 * 1024 * 1024, 'a'));
  FileTransfer sender(m_certificate, 0, {}, {"127.0.0.1"});
  FileTransfer receiver({}, 0, cache.path(), {}, false);
  const auto offer = sender.offer({path});
  bool changed = false;
  const auto result = receiver.receiveFile(offer, 0, {}, [&](qint64 done, qint64) {
    if (done == 0 || changed)
      return;
    changed = true;
    QFile original(path);
    QVERIFY(original.open(QIODevice::WriteOnly | QIODevice::Append));
    QCOMPARE(original.write("modified"), 8);
  });
  QVERIFY(changed);
  QVERIFY(result.isEmpty());
  QVERIFY(QDir(cache.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

void FileTransferTests::helperReceivesVerifiedFile()
{
  QTemporaryDir source, cache;
  const auto path = source.filePath("helper.bin");
  const QByteArray data(1024 * 1024 + 17, 'h');
  write(path, data);
  FileTransfer sender(m_certificate, 0, {}, {"127.0.0.1"});
  QProcess helper;
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("XDG_CACHE_HOME", cache.path());
  helper.setProcessEnvironment(environment);
  const auto executable =
      QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../../bin/deskflow-file-transfer");
  helper.start(executable, {"0", cache.path()});
  QVERIFY(helper.waitForStarted());
  auto manifest = QJsonDocument::fromJson(sender.offer({path})).object();
  manifest["padding"] = QString(200000, 'p'); // Exercise input beyond a pipe buffer.
  helper.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
  helper.closeWriteChannel();
  QVERIFY(helper.waitForFinished(10000));
  QCOMPARE(helper.exitCode(), 0);
  QString result;
  for (const auto &line : helper.readAllStandardOutput().split('\n')) {
    const auto object = QJsonDocument::fromJson(line).object();
    if (object["event"] == "complete")
      result = object["path"].toString();
  }
  QFile copied(result);
  QVERIFY(copied.open(QIODevice::ReadOnly));
  QCOMPARE(copied.readAll(), data);
}

QTEST_GUILESS_MAIN(FileTransferTests)
