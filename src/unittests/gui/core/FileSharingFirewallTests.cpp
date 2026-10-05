/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileSharingFirewallTests.h"
#include "gui/core/FileSharingFirewall.h"

#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>

using deskflow::gui::FileSharingFirewall;

namespace {
const FileSharingFirewall::Rule first{"192.0.2.21", "192.0.2.10", "eth0"};
const FileSharingFirewall::Rule second{"192.0.2.22", "192.0.2.10", "eth0"};
} // namespace

void FileSharingFirewallTests::initTestCase()
{
  QVERIFY(m_dir.isValid());
  const QByteArray script = R"PY(#!/usr/bin/python3
import json, os, pathlib, sys, time
root = pathlib.Path(__file__).parent
program = pathlib.Path(__file__).name
args = sys.argv[1:]
mode = (root / 'mode').read_text()
with (root / 'calls').open('a') as log:
    log.write(json.dumps([program] + args) + '\n')
if program == 'auth':
    if mode == 'deny':
        print('Authorization was dismissed')
        sys.exit(126)
    os.execv(args[0], args)
if program == 'ufw':
    if args == ['status']:
        print('Status: inactive' if mode == 'inactive' else 'Status: active')
    elif args[0] == 'allow':
        if mode == 'slow':
            time.sleep(5)
        if mode == 'fail':
            print('Cannot update rules')
            sys.exit(1)
        print('Rule added')
    else:
        sys.exit(2)
elif program == 'firewall-cmd':
    if args == ['--state']:
        print('running')
    elif args == ['--get-active-zones']:
        print('home\n  interfaces: eth0')
    elif args[0].startswith('--get-zone-of-interface='):
        print('home')
    elif '--permanent' not in args and mode == 'fail-runtime':
        print('Runtime update failed')
        sys.exit(1)
    else:
        print('success')
)PY";
  for (const auto &name : {"auth", "ufw", "firewall-cmd"}) {
    QFile file(m_dir.filePath(name));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(script), script.size());
    file.close();
    QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
  }
}

void FileSharingFirewallTests::mode(const QByteArray &value)
{
  QFile file(m_dir.filePath("mode"));
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write(value), value.size());
}

QByteArray FileSharingFirewallTests::calls() const
{
  QFile file(m_dir.filePath("calls"));
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return file.readAll();
}

void FileSharingFirewallTests::init()
{
  QFile::remove(m_dir.filePath("calls"));
  mode("");
}

void FileSharingFirewallTests::scopedRules()
{
  const auto args = FileSharingFirewall::ufwArguments(first);
  QCOMPARE(
      args, QStringList(
                {"allow", "in", "on", "eth0", "from", "192.0.2.21", "to", "192.0.2.10", "port", "24801", "proto", "tcp",
                 "comment", "Deskflow copied files"}
            )
  );
  const auto rule = FileSharingFirewall::richRule(first);
  QVERIFY(rule.contains("source address=\"192.0.2.21/32\""));
  QVERIFY(rule.contains("destination address=\"192.0.2.10/32\""));
  QVERIFY(rule.contains("port port=\"24801\" protocol=\"tcp\""));
  QVERIFY(!rule.contains("24800"));
  auto malicious = first;
  malicious.interface = "eth0; touch /tmp/unwanted";
  QVERIFY(FileSharingFirewall::ufwArguments(malicious).isEmpty());
}

void FileSharingFirewallTests::rejectsUnsafeAddresses_data()
{
  QTest::addColumn<QString>("address");
  for (const auto &value :
       {"", "any", "0.0.0.0", "192.0.2.0/24", "::", "::1", "127.0.0.1", "224.0.0.1", "255.255.255.255", "example.com",
        "192.0.2.21; echo bad", "$(touch /tmp/bad)", "--force"})
    QTest::newRow(value) << QString::fromLatin1(value);
}

void FileSharingFirewallTests::rejectsUnsafeAddresses()
{
  QFETCH(QString, address);
  FileSharingFirewall firewall({m_dir.filePath("ufw"), {}, m_dir.filePath("auth")}, nullptr);
  QSignalSpy finished(&firewall, &FileSharingFirewall::finished);
  firewall.apply({{address, first.local, first.interface}});
  QCOMPARE(finished.count(), 1);
  QVERIFY(calls().isEmpty());
}

