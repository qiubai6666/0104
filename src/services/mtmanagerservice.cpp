#include "mtmanagerservice.h"
#include "openlistservice.h"
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUuid>

namespace {
const QUrl kPage(QStringLiteral("https://mt.cc/download/"));
const qint64 kMaxPage = 4 * 1024 * 1024;
}

MtManagerService::MtManagerService(QObject *parent, QNetworkAccessManager *network, const QString &downloadRoot)
    : QObject(parent), m_network(network ? network : new QNetworkAccessManager(this)),
      m_root(downloadRoot.isEmpty() ? OpenList::defaultDownloadRoot() : downloadRoot) {
    m_downloader = new OpenListService(this, m_network, m_root);
    connect(m_downloader, &OpenListService::progress, this, &MtManagerService::progress);
    connect(m_downloader, &OpenListService::failed, this, [this](const QString &message) {
        if (m_active) finishFailure(message);
    });
    connect(m_downloader, &OpenListService::ready, this, [this](const OpenList::Entry &, const QString &file) {
        if (!m_active) return;
        QFile apk(file);
        const bool valid = apk.open(QIODevice::ReadOnly) && apk.read(4) == QByteArray("PK\x03\x04", 4);
        apk.close();
        if (!valid) {
            const bool removed = QFile::remove(file);
            finishFailure(removed ? QStringLiteral("下载内容不是有效 APK。") :
                QStringLiteral("下载内容不是有效 APK，清理失败，文件保留：%1").arg(file));
            return;
        }
        m_path = file; m_active = false;
        emit log(QStringLiteral("MT 管理器下载完成。")); emit ready(m_version, file);
    });
}

MtManagerService::~MtManagerService() {
    m_active = false; m_downloader->cancel();
    if (m_reply) { disconnect(m_reply, nullptr, this, nullptr); m_reply->abort(); m_reply->deleteLater(); }
    m_file.close();
    if (!m_part.isEmpty()) QFile::remove(m_part);
}

