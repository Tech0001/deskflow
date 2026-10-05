/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2014 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerConfigTests.h"

#include "common/Settings.h"
#include "server/Config.h"

#include <QSettings>
#include <sstream>

class OnlySystemFilter : public InputFilter::Condition
{
public:
  Condition *clone() const override
  {
    return new OnlySystemFilter();
  }
  std::string format() const override
  {
    return "";
  }

  InputFilter::FilterStatus match(const Event &ev) override
  {
    return ev.getType() == EventTypes::System ? InputFilter::FilterStatus::Activate
                                              : InputFilter::FilterStatus::NoMatch;
  }
};

using namespace deskflow::server;

void ServerConfigTests::initTestCase()
{
  QVERIFY(m_settingsDir.isValid());
  const auto path = m_settingsDir.filePath("Deskflow.conf");
  {
    // Exercise the 1.26 settings migration used by existing external configs.
    QSettings legacy(path, QSettings::IniFormat);
    legacy.setValue("screen_mac.local/name", "mac.local");
    legacy.setValue("screen_linux.local/name", "linux.local");
    legacy.setValue("screen_mac.local/switchCornerSize", 17);
  }
  Settings::setSettingsFile(path);
  Settings::setStateFile(m_settingsDir.filePath("Deskflow.state"));
  QVERIFY(Settings::knownComputers().contains("mac.local"));
  QVERIFY(Settings::knownComputers().contains("linux.local"));
  QCOMPARE(Settings::value(Settings::Computer::SwitchCornerSize.arg("mac.local")).toInt(), 17);
}

void ServerConfigTests::modifierSwap_selectedComputerOnly()
{
  const auto key = Settings::Server::SwapControlSuperScreens;
  Settings::setValue(key, QStringList{"mac.local"});
  Config config(nullptr);
  QVERIFY(config.addComputer("Mac.local"));
  QVERIFY(config.addComputer("linux.local"));
  const auto mac = config.getOptions("Mac.local");
  QVERIFY(mac != nullptr);
  QCOMPARE(mac->at(kOptionModifierMapForControl), OptionValue(kKeyModifierIDSuper));
  QCOMPARE(mac->at(kOptionModifierMapForSuper), OptionValue(kKeyModifierIDControl));
  const auto linux = config.getOptions("linux.local");
  QVERIFY(linux != nullptr);
  QCOMPARE(linux->at(kOptionModifierMapForControl), OptionValue(kKeyModifierIDControl));
  QCOMPARE(linux->at(kOptionModifierMapForSuper), OptionValue(kKeyModifierIDSuper));
  Settings::setValue(key);
}

void ServerConfigTests::modifierSwap_externalConfigAndToggleOff()
{
  const auto key = Settings::Server::SwapControlSuperScreens;
  const auto ctrl = Settings::Computer::ModifierCtrl.arg("mac.local");
  const std::string text = "section: screens\nmac.local:\nctrl = alt\nsuper = super\nshift = shift\nend\n";
  // Upstream now reads modifier mappings from general settings.
  Settings::setValue(ctrl, "alt");
  Settings::setValue(key, QStringList{"mac.local"});
  // Reload settings to exercise persistence across restarts.
  const auto path = Settings::settingsFile();
  Settings::setSettingsFile(m_settingsDir.filePath("other.conf"));
  Settings::setSettingsFile(path);
  QCOMPARE(Settings::value(key).toStringList(), QStringList{"mac.local"});

  Config enabled(nullptr);
  std::istringstream input(text);
  input >> enabled;
  const auto swapped = enabled.getOptions("mac.local");
  QVERIFY(swapped != nullptr);
  QCOMPARE(swapped->at(kOptionModifierMapForControl), OptionValue(kKeyModifierIDSuper));
  QCOMPARE(swapped->at(kOptionModifierMapForSuper), OptionValue(kKeyModifierIDControl));
  QCOMPARE(swapped->at(kOptionModifierMapForShift), OptionValue(kKeyModifierIDShift));

  Settings::setValue(key, QStringList{});
  Config disabled(nullptr);
  std::istringstream original(text);
  original >> disabled;
  const auto restored = disabled.getOptions("mac.local");
  QVERIFY(restored != nullptr);
  QCOMPARE(restored->at(kOptionModifierMapForControl), OptionValue(kKeyModifierIDAlt));
  QCOMPARE(restored->at(kOptionModifierMapForSuper), OptionValue(kKeyModifierIDSuper));
  Settings::setValue(key);
  Settings::setValue(ctrl);
}

void ServerConfigTests::externalConfig_preservesOffsetAndGeneralScreenOptions()
{
  const auto swap = Settings::Server::SwapControlSuperScreens;
  const auto corner = Settings::Computer::SwitchCornerSize.arg("mac.local");
  Settings::setValue(swap, QStringList{"mac.local"});
  Settings::setValue(corner, 17);
  // Legacy screen entries retain their links, while moved options come from
  // general settings. The Mac is 400 pixels higher on two 1440-pixel screens.
  std::istringstream input(R"(
section: screens
  mac.local:
    halfDuplexCapsLock = false
    switchCornerSize = 0
  linux.local:
    switchCornerSize = 0
end
section: links
  mac.local:
    right(27.777778,100) = linux.local(0,72.222222)
  linux.local:
    left(0,72.222222) = mac.local(27.777778,100)
end
)");
  Config config(nullptr);
  input >> config;
  float destination = 0;
  QCOMPARE(config.getNeighbor("mac.local", Direction::Right, 0.5f, &destination), std::string("linux.local"));
  QVERIFY(qAbs(destination - 0.22222222f) < 0.00001f);
  QCOMPARE(config.getNeighbor("linux.local", Direction::Left, 0.5f, &destination), std::string("mac.local"));
  QVERIFY(qAbs(destination - 0.77777778f) < 0.00001f);
  QVERIFY(config.getNeighbor("mac.local", Direction::Right, 0.1f, nullptr).empty());
  QVERIFY(config.getNeighbor("linux.local", Direction::Left, 0.9f, nullptr).empty());
  const auto mac = config.getOptions("mac.local");
  QVERIFY(mac != nullptr);
  QCOMPARE(mac->at(kOptionModifierMapForControl), OptionValue(kKeyModifierIDSuper));
  QCOMPARE(mac->at(kOptionModifierMapForSuper), OptionValue(kKeyModifierIDControl));
  QCOMPARE(mac->at(kOptionComputerSwitchCornerSize), OptionValue(17));
  Settings::setValue(swap);
  Settings::setValue(corner);
}

