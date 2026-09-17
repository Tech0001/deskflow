/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/FileTransfer.h"

#include "base/Log.h"
#include "common/Settings.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QSaveFile>
#include <QSet>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace deskflow {
namespace {
constexpr qint64 kBlock = 64 * 1024;
constexpr qint64 kOfferLifetime = 30 * 60 * 1000;
constexpr int kIdleTimeout = 10000;

bool cancelled(const FileTransfer::Cancelled &check)
{
  return check && check();
}

bool hex(const QString &s, int length)
{
  if (s.size() != length)
    return false;
  for (auto c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

bool safePath(const QString &path)
{
  if (path.isEmpty() || path.size() > 1024 || path.contains('\\') || path.contains(':'))
    return false;
  const auto parts = path.split('/');
  if (parts.size() > 32)
    return false;
  for (const auto &part : parts) {
    if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' '))
      return false;
    for (auto c : part) {
      if (c.unicode() < 32 || c.unicode() == 127)
        return false;
    }
  }
  return true;
}

// Open every component without following symlinks, including parents that
// might have been replaced since the copied file was registered.
bool openRegular(QFile &file, const QString &path)
{
  if (!path.startsWith('/'))
    return false;
  const auto parts = path.split('/', Qt::SkipEmptyParts);
  int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  for (int i = 0; fd >= 0 && i < parts.size(); ++i) {
    const auto name = QFile::encodeName(parts[i]);
    const int next = ::openat(
        fd, name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | (i + 1 < parts.size() ? O_DIRECTORY : 0)
    );
    ::close(fd);
    fd = next;
  }
  struct stat st{};
  if (fd < 0)
    return false;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      !file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
    ::close(fd);
    return false;
  }
  return true;
}

struct Entry
{
  QString path;
  QString source;
  bool directory = false;
  qint64 size = 0;
  QByteArray digest;
};

struct Offer
{
  QString token;
  QStringList addresses;
  quint16 port = 0;
  QByteArray fingerprint;
  qint64 expires = 0;
  QList<Entry> entries;
  QStringList roots;
  qint64 total = 0;
};

bool decode(const QByteArray &bytes, Offer &offer)
{
  if (bytes.size() > FileTransfer::kMaxOfferBytes)
    return false;
  const auto doc = QJsonDocument::fromJson(bytes);
  if (!doc.isObject())
    return false;
  const auto o = doc.object();
  if (o["version"].toInt() != 1 || !hex(o["token"].toString(), 64) || !hex(o["fingerprint"].toString(), 64))
    return false;
  offer.token = o["token"].toString();
  offer.fingerprint = QByteArray::fromHex(o["fingerprint"].toString().toLatin1());
  const auto port = o["port"].toInt();
  if (port < 1 || port > 65535)
    return false;
  offer.port = static_cast<quint16>(port);
  bool ok = false;
  offer.expires = o["expires"].toString().toLongLong(&ok);
  const auto now = QDateTime::currentMSecsSinceEpoch();
  if (!ok || offer.expires < now || offer.expires > now + kOfferLifetime + 60000)
    return false;
  const auto addresses = o["addresses"].toArray();
  if (addresses.isEmpty() || addresses.size() > 16)
    return false;
  for (const auto &value : addresses) {
    QHostAddress address(value.toString());
    if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isNull() || address.isMulticast() ||
        address == QHostAddress::AnyIPv4 || address == QHostAddress::Broadcast)
      return false;
    offer.addresses.append(address.toString());
  }
  const auto entries = o["entries"].toArray();
  if (entries.isEmpty() || entries.size() > FileTransfer::kMaxEntries)
    return false;
  QSet<QString> seen;
  QSet<QString> directories;
  for (const auto &value : entries) {
    if (!value.isObject())
      return false;
    const auto e = value.toObject();
    Entry entry;
    entry.path = e["path"].toString();
    if (!safePath(entry.path) || !e["directory"].isBool())
      return false;
    const auto key = entry.path.normalized(QString::NormalizationForm_C).toCaseFolded();
    if (seen.contains(key))
      return false;
    const auto slash = key.lastIndexOf('/');
    if (slash >= 0 && !directories.contains(key.left(slash)))
      return false;
    seen.insert(key);
    entry.directory = e["directory"].toBool();
    entry.size = e["size"].toString().toLongLong(&ok);
    if (!ok || entry.size < 0 || entry.size > FileTransfer::kMaxBytes - offer.total)
      return false;
    offer.total += entry.size;
    if (entry.directory) {
      if (entry.size != 0)
        return false;
      directories.insert(key);
    } else {
      if (!hex(e["sha256"].toString(), 64))
        return false;
      entry.digest = QByteArray::fromHex(e["sha256"].toString().toLatin1());
    }
    if (slash < 0)
      offer.roots.append(entry.path);
    offer.entries.append(entry);
  }
  return !offer.roots.isEmpty();
}

