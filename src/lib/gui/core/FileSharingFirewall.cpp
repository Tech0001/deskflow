/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileSharingFirewall.h"

#include <QFileInfo>
#include <QHostAddress>
#include <QProcessEnvironment>
#include <QRegularExpression>

#include <algorithm>

namespace deskflow::gui {
namespace {
#ifdef Q_OS_LINUX
QString systemExecutable(const QString &name)
{
  // Privileged programs must never be resolved from the user's PATH.
  for (const auto &dir : {QStringLiteral("/usr/sbin/"), QStringLiteral("/usr/bin/"), QStringLiteral("/sbin/")}) {
    const QFileInfo file(dir + name);
    if (file.isExecutable() && file.isFile() && file.ownerId() == 0)
      return file.canonicalFilePath();
  }
  return {};
}
#endif
} // namespace

FileSharingConnectionCheck::FileSharingConnectionCheck(QObject *parent) : QObject(parent)
{
  m_timer.setSingleShot(true);
  connect(&m_socket, &QTcpSocket::connected, this, [this] { finish(true, tr("File port reachable")); });
  connect(&m_socket, &QTcpSocket::errorOccurred, this, [this] {
    finish(false, tr("Not reachable: %1").arg(m_socket.errorString()));
  });
  connect(&m_timer, &QTimer::timeout, this, [this] {
    finish(
        false, tr("Timed out. Check the other computer's firewall and copy a file there to start its file service.")
    );
  });
}

void FileSharingConnectionCheck::start(const QHostAddress &peer, const QHostAddress &local, quint16 port, int timeoutMs)
{
  m_timer.start(timeoutMs);
  if (!m_socket.bind(local)) {
    finish(false, tr("Could not use the selected local address."));
    return;
  }
  m_socket.connectToHost(peer, port);
}

void FileSharingConnectionCheck::finish(bool reachable, const QString &description)
{
  if (m_finished)
    return;
  m_finished = true;
  m_timer.stop();
  m_socket.abort();
  Q_EMIT finished(reachable, description);
}

FileSharingFirewall::Programs FileSharingFirewall::systemPrograms()
{
#ifdef Q_OS_LINUX
  return {systemExecutable("ufw"), systemExecutable("firewall-cmd"), systemExecutable("pkexec")};
#else
  return {};
#endif
}

FileSharingFirewall::FileSharingFirewall(QObject *parent) : FileSharingFirewall(systemPrograms(), parent)
{
}

FileSharingFirewall::FileSharingFirewall(Programs programs, QObject *parent)
    : QObject(parent),
      m_programs(std::move(programs))
{
  m_process.setProcessChannelMode(QProcess::MergedChannels);
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("LC_ALL", "C");
  environment.insert("LANG", "C");
  m_process.setProcessEnvironment(environment);
  m_timeout.setSingleShot(true);
  connect(&m_timeout, &QTimer::timeout, this, [this] {
    m_timedOut = true;
    m_output = "Firewall setup timed out. A completed rule may remain; check the firewall before retrying.";
    m_process.kill();
  });
  connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
    const auto bytes = m_process.readAllStandardOutput();
    if (m_output.size() < 65536)
      m_output += bytes.left(65536 - m_output.size());
  });
  connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
    if (m_cancelling) {
      m_cancelling = false;
      finish();
    } else {
      completeProcess(!m_timedOut && status == QProcess::NormalExit && code == 0);
    }
  });
  connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      m_output = m_process.errorString().toUtf8();
      completeProcess(false);
    }
  });
}

FileSharingFirewall::~FileSharingFirewall()
{
  m_completion = {};
  m_process.disconnect(this);
  if (m_process.state() != QProcess::NotRunning) {
    m_process.kill();
    m_process.waitForFinished(1000);
  }
}

QString FileSharingFirewall::ipv4(const QString &text)
{
  const QHostAddress address(text.trimmed());
  if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isNull() || address.isLoopback() ||
      address.isMulticast() || address == QHostAddress::Broadcast || address.toIPv4Address() == 0)
    return {};
  return address.toString();
}

