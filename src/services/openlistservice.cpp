#include "openlistservice.h"
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QUuid>
#include <QStorageInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QDateTime>
#include <QTimeZone>
#include <cmath>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
const QString base = QStringLiteral("https://pan.xn--ucy.xyz");
QString remoteRoot() { return QStringLiteral("/A秋白工作室"); }
constexpr qint64 maxJson = 4 * 1024 * 1024;
// A single HTTP connection can be the bottleneck on some OpenList/CDN setups.
// Use a few ranged requests for larger files, while retaining a single-stream
// fallback for servers or signed URLs that do not support byte ranges.
constexpr qint64 parallelThreshold = 2 * 1024 * 1024;
constexpr qint64 parallelChunk = 1 * 1024 * 1024;
constexpr int maxParallelRequests = 6;
bool apiObject(QNetworkReply *r, QJsonObject *result) {
    if (r->error() != QNetworkReply::NoError || r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
        return false;
    const QByteArray bytes = r->readAll();
    if (bytes.size() > maxJson) return false;
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject() || doc.object().value("code").toInt() != 200 || !doc.object().value("data").isObject())
        return false;
    *result = doc.object().value("data").toObject();
    return true;
}
constexpr int listingPageSize = 100;
// OpenList 的目录读取主要是请求往返耗时；并发几路即可明显缩短整树时间，
// 同时给下载连接留出余量。
constexpr int maxConcurrentListRequests = 4;
constexpr int maxListRequests = 2048;
constexpr int maxListingEntries = 50000;
constexpr int maxListingDepth = 32;
constexpr qint64 maxListingCacheBytes = 8 * 1024 * 1024;
constexpr int listingCacheVersion = 1;
constexpr int maxListingCacheEntries = 50000;
QString listingCacheFileName() { return QStringLiteral("openlist-listing-v1.json"); }
QJsonObject cachedEntryJson(const OpenList::Entry &entry) {
    return QJsonObject{
        {"name", entry.name},
        {"relativePath", entry.relativePath},
        {"remotePath", entry.remotePath},
        {"modified", entry.modified},
        {"size", double(entry.size)},
        {"directory", entry.directory}
    };
}
bool parseCachedEntry(const QJsonObject &object, OpenList::Entry *entry) {
    if (!object.value("name").isString() || !object.value("relativePath").isString()
        || !object.value("remotePath").isString() || !object.value("directory").isBool()
        || !object.value("size").isDouble()) return false;
    OpenList::Entry parsed;
    parsed.name = object.value("name").toString();
    parsed.relativePath = object.value("relativePath").toString();
    parsed.remotePath = object.value("remotePath").toString();
    parsed.modified = object.value("modified").toString();
    parsed.directory = object.value("directory").toBool();
    if (parsed.name.size() > 200 || !OpenList::safeComponent(parsed.name)) return false;
    if (parsed.relativePath.isEmpty() || parsed.relativePath.startsWith('/')
        || parsed.relativePath.contains('\\') || parsed.relativePath.contains(QStringLiteral("//"))) return false;
    const auto parts = parsed.relativePath.split('/');
    for (const auto &part : parts) if (!OpenList::safeComponent(part)) return false;
    if (parsed.remotePath != remoteRoot() + '/' + parsed.relativePath) return false;
    if (parsed.modified.size() > 128) return false;
    const double size = object.value("size").toDouble();
    if (!std::isfinite(size) || size < -1 || size > 9.0e18) return false;
    parsed.size = static_cast<qint64>(size);
    *entry = parsed;
    return true;
}
} // namespace

