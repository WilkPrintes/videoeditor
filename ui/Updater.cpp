#include "ui/Updater.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

namespace {

#if defined(Q_OS_MACOS)
// Runs detached after the app has started quitting: $1 pid, $2 new bundle, $3 bundle to replace,
// $4 log file. The old bundle is only removed once the new one is in place.
constexpr const char* kInstallScript = R"sh(#!/bin/sh
pid="$1"
new="$2"
target="$3"
exec >>"$4" 2>&1
echo "$(date): updating $target"
n=0
while kill -0 "$pid" 2>/dev/null; do
    n=$((n + 1))
    if [ "$n" -gt 1200 ]; then
        echo "VideoEditor did not quit; nothing was changed"
        exit 1
    fi
    sleep 0.1
done
backup="$target.previous"
rm -rf "$backup"
if ! mv "$target" "$backup"; then
    echo "cannot move the old version away; nothing was changed"
    exit 1
fi
if ! mv "$new" "$target"; then
    echo "cannot move the new version in; the old one is back"
    mv "$backup" "$target"
    open "$target"
    exit 1
fi
rm -rf "$backup"
xattr -dr com.apple.quarantine "$target" 2>/dev/null
echo "updated"
open "$target"
)sh";
#endif

} // namespace

namespace update {

QString platformAssetSuffix()
{
#if defined(Q_OS_MACOS)
    const QString os = QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    const QString os = QStringLiteral("windows");
#else
    const QString os = QStringLiteral("linux");
#endif
    return QStringLiteral("%1-%2.zip").arg(os, QSysInfo::buildCpuArchitecture());
}

QVersionNumber versionOf(const QString& tag)
{
    QString text = tag.trimmed();
    if (text.startsWith(QLatin1Char('v')) || text.startsWith(QLatin1Char('V')))
        text.remove(0, 1);
    return QVersionNumber::fromString(text);
}

std::optional<ReleaseInfo> parseRelease(const QByteArray& json, const QString& assetSuffix, QString* error)
{
    auto fail = [error](const QString& why) -> std::optional<ReleaseInfo> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail(Updater::tr("The release feed is not valid JSON"));
    const QJsonObject o = doc.object();

    ReleaseInfo release;
    release.tag = o.value(QLatin1String("tag_name")).toString();
    release.version = versionOf(release.tag);
    if (release.version.isNull())
        return fail(Updater::tr("The latest release has no version tag"));
    release.name = o.value(QLatin1String("name")).toString();
    release.notes = o.value(QLatin1String("body")).toString();
    release.page = QUrl(o.value(QLatin1String("html_url")).toString());

    const QJsonArray assets = o.value(QLatin1String("assets")).toArray();
    for (const QJsonValue& value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QLatin1String("name")).toString();
        if (name.startsWith(QLatin1String("VideoEditor-")) && name.endsWith(QLatin1Char('-') + assetSuffix)) {
            release.assetName = name;
            release.assetUrl = QUrl(asset.value(QLatin1String("browser_download_url")).toString());
            release.assetSize = asset.value(QLatin1String("size")).toInteger();
        }
    }
    if (!release.assetName.isEmpty()) {
        for (const QJsonValue& value : assets) {
            const QJsonObject asset = value.toObject();
            if (asset.value(QLatin1String("name")).toString() == release.assetName + QLatin1String(".sha256"))
                release.checksumUrl = QUrl(asset.value(QLatin1String("browser_download_url")).toString());
        }
    }
    return release;
}

QByteArray checksumFrom(const QByteArray& text)
{
    static const QRegularExpression hex(QStringLiteral("^\\s*([0-9a-fA-F]{64})(\\s|$)"));
    const QRegularExpressionMatch m = hex.match(QString::fromLatin1(text));
    return m.hasMatch() ? m.captured(1).toLower().toLatin1() : QByteArray();
}

QUrl githubFeed(const QString& repository)
{
    return QUrl(QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(repository));
}

} // namespace update

Updater::Updater(QUrl feed, QVersionNumber current, QObject* parent)
    : QObject(parent)
    , m_feed(std::move(feed))
    , m_current(std::move(current))
    , m_network(new QNetworkAccessManager(this))
{
}

Updater::~Updater()
{
    cancel();
}

QNetworkReply* Updater::get(const QUrl& url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("VideoEditor/%1").arg(m_current.toString()));
    if (url == m_feed)
        request.setRawHeader("Accept", "application/vnd.github+json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000); // without any data for 30 s
    return m_network->get(request);
}

void Updater::cancel()
{
    if (m_reply) {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (m_file) {
        m_file->close();
        m_file->remove();
        m_file.reset();
    }
}

void Updater::check()
{
    cancel();
    QNetworkReply* reply = get(m_feed);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_reply = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString why = status == 404 ? tr("No release has been published yet.")
                : status == 403 || status == 429 ? tr("GitHub refused the request (rate limit). Try again later.")
                                                 : reply->errorString();
            emit checked(false, {}, why);
            return;
        }
        QString error;
        const auto release = update::parseRelease(reply->readAll(), update::platformAssetSuffix(), &error);
        if (!release) {
            emit checked(false, {}, error);
            return;
        }
        emit checked(release->version > m_current, *release, {});
    });
}

