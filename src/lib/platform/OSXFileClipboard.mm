/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXFileClipboard.h"

#include "base/Log.h"
#include "deskflow/Clipboard.h"
#include "platform/FileTransfer.h"

#import <AppKit/AppKit.h>

#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace deskflow {

namespace {
QByteArray providerCommand(const QString &command, const QByteArray &input, const FileTransfer::Cancelled &stop)
{
  auto program = qEnvironmentVariable("DESKFLOW_FILE_PROVIDER_HELPER");
  if (program.isEmpty()) {
    for (const auto &root : {QDir::homePath() + "/Applications", QString("/Applications")}) {
      const auto candidate = root + "/Deskflow Files.app/Contents/MacOS/DeskflowFiles";
      if (QFileInfo(candidate).isExecutable()) {
        program = candidate;
        break;
      }
    }
  }
  if (program.isEmpty()) {
    if (command == "publish")
      LOG_WARN("Install and enable the Deskflow Files companion to receive files on demand");
    return {};
  }
  QProcess process;
  process.setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors);
  process.setStandardErrorFile(QProcess::nullDevice());
  process.start(program, {command});
  if (!process.waitForStarted(1000))
    return {};
  process.write(input);
  process.closeWriteChannel();
  QByteArray result;
  QDeadlineTimer deadline(45000);
  while (!deadline.hasExpired() && !stop()) {
    process.waitForReadyRead(100);
    result += process.readAllStandardOutput();
    if (result.size() > FileTransfer::kMaxOfferBytes)
      break;
    if (process.state() == QProcess::NotRunning) {
      if (process.exitCode() != 0 && command == "publish")
        LOG_WARN("Deskflow Files could not publish the selection; check that its File Provider extension is enabled");
      return process.exitCode() == 0 ? result : QByteArray();
    }
  }
  process.kill();
  process.waitForFinished(1000);
  return {};
}
} // namespace

struct OSXFileClipboard::Impl
{
  struct Task
  {
    uint64_t generation;
    bool receiving;
    QStringList paths;
    QByteArray offer;
  };

  std::function<void()> changed;
  std::mutex mutex;
  std::condition_variable wake;
  std::atomic<bool> stopping = false;
  std::atomic<uint64_t> generation = 0;
  std::optional<Task> pending;
  std::optional<Task> result;
  std::thread worker;
  std::unique_ptr<FileTransfer> files;
  NSInteger observed = -1;
  bool localFiles = false;
  bool receiving = false;
  QByteArray cached;

  explicit Impl(std::function<void()> callback) : changed(std::move(callback))
  {
    worker = std::thread([this] { run(); });
  }

  ~Impl()
  {
    stopping = true;
    ++generation;
    wake.notify_one();
    worker.join();
  }

  void cancel()
  {
    ++generation;
    std::scoped_lock lock(mutex);
    pending.reset();
    result.reset();
    cached.clear();
    receiving = false;
  }

  void queue(Task task)
  {
    std::scoped_lock lock(mutex);
    pending = std::move(task);
    wake.notify_one();
  }

  void run()
  {
    std::unique_lock lock(mutex);
    while (!stopping) {
      wake.wait(lock, [this] { return stopping || pending.has_value(); });
      if (stopping)
        return;
      auto task = std::move(*pending);
      pending.reset();
      lock.unlock();
      if (!files)
        files = std::make_unique<FileTransfer>();
      const auto stop = [this, id = task.generation] {
        return stopping || generation != id || !FileTransfer::enabled();
      };
      if (task.receiving) {
        task.paths = files->localPaths(task.offer);
        if (task.paths.isEmpty()) {
          const auto result = providerCommand("publish", task.offer, stop);
          const auto document = QJsonDocument::fromJson(result);
          if (document.isArray()) {
            for (const auto &value : document.array()) {
              if (!value.isString() || !QDir::isAbsolutePath(value.toString())) {
                task.paths.clear();
                break;
              }
              task.paths.append(value.toString());
            }
          }
        }
      } else {
        task.offer = providerCommand("resolve", QJsonDocument(QJsonArray::fromStringList(task.paths)).toJson(), stop);
        if (!FileTransfer::validOffer(task.offer))
          task.offer = files->offer(task.paths, stop);
      }
      lock.lock();
      if (!stopping && generation == task.generation)
        result = std::move(task);
    }
  }
};

