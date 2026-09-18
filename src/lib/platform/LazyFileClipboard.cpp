/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#define FUSE_USE_VERSION 31
#include "platform/LazyFileClipboard.h"
#include "base/Log.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fuse3/fuse.h>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace deskflow {
struct LazyFileClipboard::Impl
{
  struct Selection
  {
    QByteArray offer;
    QList<FileTransfer::Entry> entries;
    QStringList paths;
  };
  struct Handle
  {
    std::shared_ptr<Selection> selection;
    int index;
    int fd = -1;
    std::mutex mutex;
    ~Handle()
    {
      if (fd >= 0)
        ::close(fd);
    }
  };
  FileTransfer &files;
  QString mount;
  struct fuse *filesystem = nullptr;
  std::thread worker;
  std::atomic<bool> stopping = false;
  mutable std::mutex mutex;
  std::map<QString, std::shared_ptr<Selection>> selections;

  static Impl &self()
  {
    return *static_cast<Impl *>(fuse_get_context()->private_data);
  }

  std::pair<std::shared_ptr<Selection>, int> lookup(const char *raw) const
  {
    const auto path = QString::fromUtf8(raw).mid(1);
    const auto slash = path.indexOf('/');
    const auto id = slash < 0 ? path : path.left(slash);
    std::scoped_lock lock(mutex);
    const auto it = selections.find(id);
    if (it == selections.end())
      return {{}, -1};
    if (slash < 0)
      return {it->second, -1};
    const auto relative = path.mid(slash + 1);
    for (int i = 0; i < it->second->entries.size(); ++i)
      if (it->second->entries[i].path == relative)
        return {it->second, i};
    return {{}, -1};
  }

  static int attributes(const char *path, struct stat *st, struct fuse_file_info *)
  {
    std::memset(st, 0, sizeof(*st));
    st->st_uid = getuid();
    st->st_gid = getgid();
    bool directory = !std::strcmp(path, "/");
    if (!directory) {
      const auto [selection, index] = self().lookup(path);
      if (!selection)
        return -ENOENT;
      directory = index < 0 || selection->entries[index].directory;
      if (!directory)
        st->st_size = selection->entries[index].size;
    }
    // The mount is read-only; advertise normal owner permissions so the native
    // file manager does not create unwritable destination copies.
    st->st_mode = directory ? S_IFDIR | 0700 : S_IFREG | 0600;
    st->st_nlink = directory ? 2 : 1;
    st->st_blksize = 65536;
    return 0;
  }

  static std::pair<std::shared_ptr<Selection>, int> lookupHandle(const char *path, struct fuse_file_info *info)
  {
    if (info && info->fh) {
      const auto *handle = reinterpret_cast<Handle *>(info->fh);
      return {handle->selection, handle->index};
    }
    return self().lookup(path);
  }

  static int list(
      const char *path, void *buffer, fuse_fill_dir_t fill, off_t offset, struct fuse_file_info *info,
      enum fuse_readdir_flags
  )
  {
    QStringList names{".", ".."};
    if (!std::strcmp(path, "/")) {
      std::scoped_lock lock(self().mutex);
      for (const auto &[id, selection] : self().selections)
        names.append(id);
    } else {
      const auto [selection, index] = lookupHandle(path, info);
      if (!selection)
        return -ENOENT;
      if (index >= 0 && !selection->entries[index].directory)
        return -ENOTDIR;
      const QString prefix = index < 0 ? QString() : selection->entries[index].path + '/';
      for (const auto &entry : selection->entries) {
        if (!entry.path.startsWith(prefix))
          continue;
        const auto name = entry.path.mid(prefix.size());
        if (!name.isEmpty() && !name.contains('/'))
          names.append(name);
      }
    }
    for (auto i = offset; i >= 0 && i < names.size(); ++i)
      if (fill(buffer, names[i].toUtf8().constData(), nullptr, i + 1, static_cast<fuse_fill_dir_flags>(0)))
        break;
    return 0;
  }

  static int openDirectory(const char *path, struct fuse_file_info *info)
  {
    if (!std::strcmp(path, "/"))
      return 0;
    const auto [selection, index] = self().lookup(path);
    if (!selection)
      return -ENOENT;
    if (index >= 0 && !selection->entries[index].directory)
      return -ENOTDIR;
    info->fh = reinterpret_cast<uint64_t>(new Handle{selection, index});
    return 0;
  }

  static int release(const char *, struct fuse_file_info *info)
  {
    delete reinterpret_cast<Handle *>(info->fh);
    info->fh = 0;
    return 0;
  }

