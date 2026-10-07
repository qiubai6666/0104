#include "openlistservice.h"
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QUuid>
#include <QStorageInfo>
#include <QRegularExpression>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
const QString base = QStringLiteral("https://pan.xn--ucy.xyz");
QString remoteRoot(bool module) { return module ? QStringLiteral("/模块") : QStringLiteral("/APK"); }
constexpr qint64 maxJson = 4 * 1024 * 1024;
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
}

QString OpenList::redact(QString text) {
    text.replace(QRegularExpression("(?:https?://|www\\.)[^\\s<>]+", QRegularExpression::CaseInsensitiveOption), QStringLiteral("[地址已隐藏]"));
    text.replace(QRegularExpression("/(?:www|data|storage|sdcard|mnt|home|tmp|var)/[^\\s<>]+"), QStringLiteral("[路径已隐藏]"));
    text.replace(QRegularExpression("(?:raw_url|sign|token)\\s*[:=]\\s*[^\\s]+", QRegularExpression::CaseInsensitiveOption), QStringLiteral("[参数已隐藏]"));
    return text.left(4000);
}
bool OpenList::safeComponent(const QString &name) {
    if (name.isEmpty() || name == "." || name == ".." || name.endsWith(' ') || name.endsWith('.') || name.size() > 200) return false;
    if (name.contains(QRegularExpression("[\\x00-\\x1f<>:\"/\\\\|?*]"))) return false;
    return !QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)", QRegularExpression::CaseInsensitiveOption).match(name).hasMatch();
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
QString OpenList::defaultDownloadRoot() {
    QDir dir(QCoreApplication::applicationDirPath());
    if (dir.dirName().compare("OrangeToolsApp", Qt::CaseInsensitive) == 0) return dir.filePath("download");
    do {
        if (QFileInfo::exists(dir.filePath("OrangeTools.pro"))) return dir.filePath("OrangeToolsApp/download");
    } while (dir.cdUp());
    return {}; // Never silently write to another application or user directory.
}
OpenListService::OpenListService(QObject *parent, QNetworkAccessManager *network, const QString &root)
    : QObject(parent), m_root(root.isEmpty() ? OpenList::defaultDownloadRoot() : root),
      m_network(network ? network : new QNetworkAccessManager(this)) {
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
        if (key == "sha256" && QRegularExpression("^[a-f0-9]{64}$").match(value).hasMatch()) return value;
    }
    return {};
}
void OpenListService::refresh() {
    if (m_active) return;
    const quint64 generation = ++m_listGeneration;
    const auto replies = m_lists; m_lists.clear();
    for (auto r : replies) if (r) { r->abort(); r->deleteLater(); }
    for (int i = 0; i < 2; ++i) {
        m_listing[i] = Listing();
        m_listing[i].pending.enqueue({remoteRoot(i), 1, 0});
        m_listing[i].visited.insert(remoteRoot(i));
        pump(i, generation);
    }
}
void OpenListService::pump(bool module, quint64 generation) {
    auto &listing = m_listing[module];
    if (listing.pending.isEmpty()) { emit listingReady(module, listing.entries, listing.error); return; }
    if (++listing.requests > 2048) {
        listing.pending.clear(); listing.error = QStringLiteral("目录过多，列表不完整");
        emit listingReady(module, listing.entries, listing.error); return;
    }
    Page page = listing.pending.dequeue();
    auto req = request(QUrl(base + "/api/fs/list")); req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QJsonObject body{{"path", page.path}, {"password", ""}, {"page", page.page}, {"per_page", 100}, {"refresh", false}};
    auto *r = m_network->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    r->setReadBufferSize(maxJson + 1); m_lists.append(r);
    connect(r, &QNetworkReply::readyRead, this, [r]{ if (r->bytesAvailable() > maxJson) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [this, r, page, module, generation] {
        r->deleteLater(); m_lists.removeAll(r);
        if (generation != m_listGeneration) return;
        auto &list = m_listing[module]; QJsonObject data;
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
                    if (page.depth >= 32) { list.error = QStringLiteral("目录层级过深，列表不完整"); continue; }
                    if (!list.visited.contains(full)) {
                        list.visited.insert(full); list.pending.enqueue({full, 1, page.depth + 1});
                        OpenList::Entry folder; folder.name = name; folder.remotePath = full;
                        folder.relativePath = full.mid(remoteRoot(module).size() + 1);
                        folder.modified = o.value("modified").toString(); folder.module = module; folder.directory = true;
                        if (list.entries.size() < 50000) list.entries.append(folder);
                        else list.error = QStringLiteral("资源过多，列表不完整");
                    }
                } else if (name.endsWith(module ? ".zip" : ".apk", Qt::CaseInsensitive)) {
                    OpenList::Entry entry;
                    entry.name = name; entry.remotePath = full;
                    entry.relativePath = full.mid(remoteRoot(module).size() + 1);
                    entry.size = static_cast<qint64>(o.value("size").toDouble(-1));
                    entry.modified = o.value("modified").toString(); entry.module = module;
                    if (list.entries.size() < 50000) list.entries.append(entry);
                    else list.error = QStringLiteral("文件过多，列表不完整");
                }
            }
            const int total = data.value("total").toInt();
            if (!content.isEmpty() && page.page * 100 < total) list.pending.enqueue({page.path, page.page + 1, page.depth});
        }
        pump(module, generation);
    });
}
bool OpenListService::prepareFile() {
    const QStringList parts = m_entry.relativePath.split('/');
    if (parts.isEmpty()) return false;
    for (const auto &part : parts) if (!OpenList::safeComponent(part)) return false;
    QString dir = QDir(m_root).filePath(m_entry.module ? QStringLiteral("模块") : QStringLiteral("软件"));
    for (int i = 0; i + 1 < parts.size(); ++i) dir += '/' + parts[i];
    if (!OpenList::safeDirectory(dir, true)) return false;
    const QString name = parts.last();
    m_path = QDir(dir).filePath(name);
    int suffix = 1;
    while (QFileInfo::exists(m_path) || QFileInfo(m_path).isSymLink()) {
        if (suffix > 10000) return false;
        m_path = QDir(dir).filePath(QFileInfo(name).completeBaseName() + QString(" (%1).").arg(suffix++) + QFileInfo(name).suffix());
    }
    m_part = m_path + '.' + QUuid::createUuid().toString(QUuid::Id128) + ".part";
    m_file.setFileName(m_part);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        m_part.clear(); // We do not own a file that could not be exclusively created.
        return false;
    }
    return true;
}
bool OpenListService::download(const OpenList::Entry &entry) {
    if (m_active || entry.directory) return false;
    m_active = true; const auto generation = ++m_downloadGeneration;
    m_entry = entry; m_path.clear(); m_part.clear(); m_hash.reset(); m_expectedHash.clear();
    if (!entry.remotePath.startsWith(remoteRoot(entry.module) + '/') || entry.remotePath != remoteRoot(entry.module) + '/' + entry.relativePath || !prepareFile()) {
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
        emit log(m_expectedHash.isEmpty() ? QStringLiteral("未提供发布方 SHA-256，仅验证传输大小及安装包格式。") : QStringLiteral("下载后将校验发布方 SHA-256。"));
        m_clock.start(); getFile(url,0,generation);
    });
    return true;
}
void OpenListService::getFile(const QUrl &url, int redirects, quint64 generation) {
    if (!https(url) || redirects > 5) { fail(QStringLiteral("下载重定向不安全或次数过多。")); return; }
    auto *r = m_network->get(request(url)); m_reply = r; r->setReadBufferSize(256 * 1024);
    connect(r,&QNetworkReply::readyRead,this,[this,r,generation] {
        if (m_active && generation == m_downloadGeneration && r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) consume(r);
    });
    connect(r,&QNetworkReply::downloadProgress,this,[this,generation](qint64 received,qint64 total) {
        if (m_active && generation == m_downloadGeneration)
            emit progress(received,total,received * 1000.0 / qMax<qint64>(1,m_clock.elapsed()));
    });
    connect(r,&QNetworkReply::finished,this,[this,r,url,redirects,generation] {
        r->deleteLater(); if (!m_active || generation != m_downloadGeneration) return;
        m_reply = nullptr;
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300 && status < 400) {
            if (r->error() != QNetworkReply::NoError) { fail(QStringLiteral("下载请求失败。")); return; }
            getFile(url.resolved(r->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl()),redirects+1,generation); return;
        }
        if (r->error() != QNetworkReply::NoError || status != 200) { fail(QStringLiteral("下载失败：网络连接或服务器响应异常。")); return; }
        if (!consume(r)) return;
        if (!m_file.flush()) { fail(QStringLiteral("文件写入失败，请检查磁盘空间。")); return; }
        const auto size = m_file.size(); m_file.close();
        if (size != m_entry.size) { fail(QStringLiteral("下载文件大小不匹配。")); return; }
        if (!m_expectedHash.isEmpty() && m_hash.result().toHex() != m_expectedHash) { fail(QStringLiteral("SHA-256 校验失败，禁止安装。")); return; }
        if (!OpenList::safeDirectory(QFileInfo(m_path).absolutePath(),false) || QFileInfo::exists(m_path) || !QFile::rename(m_part,m_path)) {
            fail(QStringLiteral("下载文件保存失败，未覆盖既有文件。")); return;
        }
        m_part.clear(); m_active = false;
        emit ready(m_entry,m_path);
    });
}
bool OpenListService::consume(QNetworkReply *r) {
    const auto bytes = r->readAll();
    if (m_file.size() + bytes.size() > m_entry.size || m_file.write(bytes) != bytes.size()) {
        fail(QStringLiteral("下载大小异常或磁盘写入失败。")); return false;
    }
    m_hash.addData(bytes); return true;
}
void OpenListService::fail(const QString &reason) {
    if (!m_active) return;
    m_active = false; ++m_downloadGeneration;
    if (m_reply) { auto r = m_reply; m_reply = nullptr; r->abort(); r->deleteLater(); }
    m_file.close();
    QString message = reason;
    if (!m_part.isEmpty() && OpenList::safeDirectory(QFileInfo(m_part).absolutePath(),false) && QFileInfo::exists(m_part) && !QFile::remove(m_part)) message += QStringLiteral(" 未完成文件清理失败。");
    m_part.clear(); emit failed(message);
}
void OpenListService::cancel() { fail(QStringLiteral("下载已取消，未执行安装。")); }