void ServerConfigTests::equalityCheck()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addComputer("computerA"));
  QVERIFY(a != b);

  QVERIFY(b.addComputer("computerB"));
  QVERIFY(a != b);

  QVERIFY(a.addComputer("computerB"));
  QVERIFY(a.addComputer("computerC"));
  QVERIFY(a.connect("computerA", Direction::Bottom, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(a.connect("computerB", Direction::Left, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(b.addComputer("computerA"));
  QVERIFY(b.addComputer("computerC"));
  QVERIFY(b.connect("computerA", Direction::Bottom, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(b.connect("computerB", Direction::Left, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(a.addOption("computerA", kOptionClipboardSharing, 1));
  QVERIFY(b.addOption("computerA", kOptionClipboardSharing, 1));
  QVERIFY(a.addOption(std::string(), kOptionClipboardSharing, 1));
  QVERIFY(b.addOption(std::string(), kOptionClipboardSharing, 1));

  a.getInputFilter()->addFilterRule(InputFilter::Rule{new OnlySystemFilter()});
  b.getInputFilter()->addFilterRule(InputFilter::Rule{new OnlySystemFilter()});
  QVERIFY(a.addAlias("computerA", "aliasA"));
  QVERIFY(b.addAlias("computerA", "aliasA"));
  /* TODO Fix linking to the proper libs
  NetworkAddress addr1("localhost", 8080);
  addr1.resolve();
  NetworkAddress addr2("localhost", 8080);
  addr2.resolve();
  a.setDeskflowAddress(addr1);
  b.setDeskflowAddress(addr2);
  */
  QVERIFY(a == b);
}

void ServerConfigTests::equalityCheck_diff_options()
{
  Config a(nullptr);
  Config b(nullptr);

  QVERIFY(a.addComputer("computerA"));
  QVERIFY(b.addComputer("computerA"));
  QVERIFY(a.addOption("computerA", kOptionClipboardSharing, 0));
  QVERIFY(b.addOption("computerA", kOptionClipboardSharing, 1));
  QVERIFY(a != b);
}

void ServerConfigTests::equalityCheck_diff_alias()
{
  Config a(nullptr);
  Config b(nullptr);

  QVERIFY(a.addComputer("computerA"));
  QVERIFY(b.addComputer("computerA"));
  QVERIFY(b.addAlias("computerA", "aliasA"));
  QVERIFY(a != b);

  QVERIFY(a.addAlias("computerA", "aliasA"));
  QVERIFY(b.addAlias("computerA", "aliasB"));
  QVERIFY(a != b);
}

void ServerConfigTests::equalityCheck_diff_filters()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addComputer("computerA"));
  QVERIFY(b.addComputer("computerA"));

  a.getInputFilter()->addFilterRule(InputFilter::Rule{new OnlySystemFilter()});
  QVERIFY(a != b);
}

// TODO FIX
/*
void ServerConfigTests::equalityCheck_diff_address()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addComputer("computerA"));
  QVERIFY(b.addComputer("computerA"));
  a.setDeskflowAddress(NetworkAddress(8000));
  b.setDeskflowAddress(NetworkAddress(9000));
  QVERIFY(a != b);
}
*/

void ServerConfigTests::equalityCheck_diff_neighbours1()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addComputer("computerA"));
  QVERIFY(a.addComputer("computerB"));
  QVERIFY(a.connect("computerA", Direction::Bottom, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(b.addComputer("computerA"));
  QVERIFY(b.addComputer("computerB"));
  QVERIFY(a != b);
  QVERIFY(b != a);
}

void ServerConfigTests::equalityCheck_diff_neighbours2()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addComputer("computerA"));
  QVERIFY(a.addComputer("computerB"));
  QVERIFY(a.connect("computerA", Direction::Bottom, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(b.addComputer("computerA"));
  QVERIFY(b.addComputer("computerB"));
  QVERIFY(b.connect("computerA", Direction::Bottom, 0.0f, 0.25f, "computerB", 0.25f, 1.0f));
  QVERIFY(a != b);
}

void ServerConfigTests::equalityCheck_diff_neighbours3()
{
  Config a(nullptr);
  Config b(nullptr);
  QVERIFY(a.addComputer("computerA"));
  QVERIFY(a.addComputer("computerB"));
  QVERIFY(a.addComputer("computerC"));
  QVERIFY(a.connect("computerA", Direction::Bottom, 0.0f, 0.5f, "computerB", 0.5f, 1.0f));
  QVERIFY(b.addComputer("computerA"));
  QVERIFY(b.addComputer("computerB"));
  QVERIFY(b.addComputer("computerC"));
  QVERIFY(b.connect("computerA", Direction::Bottom, 0.0f, 0.5f, "computerC", 0.5f, 1.0f));
  QVERIFY(a != b);
}

QTEST_MAIN(ServerConfigTests)