bool FileSharingFirewall::validRule(const Rule &rule)
{
  static const QRegularExpression interfaceName("^[a-zA-Z0-9_][a-zA-Z0-9_.:-]{0,63}$");
  return !ipv4(rule.peer).isEmpty() && !ipv4(rule.local).isEmpty() && ipv4(rule.peer) != ipv4(rule.local) &&
         interfaceName.match(rule.interface).hasMatch();
}

QStringList FileSharingFirewall::ufwArguments(const Rule &rule)
{
  if (!validRule(rule))
    return {};
  return {"allow",          "in",   "on",    rule.interface, "from", ipv4(rule.peer), "to",
          ipv4(rule.local), "port", "24801", "proto",        "tcp",  "comment",       "Deskflow copied files"};
}

QString FileSharingFirewall::richRule(const Rule &rule)
{
  if (!validRule(rule))
    return {};
  return QStringLiteral(
             "rule family=\"ipv4\" source address=\"%1/32\" destination address=\"%2/32\" "
             "port port=\"24801\" protocol=\"tcp\" accept"
  )
      .arg(ipv4(rule.peer), ipv4(rule.local));
}

FileSharingFirewall::Backend FileSharingFirewall::backend() const
{
  if (!m_programs.pkexec.isEmpty()) {
    if (!m_useFirewalld && !m_programs.ufw.isEmpty())
      return Backend::Ufw;
    if (!m_programs.firewalld.isEmpty())
      return Backend::Firewalld;
  }
#ifdef Q_OS_MACOS
  return Backend::SystemSettings;
#endif
  return Backend::Manual;
}

std::optional<QString> FileSharingFirewall::sourceZone(const QString &activeZones, const QString &peer)
{
  QString zone;
  QString match;
  for (const auto &line : activeZones.split('\n')) {
    if (!line.isEmpty() && !line.front().isSpace()) {
      zone = line.trimmed().section(' ', 0, 0);
    } else if (line.trimmed().startsWith("sources:")) {
      const auto sources = line.trimmed().mid(8).split(' ', Qt::SkipEmptyParts);
      for (const auto &source : sources) {
        const auto subnet = QHostAddress::parseSubnet(source);
        if (subnet.first.isNull())
          return std::nullopt; // MAC/ipset source bindings need manual selection.
        if (QHostAddress(peer).isInSubnet(subnet)) {
          if (!match.isEmpty() && match != zone)
            return std::nullopt;
          match = zone;
        }
      }
    }
  }
  return match;
}

void FileSharingFirewall::run(const QString &program, const QStringList &args, bool elevated, Completion completion)
{
  m_output.clear();
  m_timedOut = false;
  m_completion = std::move(completion);
  m_timeout.start(elevated ? 90000 : 10000);
  if (elevated)
    m_process.start(m_programs.pkexec, QStringList{program} + args);
  else
    m_process.start(program, args);
}

void FileSharingFirewall::completeProcess(bool success)
{
  m_timeout.stop();
  if (!m_completion)
    return;
  const auto output = QString::fromUtf8(m_output + m_process.readAllStandardOutput()).trimmed();
  auto callback = std::move(m_completion);
  m_completion = {};
  callback(success, output);
}

void FileSharingFirewall::apply(const QList<Rule> &rules)
{
  if (m_busy)
    return;
  if (rules.isEmpty() || std::any_of(rules.begin(), rules.end(), [](const auto &rule) { return !validRule(rule); })) {
    Q_EMIT statusChanged(tr("Select another computer's IPv4 address and a local network address."));
    Q_EMIT finished();
    return;
  }
  m_rules = rules;
  m_index = 0;
  m_busy = true;
  Q_EMIT statusChanged(tr("Checking the firewall. Your system may ask for administrator authorization."));
  if (backend() == Backend::Ufw) {
    run(m_programs.ufw, {"status"}, true, [this](bool ok, const QString &output) {
      if (ok && output.startsWith("Status: inactive") && !m_programs.firewalld.isEmpty()) {
        m_useFirewalld = true;
        checkFirewalld();
        return;
      }
      if (!ok || !output.startsWith("Status: active")) {
        Q_EMIT statusChanged(
            ok && output.startsWith("Status: inactive")
                ? tr("UFW is inactive; no rule was added and it was not enabled. Use Check connection to test the "
                     "other computer.")
                : tr("Could not check UFW: %1").arg(output)
        );
        finish();
        return;
      }
      nextRule();
    });
  } else if (backend() == Backend::Firewalld) {
    checkFirewalld();
  } else {
    Q_EMIT statusChanged(
        tr("Automatic firewall setup is unavailable. Configure incoming TCP 24801 from the selected computers in your "
           "firewall.")
    );
    finish();
  }
}