QByteArray encode(const Offer &offer)
{
  QJsonArray entries;
  for (const auto &e : offer.entries) {
    entries.append(
        QJsonObject{
            {"path", e.path},
            {"directory", e.directory},
            {"size", QString::number(e.size)},
            {"sha256", QString::fromLatin1(e.digest.toHex())}
        }
    );
  }
  return QJsonDocument(
             QJsonObject{
                 {"version", 1},
                 {"token", offer.token},
                 {"addresses", QJsonArray::fromStringList(offer.addresses)},
                 {"port", offer.port},
                 {"fingerprint", QString::fromLatin1(offer.fingerprint.toHex())},
                 {"expires", QString::number(offer.expires)},
                 {"entries", entries}
             }
  ).toJson(QJsonDocument::Compact);
}

bool waitEncrypted(QSslSocket &socket, const FileTransfer::Cancelled &check)
{
  const QDeadlineTimer deadline(kIdleTimeout);
  while (!socket.isEncrypted() && !deadline.hasExpired() && !cancelled(check)) {
    socket.waitForEncrypted(100);
    if (socket.state() == QAbstractSocket::UnconnectedState)
      break;
  }
  return socket.isEncrypted() && !cancelled(check);
}

QByteArray readLine(QSslSocket &socket, int maxBytes, const FileTransfer::Cancelled &check)
{
  const QDeadlineTimer deadline(kIdleTimeout);
  while (!socket.canReadLine() && socket.bytesAvailable() <= maxBytes && !deadline.hasExpired() && !cancelled(check)) {
    if (socket.state() == QAbstractSocket::UnconnectedState)
      return {};
    socket.waitForReadyRead(100);
  }
  if (!socket.canReadLine() || cancelled(check))
    return {};
  const auto line = socket.readLine(maxBytes + 1);
  return line.size() <= maxBytes && line.endsWith('\n') ? line : QByteArray();
}

class Listener : public QTcpServer
{
public:
  std::deque<qintptr> descriptors;
  ~Listener() override
  {
    for (auto fd : descriptors)
      ::close(static_cast<int>(fd));
  }
  void incomingConnection(qintptr fd) override
  {
    if (descriptors.size() < 4)
      descriptors.push_back(fd);
    else
      ::close(static_cast<int>(fd));
  }
};
} // namespace

struct FileTransfer::Impl
{
  QString certificatePath;
  quint16 requestedPort;
  quint16 port = 0;
  QString cacheRoot;
  QStringList addresses;
  QByteArray fingerprint;
  std::atomic<bool> stopping = false;
  std::mutex mutex;
  std::condition_variable ready;
  bool initialized = false;
  std::thread server;
  QList<Offer> offers;
  QByteArray lastOffer;
  QStringList lastPaths;
  QHash<QByteArray, QStringList> received;

  Impl(QString cert, quint16 requested, QString root, QStringList hosts)
      : certificatePath(std::move(cert)),
        requestedPort(requested),
        cacheRoot(std::move(root)),
        addresses(std::move(hosts))
  {
    server = std::thread([this] { serve(); });
  }

  ~Impl()
  {
    stopping = true;
    if (server.joinable())
      server.join();
  }

