/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "RemoteDesktopClipboardTests.h"

#include "base/EventQueue.h"
#include "common/Settings.h"
#include "deskflow/AppUtil.h"
#include "deskflow/Clipboard.h"
#include "platform/EiComputer.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <sys/socket.h>
#include <unistd.h>

namespace {
class TestAppUtil : public AppUtil
{
public:
  int run() override
  {
    return 0;
  }
  std::vector<std::string> getKeyboardLayoutList() override
  {
    return {"en"};
  }
  std::string getCurrentLanguageCode() override
  {
    return "en";
  }
};

class RecordingEventQueue : public EventQueue
{
public:
  void addEvent(Event &&event) override
  {
    if (event.getType() == EventTypes::EIConnected) {
      close(static_cast<IPrimaryComputer::EiConnectInfo *>(event.getData())->m_fd);
      ++connected;
    } else if (event.getType() == EventTypes::ClipboardGrabbed) {
      ++clipboardChanged;
    }
    Event::deleteData(event);
  }

  std::atomic<int> connected = 0;
  std::atomic<int> clipboardChanged = 0;
};

// Exercise the actual libportal startup and EiComputer clipboard entry points
// on a private bus. No real portal, input device or desktop clipboard is used.
class TestPortal : public QDBusVirtualObject
{
public:
  explicit TestPortal(bool clipboard) : clipboardEnabled(clipboard)
  {
  }

  QString introspect(const QString &) const override
  {
    return {};
  }

  bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override
  {
    const auto method = message.member();
    const auto args = message.arguments();
    if (message.interface() == "org.freedesktop.DBus.Properties") {
      if (method == "Get") {
        const auto value = args[1].toString() == "version" ? 2u : 3u;
        return bus.send(message.createReply(QVariant::fromValue(QDBusVariant(value))));
      }
      return bus.send(message.createReply(QVariantMap{{"version", 2u}, {"AvailableDeviceTypes", 3u}}));
    }
    if (method == "CreateSession" || method == "SelectDevices" || method == "Start") {
      const auto options = qdbus_cast<QVariantMap>(args.last());
      auto sender = message.service().mid(1);
      sender.replace('.', '_');
      const auto request =
          "/org/freedesktop/portal/desktop/request/" + sender + "/" + options.value("handle_token").toString();
      QVariantMap results;
      if (method == "CreateSession") {
        ++sessions;
        results.insert(
            "session_handle",
            "/org/freedesktop/portal/desktop/session/" + sender + "/" + options.value("session_handle_token").toString()
        );
      } else if (method == "Start") {
        results.insert("devices", 3u);
        results.insert("clipboard_enabled", clipboardEnabled);
      }
      bus.send(message.createReply(QVariant::fromValue(QDBusObjectPath(request))));
      QTimer::singleShot(0, this, [bus, request, results, destination = message.service()] {
        auto response =
            QDBusMessage::createTargetedSignal(destination, request, "org.freedesktop.portal.Request", "Response");
        response.setArguments({0u, results});
        bus.send(response);
      });
      return true;
    }
    if (method == "ConnectToEIS") {
      int fds[2];
      if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
        return false;
      QDBusUnixFileDescriptor fd;
      fd.giveFileDescriptor(fds[0]);
      const bool sent = bus.send(message.createReply(QVariant::fromValue(fd)));
      close(fds[1]);
      return sent;
    }
    if (method == "SetSelection")
      ++claims;
    return bus.send(message.createReply());
  }

  bool clipboardEnabled;
  std::atomic<int> sessions = 0;
  std::atomic<int> claims = 0;
};

class PortalRegistration
{
public:
  explicit PortalRegistration(TestPortal &portal) : m_portal(portal), m_owner(QThread::currentThread())
  {
    // libportal makes synchronous calls too. Serve them independently of the
    // client's GLib/Qt main context, like the real portal process does.
    portal.moveToThread(&m_thread);
    m_thread.start();
    QMetaObject::invokeMethod(
        &portal,
        [&] {
          registered = QDBusConnection::sessionBus().registerVirtualObject(
              "/org/freedesktop/portal/desktop", &portal, QDBusConnection::SubPath
          );
        },
        Qt::BlockingQueuedConnection
    );
  }
  ~PortalRegistration()
  {
    QMetaObject::invokeMethod(
        &m_portal,
        [&] {
          QDBusConnection::sessionBus().unregisterObject(
              "/org/freedesktop/portal/desktop", QDBusConnection::UnregisterTree
          );
          m_portal.moveToThread(m_owner);
        },
        Qt::BlockingQueuedConnection
    );
    m_thread.quit();
    m_thread.wait();
  }
  bool registered = false;

private:
  TestPortal &m_portal;
  QThread *m_owner;
  QThread m_thread;
};

void setText(Clipboard &clipboard, const QByteArray &text)
{
  clipboard.open(0);
  clipboard.empty();
  clipboard.add(IClipboard::Format::Text, text.toStdString());
  clipboard.close();
}

QByteArray getText(Clipboard &clipboard)
{
  clipboard.open(0);
  auto text = QByteArray::fromStdString(clipboard.get(IClipboard::Format::Text));
  clipboard.close();
  return text;
}
} // namespace

