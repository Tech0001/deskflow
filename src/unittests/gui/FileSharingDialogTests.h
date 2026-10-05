/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QTemporaryDir>
#include <QTest>

class FileSharingDialogTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void init();
  void enablingFilesOffersSetupOnSave();
  void cancellingSettingsDoesNotOfferSetup();
  void existingFileSharingDoesNotPromptAgain();
  void connectedPeersAreSelectableAndDeduplicated();

private:
  QTemporaryDir m_dir;
};
