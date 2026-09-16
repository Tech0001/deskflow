/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/Arch.h"
#include "base/Log.h"

#include <QTest>

class EiKeyStateTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase();
  void clearStaleModifiers_shiftDownAndNumLockOn_shiftClearedAndNumLockPreserved();
  void updateXkbState_duplicateModifierDown_singleReleaseClearsModifier();
  void updateXkbState_tracksPressedKeys();
  void mapKeyFromKeyval_ctrlAltF1_returnsFunctionKey();
  void mapKeyFromKeyval_shiftedText_preservesCase();
  void updateXkbModifiers_snapshotIsAuthoritative();
  void updateKeyState_preservesCompositorModifiersAndPressedKeys();
  void releasePressedKeys_sendsReleasesAndResetsState();

private:
  Arch m_arch;
  Log m_log;
};