void RemoteDesktopClipboardTests::writeFile(const QString &name, const QByteArray &data)
{
  QFile file(m_dir.filePath(name));
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write(data), data.size());
}

QByteArray RemoteDesktopClipboardTests::readFile(const QString &name) const
{
  QFile file(m_dir.filePath(name));
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return file.readAll();
}

void RemoteDesktopClipboardTests::initTestCase()
{
  m_arch.init();
  QVERIFY(m_dir.isValid());
  m_bus.start("dbus-daemon", {"--session", "--nofork", "--print-address=1"});
  QVERIFY(m_bus.waitForStarted());
  QVERIFY(m_bus.waitForReadyRead());
  const auto address = m_bus.readLine().trimmed();
  QVERIFY(address.startsWith("unix:"));
  qputenv("DBUS_SESSION_BUS_ADDRESS", address);
  QVERIFY(QDBusConnection::sessionBus().registerService("org.freedesktop.portal.Desktop"));

  Settings::setSettingsFile(m_dir.filePath("settings.conf"));
  writeFile("wl-copy", R"PY(#!/usr/bin/python3
import pathlib, sys, time
root = pathlib.Path(__file__).parent
(root / 'started').touch()
if (root / 'slow').exists():
    time.sleep(5)
data = sys.stdin.buffer.read()
(root / 'data').write_bytes(data)
(root / 'output').write_bytes(data)
)PY");
  writeFile("wl-paste", R"PY(#!/usr/bin/python3
import pathlib, sys
root = pathlib.Path(__file__).parent
if '--list-types' in sys.argv:
    print('text/plain;charset=utf-8')
else:
    (root / 'read').touch()
    sys.stdout.buffer.write((root / 'data').read_bytes())
)PY");
  for (const auto &name : {"wl-copy", "wl-paste"})
    QVERIFY(QFile::setPermissions(m_dir.filePath(name), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
  m_oldPath = qgetenv("PATH");
  // Exclude the real wl-clipboard binaries even if a helper is missing.
  qputenv("PATH", m_dir.path().toUtf8());
}

void RemoteDesktopClipboardTests::init()
{
  Settings::setValue(Settings::Server::EnableClipboard, true);
  Settings::setValue(Settings::Security::ShareFiles, false);
  Settings::setValue(Settings::Core::PreventSleep, false);
  Settings::setValue(Settings::Client::XdpRestoreToken, QString());
  Settings::setValue(Settings::Client::XdpClipboardRetried, false);
  for (const auto &name : {"started", "output", "read", "slow"})
    QFile::remove(m_dir.filePath(name));
  writeFile("data", "old local clipboard");
}

void RemoteDesktopClipboardTests::cleanupTestCase()
{
  qputenv("PATH", m_oldPath);
  m_bus.terminate();
  QVERIFY(m_bus.waitForFinished());
}

void RemoteDesktopClipboardTests::inputOnlyPortalCopiesInBothDirections()
{
  TestPortal portal(false);
  PortalRegistration registration(portal);
  QVERIFY(registration.registered);
  TestAppUtil app;
  RecordingEventQueue events;
  deskflow::EiComputer computer(false, &events, true);
  QTRY_COMPARE(events.connected.load(), 1);
  computer.setOptions({kOptionClipboardSharingSize, 1024});

  Clipboard incoming;
  const QByteArray text("from server: caf\xc3\xa9\nsecond line\n");
  setText(incoming, text);
  QVERIFY(computer.setClipboard(kClipboardClipboard, &incoming));
  QTRY_COMPARE(readFile("output"), text);
  QCOMPARE(portal.sessions.load(), 1); // No token retry/restart of working input.
  QCOMPARE(portal.claims.load(), 0);
  // Leaving without a local copy must not bounce the remote selection back.
  computer.checkClipboards();
  QTRY_VERIFY(QFile::exists(m_dir.filePath("read")));
  QTest::qWait(100);
  QCOMPARE(events.clipboardChanged.load(), 0);

  writeFile("data", "fresh client copy");
  computer.checkClipboards();
  QTRY_COMPARE(events.clipboardChanged.load(), 1);
  Clipboard outgoing;
  QVERIFY(computer.getClipboard(kClipboardClipboard, &outgoing));
  QCOMPARE(getText(outgoing), QByteArray("fresh client copy"));
}

void RemoteDesktopClipboardTests::nativeClipboardDoesNotUseHelpers()
{
  TestPortal portal(true);
  PortalRegistration registration(portal);
  QVERIFY(registration.registered);
  TestAppUtil app;
  RecordingEventQueue events;
  deskflow::EiComputer computer(false, &events, true);
  QTRY_COMPARE(events.connected.load(), 1);
  Clipboard incoming;
  setText(incoming, "native portal copy");
  QVERIFY(computer.setClipboard(kClipboardClipboard, &incoming));
  QTRY_COMPARE(portal.claims.load(), 1);
  computer.checkClipboards();
  QVERIFY(!QFile::exists(m_dir.filePath("started")));
  QVERIFY(!QFile::exists(m_dir.filePath("read")));
}

void RemoteDesktopClipboardTests::disabledSharing_data()
{
  QTest::addColumn<bool>("localEnabled");
  QTest::addColumn<uint>("serverEnabled");
  QTest::addColumn<uint>("limit");
  QTest::newRow("local checkbox off") << false << 1u << 1024u;
  QTest::newRow("server disabled") << true << 0u << 1024u;
  QTest::newRow("zero limit") << true << 1u << 0u;
}

void RemoteDesktopClipboardTests::disabledSharing()
{
  QFETCH(bool, localEnabled);
  QFETCH(uint, serverEnabled);
  QFETCH(uint, limit);
  Settings::setValue(Settings::Server::EnableClipboard, localEnabled);
  TestPortal portal(false);
  PortalRegistration registration(portal);
  QVERIFY(registration.registered);
  TestAppUtil app;
  RecordingEventQueue events;
  deskflow::EiComputer computer(false, &events, true);
  QTRY_COMPARE(events.connected.load(), 1);
  computer.setOptions({kOptionClipboardSharing, serverEnabled, kOptionClipboardSharingSize, limit});
  Clipboard incoming;
  setText(incoming, "must not publish");
  QVERIFY(!computer.setClipboard(kClipboardClipboard, &incoming));
  computer.checkClipboards();
  QTest::qWait(100);
  QCOMPARE(events.clipboardChanged.load(), 0);
  QCOMPARE(portal.claims.load(), 0);
  QVERIFY(!QFile::exists(m_dir.filePath("started")));
  QVERIFY(!QFile::exists(m_dir.filePath("read")));

  // A new server configuration must not retain the old disabled flag/limit.
  if (localEnabled) {
    computer.resetOptions();
    QVERIFY(computer.setClipboard(kClipboardClipboard, &incoming));
    QTRY_COMPARE(readFile("output"), QByteArray("must not publish"));
  }
}

void RemoteDesktopClipboardTests::slowClipboardDoesNotBlockClient()
{
  TestPortal portal(false);
  PortalRegistration registration(portal);
  QVERIFY(registration.registered);
  TestAppUtil app;
  RecordingEventQueue events;
  deskflow::EiComputer computer(false, &events, true);
  QTRY_COMPARE(events.connected.load(), 1);
  writeFile("slow", "");
  Clipboard incoming;
  setText(incoming, QByteArray(512 * 1024, 'x'));
  QElapsedTimer timer;
  timer.start();
  QVERIFY(computer.setClipboard(kClipboardClipboard, &incoming));
  QVERIFY(timer.elapsed() < 200);
  QTRY_VERIFY(QFile::exists(m_dir.filePath("started")));
}

QTEST_GUILESS_MAIN(RemoteDesktopClipboardTests)