OSXFileClipboard::OSXFileClipboard(std::function<void()> changed) : m_impl(std::make_unique<Impl>(std::move(changed)))
{
}
OSXFileClipboard::~OSXFileClipboard() = default;

bool OSXFileClipboard::poll()
{
  auto &impl = *m_impl;
  if (!FileTransfer::enabled()) {
    impl.cancel();
    impl.observed = -1;
    impl.localFiles = false;
    return false;
  }
  @autoreleasepool {
    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    if (pasteboard.changeCount != impl.observed) {
      impl.cancel();
      impl.observed = pasteboard.changeCount;
      NSArray *urls = [pasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                options:@{
                                                  NSPasteboardURLReadingFileURLsOnlyKey : @YES
                                                }];
      impl.localFiles = urls.count > 0;
      QStringList paths;
      if (urls.count <= FileTransfer::kMaxEntries) {
        for (NSURL *url in urls) {
          if (url.isFileURL)
            paths.append(QString::fromUtf8(url.path.UTF8String));
        }
      }
      if (!paths.isEmpty())
        impl.queue({impl.generation.load(), false, paths, {}});
    }
    std::optional<Impl::Task> completed;
    {
      std::scoped_lock lock(impl.mutex);
      completed = std::move(impl.result);
      impl.result.reset();
    }
    if (completed && completed->generation == impl.generation) {
      if (completed->receiving) {
        impl.receiving = false;
        if (!completed->paths.isEmpty()) {
          NSMutableArray *urls = [NSMutableArray arrayWithCapacity:completed->paths.size()];
          for (const auto &path : completed->paths) {
            NSString *name = [NSString stringWithUTF8String:path.toUtf8().constData()];
            [urls addObject:[NSURL fileURLWithPath:name]];
          }
          // The change counter was checked above. A newer local copy cancels
          // the download instead of having its clipboard replaced on completion.
          [pasteboard clearContents];
          if ([pasteboard writeObjects:urls]) {
            impl.observed = pasteboard.changeCount;
            impl.localFiles = true;
            impl.cached = completed->offer;
            LOG_INFO("Finder file clipboard ready; contents will download on access");
          }
        }
      } else if (!completed->offer.isEmpty()) {
        impl.cached = completed->offer;
        impl.changed();
      }
    }
    return impl.localFiles || impl.receiving;
  }
}

bool OSXFileClipboard::getClipboard(IClipboard *target)
{
  if (!poll())
    return false;
  Clipboard clipboard;
  clipboard.open(0);
  clipboard.empty();
  if (!m_impl->cached.isEmpty())
    clipboard.add(IClipboard::Format::Files, m_impl->cached.toStdString());
  clipboard.close();
  IClipboard::copy(target, &clipboard);
  return true;
}

bool OSXFileClipboard::setClipboard(const IClipboard *source)
{
  if (!source || !source->open(0))
    return false;
  const bool hasFiles = source->has(IClipboard::Format::Files);
  const auto bytes = hasFiles ? QByteArray::fromStdString(source->get(IClipboard::Format::Files)) : QByteArray();
  source->close();
  m_impl->cancel();
  if (!hasFiles)
    return false;
  if (!FileTransfer::enabled() || !FileTransfer::validOffer(bytes))
    return true; // Never publish raw remote paths or tokens to the native pasteboard.
  @autoreleasepool {
    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents]; // Supersede the old selection before preparing metadata.
    m_impl->observed = pasteboard.changeCount;
    m_impl->localFiles = false;
  }
  m_impl->receiving = true;
  m_impl->queue({m_impl->generation.load(), true, {}, bytes});
  return true;
}

} // namespace deskflow
