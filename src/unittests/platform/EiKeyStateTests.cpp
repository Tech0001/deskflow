/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "EiKeyStateTests.h"

#include "base/EventQueue.h"
#include "deskflow/AppUtil.h"
#include "deskflow/KeyTypes.h"
#include "platform/EiKeyState.h"

#include <QByteArray>
#include <QTemporaryFile>

#include <cstdint>

namespace {
class RecordingEventQueue : public EventQueue
{
public:
  void addEvent(Event &&event) override
  {
    types.push_back(event.getType());
    const auto *info = static_cast<IKeyState::KeyInfo *>(event.getData());
    buttons.push_back(info->m_button);
    Event::deleteData(event);
  }

  std::vector<EventTypes> types;
  std::vector<KeyButton> buttons;
};

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

const char TestKeymap[] = R"XKB(xkb_keymap {
xkb_keycodes "test" {
    minimum = 8;
    maximum = 255;
    <LFSH> = 50;
    <NMLK> = 77;
};
xkb_types "test" {
    type "ONE_LEVEL" {
        modifiers = none;
        level_name[Level1] = "Any";
    };
};
xkb_compat "test" {
    interpret Shift_L+AnyOfOrNone(all) {
        action = SetMods(modifiers=Shift);
    };
    interpret Num_Lock+AnyOfOrNone(all) {
        action = LockMods(modifiers=Mod2);
    };
};
xkb_symbols "test" {
    key <LFSH> { [ Shift_L ] };
    key <NMLK> { [ Num_Lock ] };
    modifier_map Shift { <LFSH> };
    modifier_map Mod2 { <NMLK> };
};
};)XKB";

// XKB keycodes for TestKeymap.
constexpr std::uint32_t LeftShiftKeycode = 50;
constexpr std::uint32_t NumLockKeycode = 77;
} // namespace

void EiKeyStateTests::initTestCase()
{
  m_arch.init();
  m_log.setFilter(LogLevel::Level::Verbose);
}

void EiKeyStateTests::clearStaleModifiers_shiftDownAndNumLockOn_shiftClearedAndNumLockPreserved()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  QTemporaryFile keymapFile;
  QVERIFY(keymapFile.open());
  const QByteArray keymapData = QByteArray::fromRawData(TestKeymap, sizeof(TestKeymap) - 1);
  QCOMPARE(keymapFile.write(keymapData), keymapData.size());
  QVERIFY(keymapFile.flush());
  keyState.init(keymapFile.handle(), keymapFile.size());

  keyState.updateXkbState(LeftShiftKeycode, true);
  keyState.updateXkbState(NumLockKeycode, true);
  keyState.updateXkbState(NumLockKeycode, false);

  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) != 0);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) != 0);

  keyState.clearStaleModifiers();

  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) == 0);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) != 0);
}

void EiKeyStateTests::updateXkbState_duplicateModifierDown_singleReleaseClearsModifier()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  keyState.updateXkbState(LeftShiftKeycode, true);
  keyState.updateXkbState(LeftShiftKeycode, true);
  keyState.updateXkbState(LeftShiftKeycode, false);

  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) == 0);
}

void EiKeyStateTests::updateXkbState_tracksPressedKeys()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  keyState.updateXkbState(LeftShiftKeycode, true);
  QVERIFY(keyState.isKeyDown(LeftShiftKeycode));
  keyState.updateXkbState(LeftShiftKeycode, false);
  QVERIFY(!keyState.isKeyDown(LeftShiftKeycode));
}

void EiKeyStateTests::mapKeyFromKeyval_ctrlAltF1_returnsFunctionKey()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  keyState.updateXkbState(37, true); // Control_L
  keyState.updateXkbState(64, true); // Alt_L
  QCOMPARE(keyState.mapKeyFromKeyval(67), kKeyF1);
}

void EiKeyStateTests::mapKeyFromKeyval_shiftedText_preservesCase()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);
  QCOMPARE(keyState.mapKeyFromKeyval(38), KeyID('a'));
  keyState.updateXkbState(LeftShiftKeycode, true);
  QCOMPARE(keyState.mapKeyFromKeyval(38), KeyID('A'));
}

void EiKeyStateTests::updateXkbModifiers_snapshotIsAuthoritative()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  // Standard XKB Mod4 is Super/Command. EIS sends modifiers before the key.
  keyState.updateXkbModifiers(1 << 6, 0, 0, 0);
  keyState.updateXkbState(133, true); // Super_L
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierSuper) != 0);
  keyState.updateXkbModifiers(0, 0, 0, 0);
  keyState.updateXkbState(133, false);
  QCOMPARE(keyState.pollActiveModifiers(), KeyModifierMask(0));

  // A latched modifier must also survive a locally observed key release.
  keyState.updateXkbModifiers(0, 1, 0, 0);
  keyState.updateXkbState(LeftShiftKeycode, false);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) != 0);
}

void EiKeyStateTests::updateKeyState_preservesCompositorModifiersAndPressedKeys()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  keyState.updateXkbModifiers(1, 0, 0, 0);
  keyState.updateXkbState(LeftShiftKeycode, true);
  keyState.updateKeyState();
  QVERIFY(keyState.isKeyDown(LeftShiftKeycode));
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) != 0);
}

void EiKeyStateTests::releasePressedKeys_sendsReleasesAndResetsState()
{
  TestAppUtil appUtil;
  RecordingEventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  keyState.updateXkbModifiers(1 << 6, 0, 0, 0);
  keyState.updateXkbState(133, true);
  keyState.updateXkbState(38, true);
  keyState.releasePressedKeys(&keyState);
  QCOMPARE(eventQueue.types, (std::vector<EventTypes>{EventTypes::KeyStateKeyUp, EventTypes::KeyStateKeyUp}));
  QCOMPARE(eventQueue.buttons, (std::vector<KeyButton>{38, 133}));
  QVERIFY(!keyState.isKeyDown(133));
  QVERIFY(!keyState.isKeyDown(38));
  QCOMPARE(keyState.pollActiveModifiers(), KeyModifierMask(0));

  // Stop, pause and removal may all occur for the same capture session.
  keyState.releasePressedKeys(&keyState);
  QCOMPARE(eventQueue.buttons.size(), size_t(2));

  // The next capture must treat the same physical key as a new press.
  keyState.updateXkbState(133, true);
  keyState.updateXkbState(133, false);
  QCOMPARE(keyState.pollActiveModifiers(), KeyModifierMask(0));
}

QTEST_MAIN(EiKeyStateTests)
