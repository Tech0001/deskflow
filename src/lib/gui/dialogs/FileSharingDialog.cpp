/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileSharingDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkInterface>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace deskflow::gui {
FileSharingDialog::FileSharingDialog(QWidget *parent, const QJsonArray &peers) : QDialog(parent), m_firewall(this)
{
  setWindowTitle(tr("Set up file sharing"));
  setObjectName("fileSharingDialog");
  resize(860, 470);
  auto layout = new QVBoxLayout(this);
  auto intro = new QLabel(
      tr("Allow the selected computers to fetch copied files from this computer. "
         "Run this setup on both computers to copy files in both directions."),
      this
  );
  intro->setWordWrap(true);
  layout->addWidget(intro);

  auto network = new QHBoxLayout;
  network->addWidget(new QLabel(tr("Local network address:"), this));
  m_local = new QComboBox(this);
  m_local->setObjectName("fileSharingLocalAddress");
  for (const auto &interface : QNetworkInterface::allInterfaces()) {
    if (!(interface.flags() & QNetworkInterface::IsUp) || (interface.flags() & QNetworkInterface::IsLoopBack))
      continue;
    for (const auto &entry : interface.addressEntries()) {
      const auto ip = FileSharingFirewall::ipv4(entry.ip().toString());
      if (!ip.isEmpty()) {
        m_local->addItem(ip + " (" + interface.humanReadableName() + ")", QStringList{ip, interface.name()});
        if (!peers.isEmpty() &&
            QHostAddress(peers.first().toObject()["address"].toString()).isInSubnet(entry.ip(), entry.prefixLength()))
          m_local->setCurrentIndex(m_local->count() - 1);
      }
    }
  }
  network->addWidget(m_local, 1);
  layout->addLayout(network);

  m_peers = new QTableWidget(0, 4, this);
  m_peers->setObjectName("fileSharingPeers");
  m_peers->setHorizontalHeaderLabels({tr("Computer"), tr("Address"), tr("Local firewall"), tr("Connection to computer")}
  );
  m_peers->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_peers->verticalHeader()->hide();
  m_peers->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  m_peers->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
  m_peers->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
  layout->addWidget(m_peers, 1);
  for (const auto &peer : peers)
    addPeer(peer.toObject()["name"].toString(), peer.toObject()["address"].toString());

  auto add = new QHBoxLayout;
  m_address = new QLineEdit(this);
  m_address->setObjectName("fileSharingPeerAddress");
  m_address->setPlaceholderText(tr("Another computer's IPv4 address"));
  m_add = new QPushButton(tr("Add computer"), this);
  add->addWidget(m_address, 1);
  add->addWidget(m_add);
  layout->addLayout(add);

  m_status = new QLabel(this);
  m_status->setObjectName("fileSharingStatus");
  m_status->setTextFormat(Qt::PlainText);
  m_status->setWordWrap(true);
  m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(m_status);

  auto explanation = new QLabel(
      tr("Files use TCP 24801. Text clipboard sharing uses the existing Deskflow connection. "
         "Firewall rules remain until removed in your firewall. To test receiving, copy a file on the other computer "
         "first."),
      this
  );
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  auto buttons = new QDialogButtonBox(this);
  m_allow = buttons->addButton(tr("Allow file connections"), QDialogButtonBox::ActionRole);
  m_allow->setObjectName("allowFileConnections");
  m_check = buttons->addButton(tr("Check connection"), QDialogButtonBox::ActionRole);
  m_check->setObjectName("checkFileConnection");
  m_close = buttons->addButton(QDialogButtonBox::Close);
  layout->addWidget(buttons);

  using enum FileSharingFirewall::Backend;
  if (m_firewall.backend() == SystemSettings) {
    m_status->setText(tr(
        "macOS controls incoming connections by application. In System Settings → Network → Firewall → "
        "Options, allow Deskflow (deskflow-core). Receiving files also requires the signed Deskflow Files companion."
    ));
    m_allow->setText(tr("Open System Settings"));
    connect(m_allow, &QPushButton::clicked, this, [] {
      QProcess::startDetached("/usr/bin/open", {"-b", "com.apple.systempreferences"});
    });
  } else if (m_firewall.backend() == Manual) {
    m_status->setText(
        tr("Automatic setup requires UFW or firewalld and an administrator authorization service. "
           "In your firewall, allow incoming TCP 24801 from each selected computer to this computer's local address.")
    );
    m_allow->hide();
  } else {
    m_status->setText(
        tr("Choose the computers and local network address, then allow file connections. "
           "Your system will ask for administrator authorization if needed.")
    );
    connect(m_allow, &QPushButton::clicked, this, &FileSharingDialog::apply);
  }
  connect(m_check, &QPushButton::clicked, this, &FileSharingDialog::checkConnections);
  connect(m_close, &QPushButton::clicked, this, &FileSharingDialog::reject);
  connect(m_add, &QPushButton::clicked, this, [this] {
    const auto ip = FileSharingFirewall::ipv4(m_address->text());
    if (ip.isEmpty()) {
      m_status->setText(tr("Enter a single computer's IPv4 address, not a hostname or network range."));
      return;
    }
    addPeer(tr("Added address"), ip);
    m_address->clear();
    updateButtons();
  });
  connect(m_local, &QComboBox::currentIndexChanged, this, &FileSharingDialog::updateButtons);
  connect(m_peers, &QTableWidget::itemChanged, this, &FileSharingDialog::updateButtons);
  connect(&m_firewall, &FileSharingFirewall::statusChanged, m_status, &QLabel::setText);
  connect(
      &m_firewall, &FileSharingFirewall::ruleFinished, this,
      [this](const QString &peer, bool, const QString &text) { setResult(peer, 2, text); }
  );
  connect(&m_firewall, &FileSharingFirewall::finished, this, &FileSharingDialog::updateButtons);
  updateButtons();
}

FileSharingDialog::~FileSharingDialog()
{
  for (auto probe : m_probes)
    if (probe) {
      probe->disconnect(this);
      delete probe;
    }
}

void FileSharingDialog::addPeer(const QString &name, const QString &address)
{
  const auto ip = FileSharingFirewall::ipv4(address);
  if (ip.isEmpty())
    return;
  for (int row = 0; row < m_peers->rowCount(); ++row)
    if (m_peers->item(row, 1)->text() == ip)
      return;
  const auto row = m_peers->rowCount();
  m_peers->insertRow(row);
  auto computer = new QTableWidgetItem(name);
  computer->setCheckState(Qt::Checked);
  m_peers->setItem(row, 0, computer);
  m_peers->setItem(row, 1, new QTableWidgetItem(ip));
  m_peers->setItem(row, 2, new QTableWidgetItem(tr("Not checked")));
  m_peers->setItem(row, 3, new QTableWidgetItem(tr("Not checked")));
}

QList<FileSharingFirewall::Rule> FileSharingDialog::selectedRules() const
{
  QList<FileSharingFirewall::Rule> rules;
  const auto local = m_local->currentData().toStringList();
  if (local.size() != 2)
    return rules;
  for (int row = 0; row < m_peers->rowCount(); ++row) {
    if (m_peers->item(row, 0) && m_peers->item(row, 1) && m_peers->item(row, 0)->checkState() == Qt::Checked) {
      FileSharingFirewall::Rule rule{m_peers->item(row, 1)->text(), local[0], local[1]};
      if (!FileSharingFirewall::validRule(rule))
        return {};
      rules.append(rule);
    }
  }
  return rules;
}

void FileSharingDialog::updateButtons()
{
  const bool busy = m_firewall.busy() || !m_probes.isEmpty();
  const bool selected = !selectedRules().isEmpty();
  m_allow->setEnabled(!busy && (selected || m_firewall.backend() == FileSharingFirewall::Backend::SystemSettings));
  m_check->setEnabled(!busy && selected);
  m_add->setEnabled(!busy);
  m_address->setEnabled(!busy);
  m_local->setEnabled(!busy);
  m_peers->setEnabled(!busy);
  m_close->setText(m_firewall.busy() ? tr("Cancel setup") : tr("Close"));
}

void FileSharingDialog::setResult(const QString &peer, int column, const QString &text)
{
  for (int row = 0; row < m_peers->rowCount(); ++row)
    if (m_peers->item(row, 1)->text() == peer) {
      m_peers->item(row, column)->setText(text);
      m_peers->item(row, column)->setToolTip(text);
    }
}

void FileSharingDialog::apply()
{
  const auto rules = selectedRules();
  for (const auto &rule : rules)
    setResult(rule.peer, 2, tr("Waiting"));
  m_firewall.apply(rules);
  updateButtons();
}

void FileSharingDialog::checkConnections()
{
  m_status->setText(
      tr("Checking this computer → selected computers on TCP 24801. "
         "A reachable port does not verify a file transfer. To test the reverse direction, run this check on the other "
         "computer.")
  );
  for (const auto &rule : selectedRules()) {
    setResult(rule.peer, 3, tr("Checking…"));
    auto probe = new FileSharingConnectionCheck(this);
    m_probes.append(probe);
    connect(
        probe, &FileSharingConnectionCheck::finished, this,
        [this, probe, peer = rule.peer](bool, const QString &result) {
          m_probes.removeAll(probe);
          setResult(peer, 3, result);
          probe->deleteLater();
          updateButtons();
        }
    );
    probe->start(QHostAddress(rule.peer), QHostAddress(rule.local));
  }
  updateButtons();
}

void FileSharingDialog::reject()
{
  if (m_firewall.busy()) {
    m_firewall.cancel();
    return;
  }
  QDialog::reject();
}
} // namespace deskflow::gui