void FileSharingFirewall::checkFirewalld()
{
  run(m_programs.firewalld, {"--state"}, false, [this](bool ok, const QString &output) {
    if (!ok || output != "running") {
      Q_EMIT statusChanged(tr("firewalld is not running or could not be checked: %1").arg(output));
      finish();
      return;
    }
    nextRule();
  });
}

void FileSharingFirewall::nextRule()
{
  if (m_index == m_rules.size()) {
    Q_EMIT statusChanged(
        tr("Firewall setup finished. Review each result below. Run Check connection on the other computer to test "
           "access to this one.")
    );
    finish();
    return;
  }
  const auto rule = m_rules.at(m_index);
  Q_EMIT statusChanged(tr("Allowing file connections from %1…").arg(rule.peer));
  if (backend() == Backend::Ufw) {
    run(m_programs.ufw, ufwArguments(rule), true, [this](bool ok, const QString &output) {
      finishRule(ok, ok ? tr("Firewall rule saved") : tr("Rule not confirmed: %1").arg(output));
    });
  } else {
    findFirewalldZone(rule);
  }
}

void FileSharingFirewall::findFirewalldZone(const Rule &rule)
{
  run(m_programs.firewalld, {"--get-active-zones"}, false, [this, rule](bool ok, const QString &output) {
    const auto source = sourceZone(output, rule.peer);
    if (!ok || !source) {
      finishRule(
          false, tr("Could not determine the source zone. Configure this computer's rule in firewalld manually.")
      );
      return;
    }
    if (!source->isEmpty()) {
      addFirewalldRule(*source);
      return;
    }
    run(m_programs.firewalld, {"--get-zone-of-interface=" + rule.interface}, false,
        [this](bool ok, const QString &zone) {
          if (ok && !zone.isEmpty() && zone != "no zone") {
            addFirewalldRule(zone);
          } else {
            run(m_programs.firewalld, {"--get-default-zone"}, false, [this](bool found, const QString &defaultZone) {
              if (found)
                addFirewalldRule(defaultZone);
              else
                finishRule(false, tr("Could not determine the firewall zone: %1").arg(defaultZone));
            });
          }
        });
  });
}

void FileSharingFirewall::addFirewalldRule(const QString &zone)
{
  static const QRegularExpression zoneName("^[a-zA-Z0-9_-]{1,64}$");
  if (!zoneName.match(zone).hasMatch()) {
    finishRule(false, tr("The firewall returned an invalid zone."));
    return;
  }
  const QStringList args{"--zone=" + zone, "--add-rich-rule=" + richRule(m_rules.at(m_index))};
  run(m_programs.firewalld, QStringList{"--permanent"} + args, true, [this, args](bool ok, const QString &output) {
    if (!ok) {
      finishRule(false, tr("Permanent rule not confirmed: %1").arg(output));
      return;
    }
    run(m_programs.firewalld, args, true, [this](bool active, const QString &output) {
      finishRule(
          active, active ? tr("Firewall rule saved and active")
                         : tr("Rule saved for reboot, but could not activate it: %1").arg(output)
      );
    });
  });
}

void FileSharingFirewall::finishRule(bool success, const QString &text)
{
  Q_EMIT ruleFinished(m_rules.at(m_index).peer, success, text);
  if (!success) {
    Q_EMIT statusChanged(tr("Setup stopped. Earlier successful rules remain; retry after resolving the reported error.")
    );
    finish();
    return;
  }
  ++m_index;
  nextRule();
}

void FileSharingFirewall::finish()
{
  m_busy = false;
  Q_EMIT finished();
}

void FileSharingFirewall::cancel()
{
  if (!m_busy)
    return;
  m_completion = {};
  m_timeout.stop();
  Q_EMIT statusChanged(tr("Setup cancelled. Rules already applied may remain. Check the firewall before retrying."));
  if (m_process.state() == QProcess::NotRunning) {
    finish();
  } else {
    m_cancelling = true;
    m_process.kill();
  }
}
} // namespace deskflow::gui