  void serve()
  {
    QFile pem(certificatePath);
    QByteArray bytes;
    if (pem.open(QIODevice::ReadOnly))
      bytes = pem.readAll();
    const QSslCertificate certificate(bytes);
    QSslKey key(bytes, QSsl::Rsa);
    if (key.isNull())
      key = QSslKey(bytes, QSsl::Ec);
    Listener listener;
    const bool success =
        !certificate.isNull() && !key.isNull() && listener.listen(QHostAddress::AnyIPv4, requestedPort);
    {
      std::scoped_lock lock(mutex);
      if (success) {
        port = listener.serverPort();
        fingerprint = certificate.digest(QCryptographicHash::Sha256);
      }
      initialized = true;
    }
    ready.notify_all();
    if (!success) {
      LOG_WARN("file clipboard listener unavailable (certificate or TCP port %u)", requestedPort);
      return;
    }
    LOG_INFO("file clipboard TLS listener ready on TCP port %u", port);
    while (!stopping) {
      if (listener.descriptors.empty())
        listener.waitForNewConnection(100);
      if (listener.descriptors.empty())
        continue;
      const auto fd = listener.descriptors.front();
      listener.descriptors.pop_front();
      QSslSocket socket;
      if (!socket.setSocketDescriptor(fd)) {
        ::close(static_cast<int>(fd));
        continue;
      }
      socket.setReadBufferSize(4096);
      socket.setLocalCertificate(certificate);
      socket.setPrivateKey(key);
      socket.setPeerVerifyMode(QSslSocket::VerifyNone); // The unguessable offer token authorizes reads.
      socket.setProtocol(QSsl::TlsV1_2OrLater);
      socket.startServerEncryption();
      const auto stop = [this] { return stopping.load(); };
      if (!waitEncrypted(socket, stop))
        continue;
      const auto request = readLine(socket, 128, stop).trimmed().split(' ');
      Entry entry;
      bool found = false;
      if (request.size() == 3 && request[0] == "DFT1") {
        bool ok = false;
        const int index = request[2].toInt(&ok);
        std::scoped_lock lock(mutex);
        for (const auto &offer : offers) {
          if (ok && offer.token.toLatin1() == request[1] && offer.expires > QDateTime::currentMSecsSinceEpoch() &&
              index >= 0 && index < offer.entries.size() && !offer.entries[index].directory) {
            entry = offer.entries[index];
            found = true;
            break;
          }
        }
      }
      QFile file;
      if (!found || !openRegular(file, entry.source) || file.size() != entry.size) {
        socket.write("ERROR\n");
        socket.waitForBytesWritten(100);
        continue;
      }
      socket.write("OK " + QByteArray::number(entry.size) + '\n');
      qint64 left = entry.size;
      QDeadlineTimer idle(kIdleTimeout);
      while (left > 0 && !stopping && socket.state() == QAbstractSocket::ConnectedState) {
        if (socket.bytesToWrite() + socket.encryptedBytesToWrite() > kBlock * 2) {
          if (socket.waitForBytesWritten(100))
            idle = QDeadlineTimer(kIdleTimeout);
          if (idle.hasExpired())
            break;
          continue;
        }
        const auto block = file.read(qMin(kBlock, left));
        if (block.isEmpty() || socket.write(block) != block.size())
          break;
        left -= block.size();
        idle = QDeadlineTimer(kIdleTimeout);
      }
      while (!stopping && socket.bytesToWrite() + socket.encryptedBytesToWrite() > 0 && !idle.hasExpired()) {
        if (socket.waitForBytesWritten(100))
          idle = QDeadlineTimer(kIdleTimeout);
      }
      socket.disconnectFromHost();
      while (!stopping && socket.state() != QAbstractSocket::UnconnectedState && !idle.hasExpired())
        socket.waitForDisconnected(100);
    }
  }
};

FileTransfer::FileTransfer(QString certificate, quint16 port, QString cacheRoot, QStringList addresses)
{
  if (certificate.isEmpty())
    certificate = Settings::value(Settings::Security::Certificate).toString();
  if (cacheRoot.isEmpty())
    cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/file-transfers";
  if (addresses.isEmpty()) {
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
      if (!(iface.flags() & QNetworkInterface::IsUp) || !(iface.flags() & QNetworkInterface::IsRunning) ||
          (iface.flags() & QNetworkInterface::IsLoopBack))
        continue;
      for (const auto &a : iface.addressEntries()) {
        if (a.ip().protocol() == QAbstractSocket::IPv4Protocol && addresses.size() < 16)
          addresses.append(a.ip().toString());
      }
    }
  }
  m_impl = std::make_unique<Impl>(certificate, port, cacheRoot, addresses);
}

FileTransfer::~FileTransfer() = default;

bool FileTransfer::enabled()
{
  return Settings::value(Settings::Security::ShareFiles).toBool() &&
         Settings::value(Settings::Security::TlsEnabled).toBool() &&
         Settings::value(Settings::Security::CheckPeers).toBool() &&
         Settings::value(Settings::Server::EnableClipboard).toBool();
}

QStringList FileTransfer::pathsFromUris(const QByteArray &bytes)
{
  if (bytes.size() > kMaxOfferBytes)
    return {};
  QStringList paths;
  for (auto line : bytes.split('\n')) {
    if (line.endsWith('\r'))
      line.chop(1);
    if (line.isEmpty() || line.startsWith('#'))
      continue;
    const QUrl url = QUrl::fromEncoded(line, QUrl::StrictMode);
    if (!url.isValid() || !url.isLocalFile() || (!url.host().isEmpty() && url.host() != "localhost") ||
        url.hasQuery() || url.hasFragment() || !QDir::isAbsolutePath(url.toLocalFile()))
      return {};
    paths.append(url.toLocalFile());
    if (paths.size() > kMaxEntries)
      return {};
  }
  return paths;
}