  static int openFile(const char *path, struct fuse_file_info *info)
  {
    if ((info->flags & O_ACCMODE) != O_RDONLY)
      return -EROFS;
    const auto [selection, index] = self().lookup(path);
    if (!selection || index < 0)
      return -ENOENT;
    if (selection->entries[index].directory)
      return -EISDIR;
    info->fh = reinterpret_cast<uint64_t>(new Handle{selection, index});
    info->direct_io = 1;
    return 0;
  }

  static int readFile(const char *, char *buffer, size_t size, off_t offset, struct fuse_file_info *info)
  {
    auto &impl = self();
    auto *handle = reinterpret_cast<Handle *>(info->fh);
    if (!handle || offset < 0)
      return -EINVAL;
    std::scoped_lock lock(handle->mutex);
    const auto stop = [&] { return impl.stopping || fuse_interrupted() || !FileTransfer::enabled(); };
    if (handle->fd < 0) {
      const auto cached = impl.files.receiveFile(handle->selection->offer, handle->index, stop);
      if (cached.isEmpty())
        return stop() ? -EINTR : -EIO;
      handle->fd = ::open(QFile::encodeName(cached).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      if (handle->fd < 0)
        return -EIO;
    }
    const auto count = ::pread(handle->fd, buffer, size, offset);
    return count < 0 ? -errno : static_cast<int>(count);
  }

  Impl(FileTransfer &transfer, QString root) : files(transfer)
  {
    if (root.isEmpty())
      root = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/deskflow-files-" +
             QString::number(QCoreApplication::applicationPid());
    mount = root;
    if (!QDir().mkpath(mount))
      return;
    QFile::setPermissions(mount, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    struct fuse_operations ops{};
    ops.getattr = attributes;
    ops.readdir = list;
    ops.open = openFile;
    ops.release = release;
    ops.opendir = openDirectory;
    ops.releasedir = release;
    ops.read = readFile;
    struct fuse_args args = FUSE_ARGS_INIT(0, nullptr);
    fuse_opt_add_arg(&args, "deskflow-files");
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "ro,default_permissions,fsname=Deskflow,subtype=deskflow");
    filesystem = fuse_new(&args, &ops, sizeof(ops), this);
    fuse_opt_free_args(&args);
    if (filesystem && fuse_mount(filesystem, mount.toUtf8().constData()) != 0) {
      fuse_destroy(filesystem);
      filesystem = nullptr;
    }
    if (filesystem)
      worker = std::thread([this] { fuse_loop_mt(filesystem, 0); });
    else
      LOG_WARN("could not mount Deskflow file clipboard; check /dev/fuse and fusermount3");
  }

  ~Impl()
  {
    stopping = true;
    if (filesystem) {
      fuse_exit(filesystem);
      fuse_unmount(filesystem);
      if (worker.joinable())
        worker.join();
      fuse_destroy(filesystem);
    }
    QDir().rmdir(mount);
  }
};

LazyFileClipboard::LazyFileClipboard(FileTransfer &files, QString root)
    : m_impl(std::make_unique<Impl>(files, std::move(root)))
{
}
LazyFileClipboard::~LazyFileClipboard() = default;
bool LazyFileClipboard::available() const
{
  return m_impl->filesystem != nullptr;
}

QStringList LazyFileClipboard::publish(const QByteArray &offer)
{
  if (!available())
    return {};
  auto selection = std::make_shared<Impl::Selection>();
  selection->entries = FileTransfer::entries(offer);
  if (selection->entries.isEmpty())
    return {};
  selection->offer = offer;
  const auto id = QString::fromLatin1(QCryptographicHash::hash(offer, QCryptographicHash::Sha256).toHex());
  for (const auto &entry : selection->entries)
    if (!entry.path.contains('/'))
      selection->paths.append(m_impl->mount + '/' + id + '/' + entry.path);
  std::scoped_lock lock(m_impl->mutex);
  // Keep old references alive for accepted pastes. Bound memory without evicting
  // active manifests halfway through a directory copy.
  for (auto it = m_impl->selections.begin(); it != m_impl->selections.end();) {
    if (it->second.use_count() == 1 && !FileTransfer::validOffer(it->second->offer))
      it = m_impl->selections.erase(it);
    else
      ++it;
  }
  if (m_impl->selections.size() >= 64 && !m_impl->selections.contains(id))
    return {};
  m_impl->selections[id] = selection;
  return selection->paths;
}

QByteArray LazyFileClipboard::offerForPaths(const QStringList &paths) const
{
  std::scoped_lock lock(m_impl->mutex);
  for (const auto &[id, selection] : m_impl->selections)
    if (selection->paths == paths)
      return selection->offer;
  return {};
}
} // namespace deskflow
