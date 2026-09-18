/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "arch/Arch.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "platform/LazyFileClipboard.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <atomic>

using namespace deskflow;

class LazyFileClipboardTests : public QObject
{
  Q_OBJECT
  Arch arch;
  Log log;
  QTemporaryDir fixture;
  QString certificate;
  void write(const QString &path, const QByteArray &bytes)
  {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
  }

private Q_SLOTS:
  void initTestCase()
  {
    arch.init();
    Settings::setSettingsFile(fixture.filePath("settings.conf"));
    Settings::setValue(Settings::Security::ShareFiles, true);
    Settings::setValue(Settings::Security::TlsEnabled, true);
    Settings::setValue(Settings::Security::CheckPeers, true);
    Settings::setValue(Settings::Server::EnableClipboard, true);
    QProcess openssl;
    openssl.start(
        "openssl", {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=Lazy-files", "-keyout",
                    fixture.filePath("key.pem"), "-out", fixture.filePath("cert.pem")}
    );
    QVERIFY(openssl.waitForFinished(10000));
    QCOMPARE(openssl.exitCode(), 0);
    QFile key(fixture.filePath("key.pem")), cert(fixture.filePath("cert.pem"));
    QVERIFY(key.open(QIODevice::ReadOnly));
    QVERIFY(cert.open(QIODevice::ReadOnly));
    certificate = fixture.filePath("identity.pem");
    write(certificate, key.readAll() + cert.readAll());
  }

  void cleanup()
  {
    FileTransfer::setProgressHandler({});
  }

  void metadataFoldersAndReads()
  {
    QTemporaryDir source, cache, mount;
    const auto folder = source.filePath("folder");
    QVERIFY(QDir().mkpath(folder + "/empty"));
    QStringList names;
    for (int i = 0; i < 350; ++i) {
      const auto name = QString::number(i) + QString(120, 'a') + ".txt";
      names.append(name);
      write(folder + '/' + name, QByteArray::number(i));
    }
    FileTransfer sender(certificate, 0, {}, {"127.0.0.1"});
    FileTransfer receiver({}, 0, cache.path(), {}, false);
    LazyFileClipboard lazy(receiver, mount.filePath("mount"));
    if (!lazy.available())
      QSKIP("FUSE mount unavailable in this environment");
    const auto offer = sender.offer({folder});
    const auto paths = lazy.publish(offer);
    QCOMPARE(paths.size(), 1);
    QCOMPARE(lazy.offerForPaths(paths), offer);
    QCOMPARE(QDir(paths[0]).entryList(QDir::Files).size(), 350);
    QVERIFY(QFileInfo(paths[0] + "/empty").isDir());
    QVERIFY(QDir(cache.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    QFile file(paths[0] + '/' + names[233]);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(QDir(cache.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    QCOMPARE(file.readAll(), QByteArray("233"));
    QCOMPARE(QDir(cache.path()).entryList({"transfer-*"}, QDir::Dirs).size(), 1);
    QProcess copy;
    const auto destination = source.filePath("native-copy.txt");
    copy.start("cp", {paths[0] + '/' + names[233], destination});
    QVERIFY(copy.waitForFinished(10000));
    QCOMPARE(copy.exitCode(), 0);
    QVERIFY(QFileInfo(destination).permission(QFile::WriteOwner));
    // A newer clipboard offer must not change an already accepted read.
    write(source.filePath("new.txt"), "new selection");
    QVERIFY(!lazy.publish(sender.offer({source.filePath("new.txt")})).isEmpty());
    QVERIFY(file.seek(0));
    QCOMPARE(file.readAll(), QByteArray("233"));
  }

  void cancelAndRetry()
  {
    QTemporaryDir source, cache, mount;
    const QByteArray data(2 * 1024 * 1024, 'x');
    write(source.filePath("file"), data);
    FileTransfer sender(certificate, 0, {}, {"127.0.0.1"});
    FileTransfer receiver({}, 0, cache.path(), {}, false);
    LazyFileClipboard lazy(receiver, mount.filePath("mount"));
    if (!lazy.available())
      QSKIP("FUSE mount unavailable in this environment");
    const auto paths = lazy.publish(sender.offer({source.filePath("file")}));
    QCOMPARE(paths.size(), 1);
    std::atomic<bool> first = true;
    FileTransfer::setProgressHandler([&](const QString &id, const QString &, qint64, qint64, const QString &state) {
      if (state == "receiving" && first.exchange(false))
        FileTransfer::cancelTransfer(id);
    });
    QFile file(paths[0]);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.readAll().isEmpty());
    QVERIFY(file.error() != QFile::NoError);
    QVERIFY(QDir(cache.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    file.close();
    FileTransfer::setProgressHandler({});
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), data);
  }

  void gigabyteRead()
  {
    if (!qEnvironmentVariableIsSet("DESKFLOW_TEST_GIB"))
      QSKIP("Set DESKFLOW_TEST_GIB=1 for the 1 GiB encrypted filesystem integration test");
    QTemporaryDir source, cache, mount;
    QFile original(source.filePath("gigabyte.bin"));
    QVERIFY(original.open(QIODevice::WriteOnly));
    constexpr qint64 total = 1024LL * 1024 * 1024;
    QVERIFY(original.resize(total));
    original.close();
    FileTransfer sender(certificate, 0, {}, {"127.0.0.1"});
    FileTransfer receiver({}, 0, cache.path(), {}, false);
    LazyFileClipboard lazy(receiver, mount.filePath("mount"));
    QVERIFY(lazy.available());
    const auto paths = lazy.publish(sender.offer({original.fileName()}));
    QCOMPARE(paths.size(), 1);
    QCOMPARE(QFileInfo(paths[0]).size(), total);
    QVERIFY(QDir(cache.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    QFile file(paths[0]);
    QVERIFY(file.open(QIODevice::ReadOnly));
    qint64 read = 0;
    while (read < total) {
      const auto bytes = file.read(4 * 1024 * 1024);
      QVERIFY(!bytes.isEmpty());
      QVERIFY(std::all_of(bytes.cbegin(), bytes.cend(), [](char c) { return c == 0; }));
      read += bytes.size();
    }
    QCOMPARE(read, total);
    QCOMPARE(QDir(cache.path()).entryList({"transfer-*"}, QDir::Dirs).size(), 1);
  }
};

QTEST_GUILESS_MAIN(LazyFileClipboardTests)
#include "LazyFileClipboardTests.moc"