void Updater::download(const ReleaseInfo& release)
{
    cancel();
    if (release.assetUrl.isEmpty()) {
        emit failed(tr("Release %1 has no download for this computer (%2).").arg(release.tag, update::platformAssetSuffix()));
        return;
    }
    if (release.checksumUrl.isEmpty()) {
        emit failed(tr("Release %1 publishes no SHA-256 checksum, so the download cannot be verified.").arg(release.tag));
        return;
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/updates/")
        + release.version.toString();
    QDir(dir).removeRecursively();
    if (!QDir().mkpath(dir)) {
        emit failed(tr("Cannot create %1").arg(QDir::toNativeSeparators(dir)));
        return;
    }
    const QString path = dir + QLatin1Char('/') + release.assetName;
    m_file = std::make_unique<QFile>(path);
    if (!m_file->open(QIODevice::WriteOnly)) {
        const QString why = m_file->errorString();
        m_file.reset();
        emit failed(tr("Cannot write %1: %2").arg(QDir::toNativeSeparators(path), why));
        return;
    }

    QNetworkReply* reply = get(release.assetUrl);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (m_file)
            m_file->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this, &Updater::downloadProgress);
    connect(reply, &QNetworkReply::finished, this, [this, reply, release, path] {
        reply->deleteLater();
        m_reply = nullptr;
        bool written = false;
        if (m_file) {
            m_file->write(reply->readAll());
            written = m_file->flush();
            m_file->close();
            m_file.reset();
        }
        if (reply->error() != QNetworkReply::NoError || !written) {
            QFile::remove(path);
            emit failed(tr("The download failed: %1").arg(reply->errorString()));
            return;
        }
        finishDownload(release, path);
    });
}

void Updater::finishDownload(const ReleaseInfo& release, const QString& path)
{
    QNetworkReply* reply = get(release.checksumUrl);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, release, path] {
        reply->deleteLater();
        m_reply = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            QFile::remove(path);
            emit failed(tr("Cannot read the checksum of %1: %2").arg(release.assetName, reply->errorString()));
            return;
        }
        verify(release, path, reply->readAll());
    });
}

void Updater::verify(const ReleaseInfo& release, const QString& path, const QByteArray& checksumText)
{
    const QByteArray expected = update::checksumFrom(checksumText);
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const bool read = file.open(QIODevice::ReadOnly) && hash.addData(&file);
    const bool sizeOk = release.assetSize <= 0 || file.size() == release.assetSize;
    file.close();
    if (expected.isEmpty() || !read || !sizeOk || hash.result().toHex() != expected) {
        QFile::remove(path);
        emit failed(tr("The download of %1 does not match its SHA-256 checksum and was deleted.").arg(release.assetName));
        return;
    }
    emit downloaded(release, path);
}

QString Updater::currentBundle()
{
    QDir dir(QCoreApplication::applicationDirPath()); // <bundle>.app/Contents/MacOS
    if (dir.dirName() != QLatin1String("MacOS") || !dir.cdUp() || dir.dirName() != QLatin1String("Contents") || !dir.cdUp())
        return {};
    const QString bundle = dir.absolutePath();
    return bundle.endsWith(QLatin1String(".app")) ? bundle : QString();
}

bool Updater::canInstallInPlace(QString* why)
{
    auto no = [why](const QString& reason) {
        if (why)
            *why = reason;
        return false;
    };
#if defined(Q_OS_MACOS)
    const QString bundle = currentBundle();
    if (bundle.isEmpty())
        return no(tr("This copy is not running from a VideoEditor.app bundle."));
    if (bundle.contains(QLatin1String("/AppTranslocation/")))
        return no(tr("macOS is running this copy from a temporary, read-only place. Move VideoEditor.app to the "
                     "Applications folder and open it from there."));
    if (!QFileInfo(QFileInfo(bundle).absolutePath()).isWritable() || !QFileInfo(bundle).isWritable())
        return no(tr("This user cannot replace %1.").arg(bundle));
    return true;
#else
    return no(tr("Installing by itself is only available in the macOS app for now; download the new version "
                 "from the release page."));
#endif
}

std::optional<Updater::Installer> Updater::prepareInstall(const ReleaseInfo& release, const QString& zipPath, QString* error)
{
    auto fail = [error](const QString& why) -> std::optional<Installer> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    QString why;
    if (!canInstallInPlace(&why))
        return fail(why);
#if defined(Q_OS_MACOS)
    const QString dir = QFileInfo(zipPath).absolutePath();
    const QString unpacked = dir + QStringLiteral("/unpacked");
    QDir(unpacked).removeRecursively();
    QDir().mkpath(unpacked);
    QProcess ditto; // keeps the bundle's symlinks, permissions and signature
    ditto.start(QStringLiteral("/usr/bin/ditto"), {QStringLiteral("-x"), QStringLiteral("-k"), zipPath, unpacked});
    if (!ditto.waitForFinished(120000) || ditto.exitStatus() != QProcess::NormalExit || ditto.exitCode() != 0)
        return fail(tr("Cannot unpack %1: %2")
                        .arg(QFileInfo(zipPath).fileName(), QString::fromLocal8Bit(ditto.readAllStandardError()).trimmed()));
    const QString app = unpacked + QStringLiteral("/VideoEditor.app");
    if (!QFileInfo(app + QStringLiteral("/Contents/MacOS/VideoEditor")).isExecutable())
        return fail(tr("The download does not contain VideoEditor.app."));
    const QSettings plist(app + QStringLiteral("/Contents/Info.plist"), QSettings::NativeFormat);
    const QVersionNumber bundled = update::versionOf(plist.value(QStringLiteral("CFBundleShortVersionString")).toString());
    if (bundled != release.version)
        return fail(tr("The downloaded app is version %1, not %2.").arg(bundled.toString(), release.version.toString()));

    const QString script = dir + QStringLiteral("/install.sh");
    QFile file(script);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(kInstallScript) < 0)
        return fail(tr("Cannot write %1").arg(script));
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return Installer{QStringLiteral("/bin/sh"),
                     {script, QString::number(QCoreApplication::applicationPid()), app, currentBundle(),
                      dir + QStringLiteral("/install.log")}};
#else
    Q_UNUSED(release)
    Q_UNUSED(zipPath)
    return fail(why);
#endif
}
