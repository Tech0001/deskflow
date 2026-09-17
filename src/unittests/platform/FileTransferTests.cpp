/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileTransferTests.h"
#include "platform/FileTransfer.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <sys/stat.h>

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

QTEST_GUILESS_MAIN(FileTransferTests)