void FileSharingFirewallTests::ufwAppliesOnlySelectedHosts()
{
  FileSharingFirewall firewall({m_dir.filePath("ufw"), {}, m_dir.filePath("auth")}, nullptr);
  QSignalSpy results(&firewall, &FileSharingFirewall::ruleFinished);
  QSignalSpy finished(&firewall, &FileSharingFirewall::finished);
  firewall.apply({first, second});
  QTRY_COMPARE(finished.count(), 1);
  QCOMPARE(results.count(), 2);
  QVERIFY(results[0][1].toBool());
  QVERIFY(results[1][1].toBool());
  const auto log = calls();
  QCOMPARE(log.count("\"ufw\", \"allow\""), 2);
  QVERIFY(log.contains("\"from\", \"192.0.2.21\", \"to\", \"192.0.2.10\""));
  QVERIFY(log.contains("\"from\", \"192.0.2.22\", \"to\", \"192.0.2.10\""));
  QVERIFY(!log.contains("\"any\""));
  QVERIFY(!log.contains("\"enable\""));
}

void FileSharingFirewallTests::inactiveFirewallIsNotEnabled()
{
  mode("inactive");
  FileSharingFirewall firewall({m_dir.filePath("ufw"), {}, m_dir.filePath("auth")}, nullptr);
  QSignalSpy finished(&firewall, &FileSharingFirewall::finished);
  QSignalSpy results(&firewall, &FileSharingFirewall::ruleFinished);
  firewall.apply({first});
  QTRY_COMPARE(finished.count(), 1);
  QCOMPARE(results.count(), 0);
  QVERIFY(!calls().contains("\"allow\""));
  QVERIFY(!calls().contains("\"enable\""));
}

void FileSharingFirewallTests::deniedAuthorizationStopsSetup()
{
  mode("deny");
  FileSharingFirewall firewall({m_dir.filePath("ufw"), {}, m_dir.filePath("auth")}, nullptr);
  QSignalSpy finished(&firewall, &FileSharingFirewall::finished);
  QSignalSpy results(&firewall, &FileSharingFirewall::ruleFinished);
  firewall.apply({first, second});
  QTRY_COMPARE(finished.count(), 1);
  QCOMPARE(results.count(), 0);
  QVERIFY(!calls().contains("\"allow\""));
}

void FileSharingFirewallTests::failedRuleStopsBeforeNextPeer()
{
  mode("fail");
  FileSharingFirewall firewall({m_dir.filePath("ufw"), {}, m_dir.filePath("auth")}, nullptr);
  QSignalSpy finished(&firewall, &FileSharingFirewall::finished);
  QSignalSpy results(&firewall, &FileSharingFirewall::ruleFinished);
  firewall.apply({first, second});
  QTRY_COMPARE(finished.count(), 1);
  QCOMPARE(results.count(), 1);
  QVERIFY(!results[0][1].toBool());
  QVERIFY(!calls().contains(second.peer.toUtf8()));
}

void FileSharingFirewallTests::firewalldSavesPermanentAndRuntime()
{
  FileSharingFirewall firewall({{}, m_dir.filePath("firewall-cmd"), m_dir.filePath("auth")}, nullptr);
  QSignalSpy finished(&firewall, &FileSharingFirewall::finished);
  QSignalSpy results(&firewall, &FileSharingFirewall::ruleFinished);
  firewall.apply({first});
  QTRY_COMPARE(finished.count(), 1);
  QCOMPARE(results.count(), 1);
  QVERIFY(results[0][1].toBool());
  QCOMPARE(calls().count("\"firewall-cmd\", \"--permanent\""), 1);
  QCOMPARE(calls().count("\"firewall-cmd\", \"--zone=home\""), 1);
  QVERIFY(!calls().contains("--reload"));
  QVERIFY(!calls().contains("--runtime-to-permanent"));
}

void FileSharingFirewallTests::firewalldReportsPartialFailure()
{
  mode("fail-runtime");
  FileSharingFirewall firewall({{}, m_dir.filePath("firewall-cmd"), m_dir.filePath("auth")}, nullptr);
  QSignalSpy finished(&firewall, &FileSharingFirewall::finished);
  QSignalSpy results(&firewall, &FileSharingFirewall::ruleFinished);
  firewall.apply({first});
  QTRY_COMPARE(finished.count(), 1);
  QCOMPARE(results.count(), 1);
  QVERIFY(!results[0][1].toBool());
  QVERIFY(results[0][2].toString().contains("saved for reboot"));
}

