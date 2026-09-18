/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "arch/Arch.h"
#include "base/Log.h"
#include "platform/FileTransfer.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t cancelled = 0;
void stop(int)
{
  cancelled = 1;
}
void output(const QJsonObject &object)
{
  const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
  std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
  std::fflush(stdout);
}
} // namespace

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  app.setApplicationName("Deskflow File Transfer");
  Arch arch;
  arch.init();
  Log log;
  struct sigaction action{};
  action.sa_handler = stop;
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, nullptr);
  sigaction(SIGINT, &action, nullptr);
  if (argc != 2 && argc != 3)
    return 2;
  bool ok = false;
  const int index = QString::fromLocal8Bit(argv[1]).toInt(&ok);
  if (!ok || index < 0)
    return 2;
  // A pipe may deliver a manifest in many short reads. Read through EOF,
  // with a strict bound, before parsing it.
  QByteArray offer;
  char block[65536];
  while (!cancelled && offer.size() <= deskflow::FileTransfer::kMaxOfferBytes) {
    const auto size = qMin<qint64>(sizeof(block), deskflow::FileTransfer::kMaxOfferBytes + 1 - offer.size());
    const auto count = ::read(STDIN_FILENO, block, size);
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return 2;
    }
    if (count == 0)
      break;
    offer.append(block, count);
  }
  if (cancelled || !deskflow::FileTransfer::validOffer(offer))
    return 2;
  // This helper only receives; it uses no fixed TCP listener and no Deskflow
  // input/clipboard service. It inherits the File Provider sandbox.
  deskflow::FileTransfer files({}, 0, argc == 3 ? QString::fromLocal8Bit(argv[2]) : QString(), {"127.0.0.1"}, false);
  QElapsedTimer update;
  update.start();
  const auto path = files.receiveFile(
      offer, index, [] { return cancelled != 0; },
      [&](qint64 done, qint64 total) {
        if (done == total || update.elapsed() >= 250) {
          output({{"event", "progress"}, {"done", QString::number(done)}, {"total", QString::number(total)}});
          update.restart();
        }
      }
  );
  if (path.isEmpty()) {
    output({{"event", cancelled ? "cancelled" : "failed"}});
    return 1;
  }
  output({{"event", "complete"}, {"path", path}});
  return 0;
}