namespace {
// redact/safeComponent 会被大目录的成千上万个条目逐个调用，正则必须复用实例。
const QRegularExpression &urlPattern()
{
    static const QRegularExpression re(QStringLiteral("(?:https?://|www\\.)[^\\s<>]+"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}
const QRegularExpression &pathPattern()
{
    static const QRegularExpression re(QStringLiteral("/(?:www|data|storage|sdcard|mnt|home|tmp|var)/[^\\s<>]+"));
    return re;
}
const QRegularExpression &secretParamPattern()
{
    static const QRegularExpression re(QStringLiteral("(?:raw_url|sign|token)\\s*[:=]\\s*[^\\s]+"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}
const QRegularExpression &reservedNamePattern()
{
    static const QRegularExpression re(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}
const QRegularExpression &hashFormatPattern()
{
    static const QRegularExpression re(QStringLiteral("^[a-f0-9]{64}$"));
    return re;
}
} // namespace

QString OpenList::redact(QString text) {
    text.replace(urlPattern(), QStringLiteral("[地址已隐藏]"));
    text.replace(pathPattern(), QStringLiteral("[路径已隐藏]"));
    text.replace(secretParamPattern(), QStringLiteral("[参数已隐藏]"));
    return text.left(4000);
}
bool OpenList::safeComponent(const QString &name) {
    if (name.isEmpty() || name == "." || name == ".." || name.endsWith(' ') || name.endsWith('.') || name.size() > 200) return false;
    for (const QChar ch : name) {
        if (ch.unicode() < 0x20 || QStringLiteral("<>:\"/\\|?*").contains(ch)) return false;
    }
    return !reservedNamePattern().match(name).hasMatch();
}
bool OpenList::safeDirectory(const QString &path, bool create) {
    if (path.isEmpty() || !QDir::isAbsolutePath(path)) return false;
    QString p = QDir::cleanPath(path);
    QStringList missing;
    for (;;) {
        QFileInfo info(p);
        if (info.isSymLink()) return false;
#ifdef Q_OS_WIN
        DWORD attr = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(p).utf16()));
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        if (info.exists() && !info.isDir()) return false;
        if (!info.exists()) missing.prepend(p);
        QString parent = info.absolutePath();
        if (parent == p) break;
        p = parent;
    }
    if (!create && !missing.isEmpty()) return false;
    for (const QString &dir : missing) {
        if (!QDir().mkdir(dir)) return false;
    }
    return true;
}
QString OpenList::resolveDownloadRoot(const QString &applicationDirectory, const QString &localDataDirectory) {
    if (!QDir::isAbsolutePath(applicationDirectory)) return {};
    QDir dir(QDir::cleanPath(applicationDirectory));
    if (dir.dirName().compare("OrangeToolsApp", Qt::CaseInsensitive) == 0) return dir.filePath("download");
    do {
        if (QFileInfo::exists(dir.filePath("OrangeTools.pro"))) return dir.filePath("OrangeToolsApp/download");
    } while (dir.cdUp());
    // SFX extraction directories are temporary; failed installs must survive SFX cleanup.
    // Resolve the location only. Creation and link/write checks still use safeDirectory.
    if (!QDir::isAbsolutePath(localDataDirectory)) return {};
    return QDir(QDir::cleanPath(localDataDirectory)).filePath("download");
}
QString OpenList::defaultDownloadRoot() {
    return resolveDownloadRoot(QCoreApplication::applicationDirPath(),
                               QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
}
QString OpenList::desktopTransferDirectory(const QString &desktopRoot) {
    QString root = desktopRoot;
    if (root.isEmpty()) root = qEnvironmentVariable("ORANGE_TOOLS_DESKTOP_ROOT");
    if (root.isEmpty()) root = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (root.isEmpty()) return {};
    return QDir(QDir::cleanPath(root)).filePath(QStringLiteral("Tools Folder"));
}
QString OpenList::defaultListingCacheFile() {
    if (qEnvironmentVariableIsSet("OPENLIST_DISABLE_LISTING_CACHE")) return {};
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (directory.isEmpty() || !QDir::isAbsolutePath(directory)) return {};
    return QDir(directory).filePath(listingCacheFileName());
}
OpenListService::OpenListService(QObject *parent, QNetworkAccessManager *network, const QString &root,
                                 const QString &cacheFile)
    : QObject(parent), m_root(root.isEmpty() ? OpenList::defaultDownloadRoot() : root),
      m_network(network ? network : new QNetworkAccessManager(this)), m_cacheFile(cacheFile) {
    qRegisterMetaType<OpenList::Entry>(); qRegisterMetaType<QList<OpenList::Entry>>();
}
OpenListService::~OpenListService() {
    disconnect(this, nullptr, nullptr, nullptr);
    cancel(); ++m_listGeneration;
    const auto replies = m_lists; m_lists.clear();
    for (auto r : replies) if (r) r->abort();
}
bool OpenListService::https(const QUrl &url) {
    return url.isValid() && url.scheme().compare("https", Qt::CaseInsensitive) == 0 && !url.host().isEmpty() && url.userInfo().isEmpty();
}
QNetworkRequest OpenListService::request(const QUrl &url) const {
    QNetworkRequest r(url);
    // 蓝奏云等 CDN 会拒绝没有浏览器 UA 的下载请求（返回 405）。
    r.setHeader(QNetworkRequest::UserAgentHeader,
                QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                               "AppleWebKit/537.36 (KHTML, like Gecko) "
                               "Chrome/124.0.0.0 Safari/537.36"));
    r.setRawHeader("Accept", "*/*");
    r.setRawHeader("Accept-Encoding", "identity");
    r.setTransferTimeout(30000);
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    r.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    r.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    r.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    return r;
}
QString OpenListService::sha256(const QJsonObject &o) {
    QJsonObject hashes = o.value("hash_info").toObject();
    if (hashes.isEmpty()) hashes = QJsonDocument::fromJson(o.value("hashinfo").toString().toUtf8()).object();
    for (auto it = hashes.begin(); it != hashes.end(); ++it) {
        QString key = it.key().toLower(); key.remove('-'); key.remove('_');
        QString value = it.value().toString().toLower();
        if (key == "sha256" && hashFormatPattern().match(value).hasMatch()) return value;
    }
    return {};
}
void OpenListService::refresh() {
    if (m_active) return;
    const quint64 generation = ++m_listGeneration;
    const auto replies = m_lists; m_lists.clear();
    for (auto r : replies) if (r) { r->abort(); r->deleteLater(); }
    m_listing = Listing();
    loadCacheOnce();
    if (generation != m_listGeneration) return;
    m_listing.pending.enqueue({remoteRoot(), 1, 0});
    m_listing.visited.insert(remoteRoot());
    pump(generation);
}
void OpenListService::loadCacheOnce() {
    if (m_cacheLoaded) return;
    m_cacheLoaded = true;
    if (m_cacheFile.isEmpty()) return;
    const QFileInfo info(m_cacheFile);
    if (info.isSymLink() || !info.isFile() || info.size() <= 0 || info.size() > maxListingCacheBytes) return;
    QFile file(m_cacheFile);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QByteArray bytes = file.read(maxListingCacheBytes + 1);
    if (bytes.size() > maxListingCacheBytes) return;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return;
    const auto object = document.object();
    if (object.value("version").toInt(-1) != listingCacheVersion) return;
    if (object.value("root").toString() != remoteRoot()) return;
    const auto values = object.value("entries");
    if (!values.isArray() || values.toArray().size() > maxListingCacheEntries) return;
    QList<OpenList::Entry> entries;
    QSet<QString> paths;
    for (const auto &value : values.toArray()) {
        if (!value.isObject()) continue;
        OpenList::Entry entry;
        if (!parseCachedEntry(value.toObject(), &entry) || paths.contains(entry.remotePath)) continue;
        paths.insert(entry.remotePath); entries.append(entry);
    }
    // 非空数组却没有任何合法条目，说明缓存已损坏或被篡改，整份忽略。
    if (!values.toArray().isEmpty() && entries.isEmpty()) return;
    const auto savedValue = object.value("savedAt");
    QDateTime savedAt;
    if (savedValue.isDouble() && std::isfinite(savedValue.toDouble())
        && savedValue.toDouble() > 0 && savedValue.toDouble() <= 9.0e18)
        savedAt = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(savedValue.toDouble()),
                                                  QTimeZone(QTimeZone::UTC));
    if (!savedAt.isValid()) savedAt = info.lastModified().toUTC();
    if (!savedAt.isValid()) savedAt = QDateTime::currentDateTimeUtc();
    m_cachedEntries = entries;
    m_cacheTime = savedAt;
    if (!m_cacheEmitted) {
        m_cacheEmitted = true;
        emit listingCached(entries, savedAt);
    }
}
QList<OpenList::Entry> OpenListService::mergedWithCache(const QList<OpenList::Entry> &entries) const {
    QList<OpenList::Entry> merged;
    QSet<QString> paths;
    for (const auto &entry : m_cachedEntries) {
        if (paths.contains(entry.remotePath)) continue;
        paths.insert(entry.remotePath); merged.append(entry);
    }
    for (const auto &entry : entries) {
        if (paths.contains(entry.remotePath)) continue;
        paths.insert(entry.remotePath); merged.append(entry);
    }
    return merged;
}
void OpenListService::saveCache(const QList<OpenList::Entry> &entries, const QDateTime &savedAt) const {
    if (m_cacheFile.isEmpty()) return;
    QJsonArray values;
    for (const auto &entry : entries) values.append(cachedEntryJson(entry));
    const QJsonObject object{
        {"version", listingCacheVersion},
        {"root", remoteRoot()},
        {"savedAt", double(savedAt.toMSecsSinceEpoch())},
        {"entries", values}
    };
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > maxListingCacheBytes) return;
    const QString directory = QFileInfo(m_cacheFile).absolutePath();
    if (directory.isEmpty() || !QDir().mkpath(directory)) return;
    QSaveFile file(m_cacheFile);
    if (!file.open(QIODevice::WriteOnly)) return;
    if (file.write(bytes) != bytes.size()) { file.cancelWriting(); return; }
    file.commit();
}
void OpenListService::finishListing(quint64 generation) {
    if (generation != m_listGeneration) return;
    auto &listing = m_listing;
    if (listing.finished) return;
    listing.finished = true;
    QList<OpenList::Entry> result = listing.entries;
    if (!listing.error.isEmpty() && !m_cachedEntries.isEmpty()) result = mergedWithCache(listing.entries);
    if (listing.error.isEmpty()) {
        m_cachedEntries = result;
        m_cacheTime = QDateTime::currentDateTimeUtc();
        m_cacheLoaded = true;
        saveCache(result, m_cacheTime);
    }
    emit listingReady(result, listing.error);
}
void OpenListService::pump(quint64 generation) {
    if (generation != m_listGeneration) return;
    auto &listing = m_listing;
    if (listing.finished) return;
    while (listing.active < maxConcurrentListRequests && !listing.pending.isEmpty()) {
        if (++listing.requests > maxListRequests) {
            listing.pending.clear();
            listing.error = QStringLiteral("目录过多，列表不完整");
            break;
        }
        Page page = listing.pending.dequeue();
        ++listing.active;
        auto req = request(QUrl(base + "/api/fs/list")); req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QJsonObject body{{"path", page.path}, {"password", ""}, {"page", page.page},
                         {"per_page", listingPageSize}, {"refresh", false}};
        auto *r = m_network->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        r->setReadBufferSize(maxJson + 1); m_lists.append(r);
        connect(r, &QNetworkReply::readyRead, this, [r]{ if (r->bytesAvailable() > maxJson) r->abort(); });
        connect(r, &QNetworkReply::finished, this, [this, r, page, generation] {
            r->deleteLater(); m_lists.removeAll(r);
            if (generation != m_listGeneration) return;
            auto &list = m_listing;
            --list.active;
            QList<OpenList::Entry> added;
            QJsonObject data;
            if (!apiObject(r, &data)) list.error = QStringLiteral("部分目录读取失败，列表不完整");
            else {
                const auto value = data.value("content");
                if (!value.isArray() && !value.isNull()) list.error = QStringLiteral("资源列表格式无效");
                const auto content = value.toArray();
                for (const auto &v : content) {
                    const auto o = v.toObject(); const QString name = o.value("name").toString();
                    if (!OpenList::safeComponent(name)) { list.error = QStringLiteral("部分文件名不安全，已跳过"); continue; }
                    const QString full = page.path + '/' + name;
                    if (o.value("is_dir").toBool()) {
                        if (page.depth >= maxListingDepth) { list.error = QStringLiteral("目录层级过深，列表不完整"); continue; }
                        if (!list.visited.contains(full)) {
                            list.visited.insert(full); list.pending.enqueue({full, 1, page.depth + 1});
                            OpenList::Entry folder; folder.name = name; folder.remotePath = full;
                            folder.relativePath = full.mid(remoteRoot().size() + 1);
                            folder.modified = o.value("modified").toString(); folder.directory = true;
                            if (list.entries.size() < maxListingEntries) { list.entries.append(folder); added.append(folder); }
                            else list.error = QStringLiteral("资源过多，列表不完整");
                        }
                    } else {
                        OpenList::Entry entry;
                        entry.name = name; entry.remotePath = full;
                        entry.relativePath = full.mid(remoteRoot().size() + 1);
                        entry.size = static_cast<qint64>(o.value("size").toDouble(-1));
                        entry.modified = o.value("modified").toString();
                        if (list.entries.size() < maxListingEntries) { list.entries.append(entry); added.append(entry); }
                        else list.error = QStringLiteral("文件过多，列表不完整");
                    }
                }
                const int total = data.value("total").toInt();
                if (!content.isEmpty() && page.page * listingPageSize < total)
                    list.pending.enqueue({page.path, page.page + 1, page.depth});
            }
            if (!added.isEmpty()) emit listingUpdated(added);
            if (generation != m_listGeneration) return;
            pump(generation);
        });
    }
    if (listing.active == 0 && listing.pending.isEmpty()) finishListing(generation);
}
QString OpenListService::destinationDirectory(const OpenList::Entry &entry, const QString &directory) const {
    const QStringList parts = entry.relativePath.split('/');
    if (parts.isEmpty()) return {};
    for (const auto &part : parts) if (!OpenList::safeComponent(part)) return {};
    if (!directory.isEmpty()) return QDir::cleanPath(directory);
    QString dir = QDir(m_root).filePath(QStringLiteral("软件"));
    for (int i = 0; i + 1 < parts.size(); ++i) dir += '/' + parts[i];
    return dir;
}
QString OpenListService::existingLocalFile(const OpenList::Entry &entry, const QString &directory) const {
    if (entry.directory || entry.relativePath.isEmpty()) return {};
    if (!entry.remotePath.startsWith(remoteRoot() + '/') ||
        entry.remotePath != remoteRoot() + '/' + entry.relativePath) return {};
    const QString dir = destinationDirectory(entry, directory);
    if (dir.isEmpty()) return {};
    const QString path = QDir(dir).filePath(entry.relativePath.section('/', -1));
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || !OpenList::safeDirectory(dir, false)) return {};
    return info.absoluteFilePath();
}
bool OpenListService::prepareFile() {
    const QStringList parts = m_entry.relativePath.split('/');
    if (parts.isEmpty()) return false;
    for (const auto &part : parts) if (!OpenList::safeComponent(part)) return false;
    const QString dir = destinationDirectory(m_entry, m_targetDirectory);
    if (dir.isEmpty() || !OpenList::safeDirectory(dir, true)) return false;
    const QString name = parts.last();
    m_path = QDir(dir).filePath(name);
    int suffix = 1;
    while (QFileInfo::exists(m_path) || QFileInfo(m_path).isSymLink()) {
        if (suffix > 10000) return false;
        const QString extension = QFileInfo(name).suffix();
        m_path = QDir(dir).filePath(QFileInfo(name).completeBaseName() + QString(" (%1)").arg(suffix++) +
            (extension.isEmpty() ? QString() : '.' + extension));
    }
    m_part = m_path + '.' + QUuid::createUuid().toString(QUuid::Id128) + ".part";
    m_file.setFileName(m_part);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        m_part.clear(); // We do not own a file that could not be exclusively created.
        return false;
    }
    return true;
}
bool OpenListService::download(const OpenList::Entry &entry, const QString &directory) {
    if (m_active || entry.directory) return false;
    m_targetDirectory = directory;
    m_active = true; const auto generation = ++m_downloadGeneration;
    m_parallel = false; m_parallelFallback = false; m_segments.clear(); m_parallelReceived = 0;
    m_entry = entry; m_path.clear(); m_part.clear(); m_hash.reset(); m_expectedHash.clear(); m_responseSize = -1;
    if (!entry.remotePath.startsWith(remoteRoot() + '/') || entry.remotePath != remoteRoot() + '/' + entry.relativePath || !prepareFile()) {
        fail(QStringLiteral("下载目录不可用或文件路径不安全。")); return false;
    }
    auto req = request(QUrl(base + "/api/fs/get")); req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto *r = m_network->post(req, QJsonDocument(QJsonObject{{"path",entry.remotePath},{"password",""}}).toJson(QJsonDocument::Compact));
    m_reply = r; r->setReadBufferSize(maxJson + 1);
    connect(r, &QNetworkReply::readyRead, this, [r]{ if (r->bytesAvailable() > maxJson) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [this,r,generation] {
        r->deleteLater(); if (!m_active || generation != m_downloadGeneration) return;
        m_reply = nullptr; QJsonObject data;
        if (!apiObject(r,&data) || data.value("is_dir").toBool()) { fail(QStringLiteral("无法读取下载信息。")); return; }
        const QUrl url(data.value("raw_url").toString());
        if (!https(url)) { fail(QStringLiteral("下载地址不是安全的 HTTPS 地址，已拒绝。")); return; }
        m_entry.size = static_cast<qint64>(data.value("size").toDouble(-1));
        if (m_entry.size < 0) { fail(QStringLiteral("服务器未提供有效文件大小。")); return; }
        const QStorageInfo storage(QFileInfo(m_path).absolutePath());
        if (storage.isValid() && storage.isReady() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < m_entry.size) {
            fail(QStringLiteral("下载目录所在磁盘空间不足。")); return;
        }
        m_expectedHash = sha256(data).toLatin1();
        if (!m_expectedHash.isEmpty()) emit log(QStringLiteral("下载后将校验发布方 SHA-256。"));
        m_clock.start(); getFile(url,0,generation);
    });
    return true;
}
bool OpenListService::downloadDirect(const OpenList::Entry &entry, const QUrl &url) {
    if (m_active || entry.directory || entry.size < 0 || !https(url)) return false;
    m_targetDirectory.clear();
    m_active = true;
    const auto generation = ++m_downloadGeneration;
    m_parallel = false; m_parallelFallback = false; m_segments.clear(); m_parallelReceived = 0;
    m_entry = entry; m_path.clear(); m_part.clear(); m_hash.reset(); m_expectedHash.clear(); m_responseSize = -1;
    if (!prepareFile()) { fail(QStringLiteral("下载目录不可用或文件路径不安全。")); return false; }
    const QStorageInfo storage(QFileInfo(m_path).absolutePath());
    if (storage.isValid() && storage.isReady() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < entry.size) {
        fail(QStringLiteral("下载目录所在磁盘空间不足。")); return false;
    }
    m_clock.start(); getFile(url, 0, generation);
    return true;
}
void OpenListService::adoptDownloadSize(qint64 total) {
    if (total <= 0) return;
    m_responseSize = total;
    if (m_entry.size == total) return;
    m_entry.size = total;
    emit log(QStringLiteral("服务器文件大小与目录信息不一致，已按实际大小下载。"));
}
void OpenListService::probeRange(const QUrl &url, int redirects, quint64 generation) {
    if (!m_active || generation != m_downloadGeneration) return;
    if (!https(url) || redirects > 5) { fail(QStringLiteral("下载重定向不安全或次数过多。")); return; }
    QNetworkRequest req = request(url);
    req.setRawHeader("Range", "bytes=0-0");
    auto *reply = m_network->get(req);
    m_reply = reply;
    reply->setReadBufferSize(1024);
    // 蓝奏云等 CDN 会按 UA/防盗链返回 405，且目录中的文件大小可能与实际
    // Content-Length 不一致。先用 1 字节 Range 探测真实大小和分段能力。
    const auto handle = [this, reply, url, redirects, generation] {
        if (m_reply != reply || !m_active || generation != m_downloadGeneration) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 0) return; // 响应头尚未完整，等待下一次信号。
        m_reply = nullptr;
        if (status >= 300 && status < 400) {
            const QUrl next = url.resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
            reply->abort(); reply->deleteLater();
            if (!https(next)) { fail(QStringLiteral("下载重定向不是安全的 HTTPS 地址。")); return; }
            probeRange(next, redirects + 1, generation);
            return;
        }
        qint64 total = 0;
        if (status == 206) {
            static const QRegularExpression contentRange(QStringLiteral("^bytes 0-0/([0-9]+)$"),
                                                         QRegularExpression::CaseInsensitiveOption);
            const auto match = contentRange.match(QString::fromLatin1(reply->rawHeader("Content-Range")).trimmed());
            if (match.hasMatch()) total = match.captured(1).toLongLong();
        }
        reply->abort(); reply->deleteLater();
        if (total > 0) {
            adoptDownloadSize(total);
            startParallel(url, generation);
        } else {
            getFile(url, 0, generation, false);
        }
    };
    connect(reply, &QNetworkReply::metaDataChanged, this, handle);
    connect(reply, &QNetworkReply::readyRead, this, handle);
    connect(reply, &QNetworkReply::finished, this, handle);
}
void OpenListService::startParallel(const QUrl &url, quint64 generation) {
    m_parallel = true;
    m_parallelFallback = false;
    m_parallelReceived = 0;
    m_segments.clear();
    m_responseSize = m_entry.size;
    const int count = qMin(maxParallelRequests,
                           qMax(2, int((m_entry.size + parallelChunk - 1) / parallelChunk)));
    if (!m_file.resize(m_entry.size)) {
        m_parallel = false;
        fail(QStringLiteral("下载文件预分配失败，请检查磁盘空间。"));
        return;
    }
    m_segments.resize(count);
    const qint64 baseSize = m_entry.size / count;
    const qint64 remainder = m_entry.size % count;
    for (int i = 0; i < count; ++i) {
        auto &segment = m_segments[i];
        segment.start = i * baseSize + qMin<qint64>(i, remainder);
        segment.end = segment.start + baseSize - 1 + (i < remainder ? 1 : 0);
        QNetworkRequest req = request(url);
        req.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(segment.start) + '-' + QByteArray::number(segment.end));
        auto *reply = m_network->get(req);
        segment.reply = reply;
        reply->setReadBufferSize(4 * 1024 * 1024);
        connect(reply, &QNetworkReply::readyRead, this, [this, reply, i, url, generation] {
            if (!m_active || generation != m_downloadGeneration || !m_parallel || m_parallelFallback) return;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 200) { fallbackToSingleStream(url, generation); return; }
            if (status == 206) {
                const auto expected = QByteArray("bytes ") + QByteArray::number(m_segments[i].start) + '-' + QByteArray::number(m_segments[i].end) + '/' + QByteArray::number(m_entry.size);
                if (reply->rawHeader("Content-Range") != expected) { fallbackToSingleStream(url, generation); return; }
                consumeSegment(i, reply);
            }
        });
        connect(reply, &QNetworkReply::downloadProgress, this, [this, generation](qint64, qint64) {
            if (m_active && generation == m_downloadGeneration && m_parallel)
                emit progress(m_parallelReceived, m_entry.size,
                              m_parallelReceived * 1000.0 / qMax<qint64>(1, m_clock.elapsed()));
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, i, url, generation] {
            reply->deleteLater();
            if (!m_active || generation != m_downloadGeneration || !m_parallel || m_parallelFallback) return;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 200 || (status >= 300 && status < 400)) { fallbackToSingleStream(url, generation); return; }
            if (reply->error() != QNetworkReply::NoError || status != 206) {
                fallbackToSingleStream(url, generation); return;
            }
            const auto expected = QByteArray("bytes ") + QByteArray::number(m_segments[i].start) + '-' + QByteArray::number(m_segments[i].end) + '/' + QByteArray::number(m_entry.size);
            if (reply->rawHeader("Content-Range") != expected) {
                fallbackToSingleStream(url, generation); return;
            }
            if (!consumeSegment(i, reply)) return;
            auto &segment = m_segments[i];
            if (segment.received != segment.end - segment.start + 1) {
                fallbackToSingleStream(url, generation); return;
            }
            segment.finished = true;
            for (const auto &part : std::as_const(m_segments)) if (!part.finished) return;
            m_parallel = false;
            if (!m_file.flush()) { fail(QStringLiteral("文件写入失败，请检查磁盘空间。")); return; }
            m_file.close();
            m_hash.reset();
            QFile verify(m_part);
            if (!verify.open(QIODevice::ReadOnly)) { fail(QStringLiteral("无法校验下载文件。")); return; }
            while (!verify.atEnd()) m_hash.addData(verify.read(4 * 1024 * 1024));
            verify.close();
            if (!m_expectedHash.isEmpty() && m_hash.result().toHex() != m_expectedHash) {
                fail(QStringLiteral("SHA-256 校验失败，禁止安装。")); return;
            }
            if (!OpenList::safeDirectory(QFileInfo(m_path).absolutePath(), false) || QFileInfo::exists(m_path) || !QFile::rename(m_part, m_path)) {
                fail(QStringLiteral("下载文件保存失败，未覆盖既有文件。")); return;
            }
            m_part.clear(); m_targetDirectory.clear(); m_active = false;
            emit ready(m_entry, m_path);
        });
    }
}
void OpenListService::fallbackToSingleStream(const QUrl &url, quint64 generation) {
    if (!m_parallel || m_parallelFallback || !m_active || generation != m_downloadGeneration) return;
    m_parallelFallback = true;
    const auto replies = m_segments;
    for (const auto &segment : replies) if (segment.reply) segment.reply->abort();
    m_segments.clear();
    m_parallel = false;
    if (!m_file.resize(0) || !m_file.seek(0)) {
        fail(QStringLiteral("无法切换下载模式。")); return;
    }
    m_hash.reset();
    m_responseSize = -1;
    emit log(QStringLiteral("服务器未按分段返回数据，已切换为单线程下载。"));
    getFile(url, 0, generation, false);
    m_parallelFallback = false;
}
bool OpenListService::consumeSegment(int index, QNetworkReply *reply) {
    if (index < 0 || index >= m_segments.size()) return false;
    auto &segment = m_segments[index];
    const QByteArray bytes = reply->readAll();
    const qint64 expected = segment.end - segment.start + 1;
    if (segment.received + bytes.size() > expected || !m_file.seek(segment.start + segment.received) || m_file.write(bytes) != bytes.size()) {
        fail(QStringLiteral("下载大小异常或磁盘写入失败。")); return false;
    }
    segment.received += bytes.size();
    m_parallelReceived += bytes.size();
    return true;
}
void OpenListService::getFile(const QUrl &url, int redirects, quint64 generation, bool allowParallel) {
    if (!https(url) || redirects > 5) { fail(QStringLiteral("下载重定向不安全或次数过多。")); return; }
    if (allowParallel && m_entry.size >= parallelThreshold && redirects == 0 && !m_parallelFallback) {
        probeRange(url, 0, generation);
        return;
    }
    auto *r = m_network->get(request(url)); m_reply = r; r->setReadBufferSize(4 * 1024 * 1024);
    connect(r,&QNetworkReply::readyRead,this,[this,r,generation] {
        if (!m_active || generation != m_downloadGeneration || r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
        const qint64 total = r->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (total > 0) adoptDownloadSize(total);
        consume(r);
    });
    connect(r,&QNetworkReply::downloadProgress,this,[this,generation](qint64 received,qint64 total) {
        if (!m_active || generation != m_downloadGeneration) return;
        if (total > 0) adoptDownloadSize(total);
        emit progress(received,total,received * 1000.0 / qMax<qint64>(1,m_clock.elapsed()));
    });
    connect(r,&QNetworkReply::finished,this,[this,r,url,redirects,generation,allowParallel] {
        r->deleteLater(); if (!m_active || generation != m_downloadGeneration) return;
        m_reply = nullptr;
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300 && status < 400) {
            if (r->error() != QNetworkReply::NoError) { fail(QStringLiteral("下载请求失败。")); return; }
            const QUrl next = url.resolved(r->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
            if (!https(next)) { fail(QStringLiteral("下载重定向不是安全的 HTTPS 地址。")); return; }
            getFile(next,redirects+1,generation,allowParallel); return;
        }
        if (r->error() != QNetworkReply::NoError || status != 200) { fail(QStringLiteral("下载失败：网络连接或服务器响应异常。")); return; }
        if (!consume(r)) return;
        if (!m_file.flush()) { fail(QStringLiteral("文件写入失败，请检查磁盘空间。")); return; }
        const auto size = m_file.size(); m_file.close();
        const qint64 expected = m_responseSize > 0 ? m_responseSize : m_entry.size;
        if (expected > 0 && size != expected) { fail(QStringLiteral("下载文件大小不匹配。")); return; }
        if (m_entry.size <= 0) m_entry.size = size;
        if (!m_expectedHash.isEmpty() && m_hash.result().toHex() != m_expectedHash) { fail(QStringLiteral("SHA-256 校验失败，禁止安装。")); return; }
        if (!OpenList::safeDirectory(QFileInfo(m_path).absolutePath(),false) || QFileInfo::exists(m_path) || !QFile::rename(m_part,m_path)) {
            fail(QStringLiteral("下载文件保存失败，未覆盖既有文件。")); return;
        }
        m_part.clear(); m_targetDirectory.clear(); m_active = false;
        emit ready(m_entry,m_path);
    });
}
bool OpenListService::consume(QNetworkReply *r) {
    const auto bytes = r->readAll();
    const qint64 expected = m_responseSize > 0 ? m_responseSize : m_entry.size;
    if (expected > 0 && m_file.size() + bytes.size() > expected) {
        fail(QStringLiteral("下载大小异常或磁盘写入失败。")); return false;
    }
    if (m_file.write(bytes) != bytes.size()) {
        fail(QStringLiteral("下载大小异常或磁盘写入失败。")); return false;
    }
    if (m_responseSize <= 0 && m_entry.size <= 0) m_entry.size = m_file.size();
    m_hash.addData(bytes); return true;
}
void OpenListService::fail(const QString &reason) {
    if (!m_active) return;
    m_active = false; ++m_downloadGeneration;
    for (const auto &segment : std::as_const(m_segments)) if (segment.reply) segment.reply->abort();
    m_segments.clear(); m_parallel = false; m_parallelFallback = false;
    if (m_reply) { auto r = m_reply; m_reply = nullptr; r->abort(); r->deleteLater(); }
    m_file.close();
    QString message = reason;
    if (!m_part.isEmpty() && OpenList::safeDirectory(QFileInfo(m_part).absolutePath(),false) && QFileInfo::exists(m_part) && !QFile::remove(m_part)) message += QStringLiteral(" 未完成文件清理失败。");
    m_part.clear(); m_targetDirectory.clear();
    emit failed(message);
}
void OpenListService::cancel() { fail(QStringLiteral("下载已取消，未执行安装。")); }

