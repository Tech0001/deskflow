/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QTemporaryDir>
#include <QTest>

class FileSharingFirewallTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void init();
  void scopedRules();
  void rejectsUnsafeAddresses_data();
  void rejectsUnsafeAddresses();
  void ufwAppliesOnlySelectedHosts();
  void inactiveFirewallIsNotEnabled();
  void deniedAuthorizationStopsSetup();
  void failedRuleStopsBeforeNextPeer();
  void firewalldSavesPermanentAndRuntime();
  void firewalldReportsPartialFailure();
  void inactiveUfwFallsBackToFirewalld();
  void sourceZoneSelection();
  void cancelDoesNotContinue();
  void reachablePort();
  void refusedPort();
  void unavailableLocalAddress();

private:
  void mode(const QByteArray &value);
  QByteArray calls() const;
  QTemporaryDir m_dir;
};