void MtManagerService::start() {
    if (m_active) return;
    m_active = true; m_received = 0; m_total = -1; m_version.clear(); m_part.clear(); m_path.clear();
    emit log(QStringLiteral("正在获取 MT 管理器最新版本…"));
    fetchPage();
}
void MtManagerService::fetchPage() {
    QNetworkRequest req(kPage);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OrangeTools/1.0"));
    req.setTransferTimeout(30000);
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    m_reply = m_network->get(req);
    connect(m_reply, &QNetworkReply::finished, this, [this] { if (m_reply) parsePage(m_reply); });
}
bool MtManagerService::validDownloadUrl(const QUrl &url) {
    static const QRegularExpression pathPattern(QStringLiteral("^/apk/[0-9]{6,}/clone-target28/?$"));
    return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 &&
           url.host().compare(QStringLiteral("pan.mt2.cn"), Qt::CaseInsensitive) == 0 &&
           pathPattern.match(url.path()).hasMatch();
}
void MtManagerService::parsePage(QNetworkReply *reply) {
    // Retire the page reply BEFORE creating the APK reply. Page cleanup must
    // never reset the next request; otherwise its data/completion callbacks stall.
    if (!m_active || m_reply != reply) return;
    m_reply = nullptr;
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) { finishFailure(QStringLiteral("获取 MT 管理器版本失败。")); return; }
    const QByteArray body = reply->read(kMaxPage + 1);
    if (body.size() > kMaxPage) { finishFailure(QStringLiteral("版本页面过大，已停止处理。")); return; }
    const QString html = QString::fromUtf8(body);
    QRegularExpression re(QStringLiteral(R"((?:https://pan\.mt2\.cn)?/apk/([0-9]{6,})/clone-target28/?(?:[^"'<>\s]*)?)"), QRegularExpression::CaseInsensitiveOption);
    auto match = re.match(html);
    if (!match.hasMatch()) { finishFailure(QStringLiteral("未找到“本地下载（共存版 TargetSdk28）”的最新版本。")); return; }
    const QString version = match.captured(1);
    const QUrl url(match.captured(0).startsWith(QStringLiteral("https://")) ? match.captured(0) : QStringLiteral("https://pan.mt2.cn") + match.captured(0));
    if (!validDownloadUrl(url)) { finishFailure(QStringLiteral("MT 下载链接不符合安全要求。")); return; }
    m_version = version; emit versionReady(version); emit log(QStringLiteral("已获取 MT 管理器版本：%1").arg(version));
    startDownload(url, version);
}
QString MtManagerService::uniqueApkPath(const QString &version) const {
    const QString dir = QDir(m_root).filePath(QStringLiteral("软件"));
    if (!OpenList::safeDirectory(dir, true)) return {};
    QString base = QDir(dir).filePath(QStringLiteral("MT管理器-%1.apk").arg(version));
    if (!QFileInfo::exists(base)) return base;
    for (int i = 1; i < 100000; ++i) {
        const QString candidate = QDir(dir).filePath(QStringLiteral("MT管理器-%1 (%2).apk").arg(version).arg(i));
        if (!QFileInfo::exists(candidate)) return candidate;
    }
    return {};
}
void MtManagerService::startDownload(const QUrl &url, const QString &version) {
    m_path = uniqueApkPath(version);
    if (m_path.isEmpty()) { finishFailure(QStringLiteral("下载目录不可写或无法生成唯一文件名。")); return; }
    m_part = m_path + QStringLiteral(".") + QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".part");
    m_file.setFileName(m_part);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { m_part.clear(); finishFailure(QStringLiteral("无法创建 MT 管理器下载文件。")); return; }
    QNetworkRequest req(url); req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OrangeTools/1.0"));
    req.setTransferTimeout(30000);
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    req.setRawHeader("Accept-Encoding", "identity");
    req.setRawHeader("Range", "bytes=0-0");
    m_reply = m_network->get(req); m_clock.start();
    connect(m_reply, &QNetworkReply::metaDataChanged, this, [this] {
        if (!m_reply || useParallel(m_reply)) return;
        const QVariant length = m_reply->header(QNetworkRequest::ContentLengthHeader);
        if (length.isValid() && length.toLongLong() > 0) {
            m_total = length.toLongLong();
            emit progress(m_received, m_total, m_clock.elapsed() > 0 ? m_received * 1000.0 / m_clock.elapsed() : 0);
        }
    });
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (!m_reply || useParallel(m_reply)) return;
        const auto data = m_reply->readAll();
        if (!data.isEmpty()) {
            if (m_file.write(data) != data.size()) { finishFailure(QStringLiteral("下载文件写入失败，请检查磁盘空间及占用情况。")); return; }
            m_received += data.size();
            emit progress(m_received, m_total, m_clock.elapsed() > 0 ? m_received * 1000.0 / m_clock.elapsed() : 0);
        }
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total > 0) m_total = total;
        const qint64 shown = qMax(m_received, received);
        emit progress(shown, m_total, m_clock.elapsed() > 0 ? shown * 1000.0 / m_clock.elapsed() : 0);
    });
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        if (!m_reply || useParallel(m_reply)) return;
        const auto reply = m_reply;
        const bool ok = reply->error() == QNetworkReply::NoError;
        const auto tail = reply->readAll();
        if (!tail.isEmpty()) {
            if (m_file.write(tail) != tail.size()) {
                finishFailure(QStringLiteral("下载文件写入失败，请检查磁盘空间及占用情况。"));
                return;
            }
            m_received += tail.size();
        }
        reply->deleteLater(); m_reply=nullptr;
        if (!m_file.flush()) { finishFailure(QStringLiteral("下载文件保存失败，请检查磁盘空间及占用情况。")); return; }
        m_file.close();
        if (!ok || (m_total > 0 && m_received != m_total) || m_received < 4 || !QFileInfo(m_part).isFile()) { finishFailure(QStringLiteral("MT 管理器下载失败或文件大小不完整。")); return; }
        QFile f(m_part);
        const bool valid = f.open(QIODevice::ReadOnly) && f.read(4) == QByteArray("PK\x03\x04",4);
        f.close(); // Release the Windows handle before failure removes our .part.
        if (!valid) { finishFailure(QStringLiteral("下载内容不是有效 APK。")); return; }
        if (QFileInfo::exists(m_path) || !QFile::rename(m_part,m_path)) { finishFailure(QStringLiteral("无法保存 MT 管理器安装包。")); return; }
        m_part.clear(); m_active=false; emit log(QStringLiteral("MT 管理器下载完成。")); emit ready(m_version,m_path);
    });
}
bool MtManagerService::useParallel(QNetworkReply *reply) {
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206) return false;
    // readyRead/metaDataChanged 每次触发都会进入这里探测，正则必须复用实例。
    static const QRegularExpression contentRange(QStringLiteral("^bytes 0-0/([0-9]+)$"));
    const auto match = contentRange.match(QString::fromLatin1(reply->rawHeader("Content-Range")));
    bool ok = false;
    const qint64 size = match.captured(1).toLongLong(&ok);
    const QUrl url = reply->url();
    if (reply->error() != QNetworkReply::NoError || !match.hasMatch() || !ok || size < 4 || !OpenListService::https(url)) {
        finishFailure(QStringLiteral("MT 下载分段探测响应无效。")); return true;
    }
    m_reply = nullptr;
    disconnect(reply, nullptr, this, nullptr); reply->abort(); reply->deleteLater();
    m_file.close();
    if (!QFile::remove(m_part)) { finishFailure(QStringLiteral("下载临时文件清理失败。")); return true; }
    m_part.clear();
    OpenList::Entry entry;
    entry.name = QStringLiteral("MT管理器-%1.apk").arg(m_version);
    entry.relativePath = entry.name; entry.size = size;
    emit log(QStringLiteral("正在加速下载 MT 管理器…"));
    if (!m_downloader->downloadDirect(entry, url) && m_active)
        finishFailure(QStringLiteral("无法启动 MT 管理器下载。"));
    return true;
}
void MtManagerService::finishFailure(const QString &message) {
    // Detach before abort: abort may synchronously emit finished.
    const auto reply = m_reply;
    m_reply = nullptr;
    m_active = false;
    m_downloader->cancel();
    if (reply) { disconnect(reply, nullptr, this, nullptr); reply->abort(); reply->deleteLater(); }
    if (m_file.isOpen()) m_file.close(); if (!m_part.isEmpty()) QFile::remove(m_part);
    m_part.clear(); emit failed(message);
}
void MtManagerService::cancel() { if (m_active) finishFailure(QStringLiteral("MT 管理器下载已取消。")); }
