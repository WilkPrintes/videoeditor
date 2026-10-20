#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>
#include <QVersionNumber>

#include <memory>
#include <optional>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

// A published release, from the GitHub "latest release" API (drafts and prereleases are never
// "latest").
struct ReleaseInfo {
    QString tag;            // "v0.0.2"
    QVersionNumber version; // 0.0.2
    QString name;
    QString notes;          // the release text (Markdown)
    QUrl page;              // the release on github.com
    QString assetName;      // this platform's download ("VideoEditor-0.0.2-macos-arm64.zip"); empty when there is none
    QUrl assetUrl;
    qint64 assetSize = 0;
    QUrl checksumUrl;       // "<asset>.sha256", as written by shasum -a 256
};

namespace update {

// The end of this platform's download name: "macos-arm64.zip", "macos-x86_64.zip", "windows-x86_64.zip"...
QString platformAssetSuffix();
// "v0.0.2" or "0.0.2" -> 0.0.2 (a "-beta"-style suffix is ignored).
QVersionNumber versionOf(const QString& tag);
// Reads the GitHub release JSON and picks the asset whose name ends with `assetSuffix`, plus its
// ".sha256" companion. Nothing (and `error`) when the JSON is not a release.
std::optional<ReleaseInfo> parseRelease(const QByteArray& json, const QString& assetSuffix, QString* error = nullptr);
// The lowercase hex SHA-256 at the start of a "<hash>  <file>" line; empty when there is none.
QByteArray checksumFrom(const QByteArray& text);
QUrl githubFeed(const QString& repository); // https://api.github.com/repos/<owner/name>/releases/latest

} // namespace update

// Checks the release feed for a version newer than this one, downloads its archive and verifies it
// against the published SHA-256. On macOS it can also install it: the verified zip is unpacked and a
// small script waits for the app to quit, swaps the .app bundle and reopens it. Every step reports
// through signals, so the window decides what to show.
class Updater : public QObject {
    Q_OBJECT

public:
    Updater(QUrl feed, QVersionNumber current, QObject* parent = nullptr);
    ~Updater() override;

    QUrl feed() const { return m_feed; }
    QVersionNumber currentVersion() const { return m_current; }

    void check();                         // -> checked()
    void download(const ReleaseInfo& release); // -> downloadProgress(), then downloaded() or failed()
    void cancel();
    bool busy() const { return m_reply != nullptr; }

    // The running .app bundle, empty when this is not one (Windows, a build tree...).
    static QString currentBundle();
    // Whether this build can replace itself; `why` says what is missing otherwise.
    static bool canInstallInPlace(QString* why = nullptr);

    // macOS: unpacks the verified zip, checks the bundle's version and writes the swap script.
    // Returns the command to start detached right before the app quits.
    struct Installer {
        QString program;
        QStringList arguments;
    };
    std::optional<Installer> prepareInstall(const ReleaseInfo& release, const QString& zipPath, QString* error);

signals:
    // `newer` is false and `error` empty when this is the latest version.
    void checked(bool newer, const ReleaseInfo& release, const QString& error);
    void downloadProgress(qint64 received, qint64 total);
    void downloaded(const ReleaseInfo& release, const QString& zipPath);
    void failed(const QString& error);

private:
    QNetworkReply* get(const QUrl& url);
    void finishDownload(const ReleaseInfo& release, const QString& path);
    void verify(const ReleaseInfo& release, const QString& path, const QByteArray& checksumText);

    QUrl m_feed;
    QVersionNumber m_current;
    QNetworkAccessManager* m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QFile> m_file;
};
