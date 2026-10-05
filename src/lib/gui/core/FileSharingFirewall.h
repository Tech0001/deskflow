/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <QProcess>
#include <QTcpSocket>
#include <QTimer>
#include <functional>
#include <optional>

namespace deskflow::gui {

class FileSharingConnectionCheck : public QObject
{
  Q_OBJECT
public:
  explicit FileSharingConnectionCheck(QObject *parent = nullptr);
  void start(const QHostAddress &peer, const QHostAddress &local, quint16 port = 24801, int timeoutMs = 4000);
Q_SIGNALS:
  void finished(bool reachable, const QString &description);

private:
  void finish(bool reachable, const QString &description);
  QTcpSocket m_socket;
  QTimer m_timer;
  bool m_finished = false;
};

class FileSharingFirewall : public QObject
{
  Q_OBJECT
public:
  enum class Backend
  {
    Ufw,
    Firewalld,
    SystemSettings,
    Manual
  };
  struct Programs
  {
    QString ufw;
    QString firewalld;
    QString pkexec;
  };
  struct Rule
  {
    QString peer;
    QString local;
    QString interface;
  };

  explicit FileSharingFirewall(QObject *parent = nullptr);
  FileSharingFirewall(Programs programs, QObject *parent);
  ~FileSharingFirewall() override;
  static Programs systemPrograms();
  static QString ipv4(const QString &text);
  static bool validRule(const Rule &rule);
  static QStringList ufwArguments(const Rule &rule);
  static QString richRule(const Rule &rule);
  static std::optional<QString> sourceZone(const QString &activeZones, const QString &peer);
  Backend backend() const;
  void apply(const QList<Rule> &rules);
  void cancel();
  bool busy() const
  {
    return m_busy;
  }

Q_SIGNALS:
  void statusChanged(const QString &text);
  void ruleFinished(const QString &peer, bool success, const QString &text);
  void finished();

private:
  using Completion = std::function<void(bool, const QString &)>;
  void run(const QString &program, const QStringList &args, bool elevated, Completion completion);
  void completeProcess(bool success);
  void nextRule();
  void addFirewalldRule(const QString &zone);
  void checkFirewalld();
  void findFirewalldZone(const Rule &rule);
  void finishRule(bool success, const QString &text);
  void finish();

  Programs m_programs;
  QProcess m_process;
  QTimer m_timeout;
  Completion m_completion;
  QList<Rule> m_rules;
  int m_index = 0;
  bool m_busy = false;
  bool m_cancelling = false;
  bool m_timedOut = false;
  bool m_useFirewalld = false;
  QByteArray m_output;
};
} // namespace deskflow::gui
