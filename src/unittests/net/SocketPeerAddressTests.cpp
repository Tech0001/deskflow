/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "SocketPeerAddressTests.h"
#include "../deskflow/MockEventQueue.h"
#include "io/StreamFilter.h"
#include "net/IDataSocket.h"
#include "net/NetworkAddress.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPListenSocket.h"

#include <QTcpServer>
#include <QTcpSocket>

void SocketPeerAddressTests::initTestCase()
{
  m_arch.init();
}

void SocketPeerAddressTests::acceptedAddressSurvivesStreamFilter()
{
  QTcpServer reserve;
  QVERIFY(reserve.listen(QHostAddress::LocalHost, 0));
  const auto port = reserve.serverPort();
  reserve.close();

  MockEventQueue events;
  SocketMultiplexer multiplexer;
  TCPListenSocket listener(&events, &multiplexer, IArchNetwork::AddressFamily::INet);
  NetworkAddress address("127.0.0.1", port);
  address.resolve();
  listener.bind(address);

  QTcpSocket client;
  client.connectToHost(QHostAddress::LocalHost, port);
  QVERIFY(client.waitForConnected(2000));
  auto accepted = listener.accept();
  QVERIFY(accepted);
  QCOMPARE(accepted->peerAddress(), std::string("127.0.0.1"));
  StreamFilter filter(&events, accepted.get(), false);
  QCOMPARE(filter.peerAddress(), std::string("127.0.0.1"));
}

QTEST_MAIN(SocketPeerAddressTests)
