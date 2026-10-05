/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "gui/core/FileSharingFirewall.h"

#include <QDialog>
#include <QJsonArray>
#include <QPointer>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace deskflow::gui {
class FileSharingDialog : public QDialog
{
  Q_OBJECT
public:
  FileSharingDialog(QWidget *parent, const QJsonArray &peers);
  ~FileSharingDialog() override;

protected:
  void reject() override;

private:
  void addPeer(const QString &name, const QString &address);
  QList<FileSharingFirewall::Rule> selectedRules() const;
  void apply();
  void checkConnections();
  void updateButtons();
  void setResult(const QString &peer, int column, const QString &text);

  FileSharingFirewall m_firewall;
  QTableWidget *m_peers;
  QComboBox *m_local;
  QLineEdit *m_address;
  QLabel *m_status;
  QPushButton *m_allow;
  QPushButton *m_check;
  QPushButton *m_add;
  QPushButton *m_close;
  QList<QPointer<FileSharingConnectionCheck>> m_probes;
};
} // namespace deskflow::gui