void FileSharingFirewallTests::inactiveUfwFallsBackToFirewalld()
{
  mode("inactive");
  FileSharingFirewall firewall(
      {m_dir.filePath("ufw"), m_dir.filePath("firewall-cmd"), m_dir.filePath("auth")}, nullptr
  );
  QSignalSpy finished(&firewall, &FileSharingFirewall::finished);
  QSignalSpy results(&firewall, &FileSharingFirewall::ruleFinished);
  firewall.apply({first});
  QTRY_COMPARE(finished.count(), 1);
  QCOMPARE(results.count(), 1);
  QVERIFY(results[0][1].toBool());
  QVERIFY(!calls().contains("\"ufw\", \"allow\""));
  QVERIFY(calls().contains("\"--add-rich-rule="));
}

void FileSharingFirewallTests::sourceZoneSelection()
{
  const auto zones = QStringLiteral("home\n  interfaces: eth0\nwork\n  sources: 192.0.2.0/24 198.51.100.0/24\n");
  const auto selected = FileSharingFirewall::sourceZone(zones, first.peer);
  QVERIFY(selected);
  QCOMPARE(*selected, QString("work"));
  QCOMPARE(*FileSharingFirewall::sourceZone(zones, "203.0.113.5"), QString());
  QVERIFY(!FileSharingFirewall::sourceZone(zones + "other\n  sources: 192.0.2.21\n", first.peer));
  QVERIFY(!FileSharingFirewall::sourceZone("work\n  sources: ipset:trusted\n", first.peer));
}

void FileSharingFirewallTests::cancelDoesNotContinue()
{
  mode("slow");
  FileSharingFirewall firewall({m_dir.filePath("ufw"), {}, m_dir.filePath("auth")}, nullptr);
  QSignalSpy finished(&firewall, &FileSharingFirewall::finished);
  QSignalSpy results(&firewall, &FileSharingFirewall::ruleFinished);
  firewall.apply({first, second});
  QTRY_VERIFY(calls().contains("\"ufw\", \"allow\""));
  firewall.cancel();
  QTRY_COMPARE(finished.count(), 1);
  QCOMPARE(results.count(), 0);
  QVERIFY(!calls().contains(second.peer.toUtf8()));
  QVERIFY(!firewall.busy());
}

void FileSharingFirewallTests::reachablePort()
{
  QTcpServer server;
  QVERIFY(server.listen(QHostAddress::LocalHost, 0));
  deskflow::gui::FileSharingConnectionCheck check;
  QSignalSpy result(&check, &deskflow::gui::FileSharingConnectionCheck::finished);
  check.start(QHostAddress::LocalHost, QHostAddress::LocalHost, server.serverPort());
  QTRY_COMPARE(result.count(), 1);
  QVERIFY(result[0][0].toBool());
  QTRY_VERIFY(server.hasPendingConnections());
  auto client = server.nextPendingConnection();
  QCOMPARE(client->bytesAvailable(), 0); // The probe never requests file content.
}

void FileSharingFirewallTests::refusedPort()
{
  QTcpServer server;
  QVERIFY(server.listen(QHostAddress::LocalHost, 0));
  const auto port = server.serverPort();
  server.close();
  deskflow::gui::FileSharingConnectionCheck check;
  QSignalSpy result(&check, &deskflow::gui::FileSharingConnectionCheck::finished);
  check.start(QHostAddress::LocalHost, QHostAddress::LocalHost, port);
  QTRY_COMPARE(result.count(), 1);
  QVERIFY(!result[0][0].toBool());
}

void FileSharingFirewallTests::unavailableLocalAddress()
{
  deskflow::gui::FileSharingConnectionCheck check;
  QSignalSpy result(&check, &deskflow::gui::FileSharingConnectionCheck::finished);
  check.start(QHostAddress::LocalHost, QHostAddress("192.0.2.10"));
  QTRY_COMPARE(result.count(), 1);
  QVERIFY(!result[0][0].toBool());
}

QTEST_GUILESS_MAIN(FileSharingFirewallTests)