QByteArray FileTransfer::urisFromPaths(const QStringList &paths)
{
  QByteArray result;
  for (const auto &path : paths)
    result += QUrl::fromLocalFile(path).toEncoded() + "\r\n";
  return result;
}

bool FileTransfer::validOffer(const QByteArray &bytes)
{
  Offer offer;
  return decode(bytes, offer);
}

QByteArray FileTransfer::offer(const QStringList &paths, const Cancelled &check)
{
  if (paths.isEmpty() || paths.size() > kMaxEntries || cancelled(check))
    return {};
  auto &impl = *m_impl;
  {
    std::unique_lock lock(impl.mutex);
    impl.ready.wait(lock, [&impl] { return impl.initialized; });
    if (!impl.port || impl.addresses.isEmpty())
      return {};
  }
  Offer offer;
  offer.addresses = impl.addresses;
  offer.port = impl.port;
  offer.fingerprint = impl.fingerprint;
  offer.token = QUuid::createUuid().toString(QUuid::Id128) + QUuid::createUuid().toString(QUuid::Id128);
  QSet<QString> names;
  std::function<bool(const QString &, const QString &)> add = [&](const QString &source, const QString &relative) {
    if (cancelled(check) || offer.entries.size() >= kMaxEntries || !safePath(relative))
      return false;
    const auto key = relative.normalized(QString::NormalizationForm_C).toCaseFolded();
    if (names.contains(key))
      return false;
    names.insert(key);
    QFileInfo info(source);
    if (info.isSymLink() || (!info.isDir() && !info.isFile()) || !info.isReadable())
      return false;
    Entry entry{relative, QDir::cleanPath(source), info.isDir(), 0, {}};
    if (entry.source.isEmpty())
      return false;
    if (entry.directory) {
      offer.entries.append(entry);
      QDirIterator children(entry.source, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
      while (children.hasNext()) {
        children.next();
        const auto child = children.fileInfo();
        if (!add(child.absoluteFilePath(), relative + '/' + child.fileName()))
          return false;
      }
    } else {
      QFile file;
      if (!openRegular(file, entry.source))
        return false;
      entry.size = file.size();
      if (entry.size < 0 || entry.size > kMaxBytes - offer.total)
        return false;
      QCryptographicHash hash(QCryptographicHash::Sha256);
      qint64 read = 0;
      while (!file.atEnd()) {
        if (cancelled(check))
          return false;
        const auto block = file.read(kBlock);
        if (block.isEmpty() || block.size() > entry.size - read)
          return false;
        hash.addData(block);
        read += block.size();
      }
      if (read != entry.size)
        return false;
      entry.digest = hash.result();
      offer.total += entry.size;
      offer.entries.append(entry);
    }
    return true;
  };
  for (const auto &path : paths) {
    const QFileInfo root(path);
    const auto parent = root.dir().canonicalPath();
    if (parent.isEmpty() || !add(parent + '/' + root.fileName(), root.fileName())) {
      LOG_WARN("file clipboard offer rejected: unsupported, changed, duplicate, cancelled, or oversized selection");
      return {};
    }
  }
  offer.expires = QDateTime::currentMSecsSinceEpoch() + kOfferLifetime;
  auto bytes = encode(offer);
  if (bytes.size() > kMaxOfferBytes || cancelled(check))
    return {};
  // Reuse an unchanged offer so crossing a screen doesn't claim a new owner.
  if (paths == impl.lastPaths) {
    Offer previous;
    if (decode(impl.lastOffer, previous)) {
      auto normalized = offer;
      normalized.token = previous.token;
      normalized.expires = previous.expires;
      if (encode(normalized) == impl.lastOffer)
        return impl.lastOffer;
    }
  }
  {
    std::scoped_lock lock(impl.mutex);
    while (impl.offers.size() >= 8)
      impl.offers.removeFirst();
    impl.offers.append(offer);
  }
  impl.lastPaths = paths;
  impl.lastOffer = bytes;
  LOG_INFO(
      "file clipboard offered %lld entries, %lld bytes", static_cast<long long>(offer.entries.size()),
      static_cast<long long>(offer.total)
  );
  return bytes;
}

QStringList FileTransfer::receive(const QByteArray &bytes, const Cancelled &check)
{
  Offer offer;
  if (!decode(bytes, offer) || cancelled(check)) {
    LOG_WARN("invalid or expired file clipboard offer");
    return {};
  }
  auto &impl = *m_impl;
  const auto id = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
  if (const auto cached = impl.received.value(id); !cached.isEmpty()) {
    bool exists = true;
    for (const auto &path : cached)
      exists = exists && QFileInfo::exists(path);
    if (exists)
      return cached;
  }
  if (!QDir().mkpath(impl.cacheRoot))
    return {};
  QFile::setPermissions(impl.cacheRoot, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
  // Completed selections are retained for later Paste, including after restart.
  // Expired caches are only removed at the start of a new file copy.
  QDir cache(impl.cacheRoot);
  for (const auto &entry : cache.entryInfoList({"transfer-*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
    if (!entry.isSymLink() && entry.lastModified().secsTo(QDateTime::currentDateTime()) > 24 * 60 * 60)
      QDir(entry.absoluteFilePath()).removeRecursively();
  }
  const QStorageInfo storage(impl.cacheRoot);
  if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < offer.total + 16 * 1024 * 1024) {
    LOG_WARN("not enough free space for file clipboard transfer");
    return {};
  }
  QTemporaryDir staging(impl.cacheRoot + "/.incoming-XXXXXX");
  if (!staging.isValid())
    return {};
  const QDeadlineTimer deadline(15 * 60 * 1000);
  const auto stop = [&] { return cancelled(check) || deadline.hasExpired(); };
  QString successfulHost;
  LOG_INFO(
      "receiving file clipboard: %lld entries, %lld bytes", static_cast<long long>(offer.entries.size()),
      static_cast<long long>(offer.total)
  );
  for (int i = 0; i < offer.entries.size(); ++i) {
    if (stop())
      return {};
    const auto &entry = offer.entries[i];
    const auto destination = staging.filePath(entry.path);
    if (entry.directory) {
      if (!QDir().mkpath(destination))
        return {};
      QFile::setPermissions(destination, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
      continue;
    }
    QSslSocket socket;
    socket.setReadBufferSize(kBlock * 2);
    socket.setProtocol(QSsl::TlsV1_2OrLater);
    // Trust is the exact certificate delivered inside authenticated Deskflow
    // clipboard data, not the CA store or an arbitrary self-signed certificate.
    socket.setPeerVerifyMode(QSslSocket::VerifyNone);
    auto hosts = offer.addresses;
    if (!successfulHost.isEmpty()) {
      hosts.removeAll(successfulHost);
      hosts.prepend(successfulHost);
    }
    bool connected = false;
    for (const auto &host : hosts) {
      socket.connectToHostEncrypted(host, offer.port);
      if (waitEncrypted(socket, stop) &&
          socket.peerCertificate().digest(QCryptographicHash::Sha256) == offer.fingerprint) {
        connected = true;
        successfulHost = host;
        break;
      }
      socket.abort();
      if (stop())
        return {};
    }
    if (!connected) {
      LOG_WARN("file clipboard connection failed (address, TCP port, or certificate pin)");
      return {};
    }
    socket.write("DFT1 " + offer.token.toLatin1() + ' ' + QByteArray::number(i) + '\n');
    socket.flush();
    if (readLine(socket, 80, stop) != "OK " + QByteArray::number(entry.size) + '\n') {
      LOG_WARN("file clipboard source refused a file (expired or changed selection)");
      return {};
    }
    QSaveFile file(destination);
    if (!file.open(QIODevice::WriteOnly))
      return {};
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 left = entry.size;
    QDeadlineTimer idle(kIdleTimeout);
    while (left > 0 && !stop() && !idle.hasExpired()) {
      const auto block = socket.read(qMin(kBlock, left));
      if (block.isEmpty()) {
        if (socket.state() == QAbstractSocket::UnconnectedState)
          break;
        socket.waitForReadyRead(100);
        continue;
      }
      if (file.write(block) != block.size())
        return {};
      hash.addData(block);
      left -= block.size();
      idle = QDeadlineTimer(kIdleTimeout);
    }
    if (left != 0 || stop() || hash.result() != entry.digest || !file.commit()) {
      LOG_WARN(
          "file clipboard transfer incomplete or checksum mismatch (%lld bytes missing); discarded staging files",
          static_cast<long long>(left)
      );
      return {};
    }
  }
  const auto finalPath = impl.cacheRoot + "/transfer-" + QUuid::createUuid().toString(QUuid::Id128);
  if (stop() || !QDir().rename(staging.path(), finalPath))
    return {};
  staging.setAutoRemove(false);
  QStringList result;
  for (const auto &root : offer.roots)
    result.append(finalPath + '/' + root);
  if (impl.received.size() >= 16)
    impl.received.clear();
  impl.received.insert(id, result);
  LOG_INFO("file clipboard transfer complete; %lld copied items ready to paste", static_cast<long long>(result.size()));
  return result;
}

} // namespace deskflow
