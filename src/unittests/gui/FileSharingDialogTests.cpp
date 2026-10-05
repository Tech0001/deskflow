/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileSharingDialogTests.h"

#include "common/Settings.h"
#include "gui/dialogs/FileSharingDialog.h"
#include "gui/dialogs/SettingsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QJsonObject>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>

void FileSharingDialogTests::initTestCase()
{
  QVERIFY(m_dir.isValid());
  Settings::setSettingsFile(m_dir.filePath("settings.conf"));
  Settings::setStateFile(m_dir.filePath("state.conf"));
}

void FileSharingDialogTests::init()
{
  Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Server);
  Settings::setValue(Settings::Security::TlsEnabled, true);
  Settings::setValue(Settings::Security::CheckPeers, true);
  Settings::setValue(Settings::Security::ShareFiles, false);
}

void FileSharingDialogTests::enablingFilesOffersSetupOnSave()
{
  ServerConfig config;
  SettingsDialog settings(nullptr, config);
  const auto files = settings.findChild<QCheckBox *>("cbShareFiles");
  const auto setup = settings.findChild<QPushButton *>("btnFileSharingSetup");
  QVERIFY(files);
  QVERIFY(setup);
  QVERIFY(!setup->isEnabled());
  files->setChecked(true);
  QVERIFY(setup->isEnabled());
  bool offered = false;
  QTimer closer;
  connect(&closer, &QTimer::timeout, &settings, [&] {
    if (auto dialog = settings.findChild<deskflow::gui::FileSharingDialog *>()) {
      offered = true;
      // Closing setup without pressing Allow must not run a privileged tool.
      dialog->done(QDialog::Rejected);
      closer.stop();
    }
  });
  closer.start(10);
  QTimer::singleShot(3000, &settings, [&] { settings.reject(); });
  const auto buttons = settings.findChild<QDialogButtonBox *>();
  QVERIFY(buttons);
  buttons->button(QDialogButtonBox::Save)->click();
  QVERIFY(offered);
  QVERIFY(Settings::value(Settings::Security::ShareFiles).toBool());
}

void FileSharingDialogTests::cancellingSettingsDoesNotOfferSetup()
{
  ServerConfig config;
  SettingsDialog settings(nullptr, config);
  settings.findChild<QCheckBox *>("cbShareFiles")->setChecked(true);
  settings.reject();
  QVERIFY(!settings.findChild<deskflow::gui::FileSharingDialog *>());
  QVERIFY(!Settings::value(Settings::Security::ShareFiles).toBool());
}

void FileSharingDialogTests::existingFileSharingDoesNotPromptAgain()
{
  Settings::setValue(Settings::Security::ShareFiles, true);
  ServerConfig config;
  SettingsDialog settings(nullptr, config);
  QVERIFY(settings.findChild<QPushButton *>("btnFileSharingSetup")->isEnabled());
  bool offered = false;
  QTimer closer;
  connect(&closer, &QTimer::timeout, &settings, [&] {
    if (auto dialog = settings.findChild<deskflow::gui::FileSharingDialog *>()) {
      offered = true;
      dialog->done(QDialog::Rejected);
    }
  });
  closer.start(10);
  // Change an unrelated preference and save; setup should not reappear.
  auto preventSleep = settings.findChild<QCheckBox *>("cbPreventSleep");
  preventSleep->setChecked(!preventSleep->isChecked());
  settings.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
  QVERIFY(!offered);
}

void FileSharingDialogTests::connectedPeersAreSelectableAndDeduplicated()
{
  const QJsonArray peers{
      QJsonObject{{"name", "Linux computer"}, {"address", "192.0.2.21"}},
      QJsonObject{{"name", "MacBook.local"}, {"address", "192.0.2.22"}},
      QJsonObject{{"name", "Duplicate"}, {"address", "192.0.2.21"}},
      QJsonObject{{"name", "Invalid"}, {"address", "0.0.0.0/0"}}
  };
  deskflow::gui::FileSharingDialog dialog(nullptr, peers);
  auto table = dialog.findChild<QTableWidget *>("fileSharingPeers");
  QVERIFY(table);
  QCOMPARE(table->rowCount(), 2);
  QCOMPARE(table->item(0, 0)->text(), QString("Linux computer"));
  QCOMPARE(table->item(0, 0)->checkState(), Qt::Checked);
  table->item(0, 0)->setCheckState(Qt::Unchecked);
  QCOMPARE(table->item(0, 0)->checkState(), Qt::Unchecked);
  if (const auto preview = qgetenv("DESKFLOW_TEST_SETUP_PREVIEW"); !preview.isEmpty()) {
    dialog.show();
    QTest::qWait(50);
    QVERIFY(dialog.grab().save(QString::fromUtf8(preview)));
  }
}

QTEST_MAIN(FileSharingDialogTests)
