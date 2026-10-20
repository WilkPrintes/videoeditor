#include "core/Log.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QPalette>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QTimer>

#include <cstdio>

namespace {

QFile* g_logFile = nullptr;
QMutex g_logMutex;

// Everything (Qt, engine, FFmpeg) ends up in stderr and in videoeditor.log next to the app data.
void messageHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    static const char* names[] = {"debug", "warning", "critical", "fatal", "info"};
    const QString line = QStringLiteral("%1 [%2] %3\n")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                                  QLatin1String(names[int(type)]), msg);
    const QByteArray utf8 = line.toUtf8();
    QMutexLocker lock(&g_logMutex);
    std::fputs(utf8.constData(), stderr);
    if (g_logFile) {
        g_logFile->write(utf8);
        g_logFile->flush();
    }
}

void installLogging()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    g_logFile = new QFile(dir + QStringLiteral("/videoeditor.log"));
    if (!g_logFile->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        delete g_logFile;
        g_logFile = nullptr;
    }
    qInstallMessageHandler(messageHandler);
    ve::setLogSink([](ve::LogLevel level, const std::string& message) {
        const QString text = QString::fromStdString(message);
        switch (level) {
        case ve::LogLevel::Debug:
            qDebug().noquote() << text;
            break;
        case ve::LogLevel::Info:
            qInfo().noquote() << text;
            break;
        case ve::LogLevel::Warning:
            qWarning().noquote() << text;
            break;
        case ve::LogLevel::Error:
            qCritical().noquote() << text;
            break;
        }
    });
}

void applyDarkTheme(QApplication& app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    const QColor window(37, 38, 42), base(28, 29, 32), text(222, 224, 228), dim(120, 124, 130);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(48, 50, 55));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor(255, 90, 80));
    p.setColor(QPalette::Highlight, QColor(70, 120, 196));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, dim);
    p.setColor(QPalette::Disabled, QPalette::Text, dim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, dim);
    app.setPalette(p);
}

} // namespace

int main(int argc, char** argv)
{
    QApplication::setApplicationName(QStringLiteral("VideoEditor"));
    QApplication::setOrganizationName(QStringLiteral("VideoEditor"));
    QApplication::setApplicationVersion(QStringLiteral(VE_VERSION));
    QApplication app(argc, argv);
    installLogging();
    applyDarkTheme(app);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Native video editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("A .veproj project or media files to import."));
    const QCommandLineOption playOption(QStringLiteral("play"), QStringLiteral("Start playback after loading."));
    const QCommandLineOption seekOption(QStringLiteral("seek"), QStringLiteral("Move the playhead to <frame>."), QStringLiteral("frame"));
    const QCommandLineOption screenshotOption(QStringLiteral("screenshot"),
                                              QStringLiteral("Save a window screenshot to <file> and quit (testing)."),
                                              QStringLiteral("file"));
    const QCommandLineOption delayOption(QStringLiteral("delay"), QStringLiteral("Milliseconds before --screenshot (default 2500)."),
                                         QStringLiteral("ms"), QStringLiteral("2500"));
    parser.addOptions({playOption, seekOption, screenshotOption, delayOption});
    parser.process(app);

    MainWindow window;
    window.resize(1600, 960);
    window.restoreLastLayout(); // panels and window as they were left (tests start from the default)
    window.show();
    qInfo().noquote() << "VideoEditor" << QApplication::applicationVersion() << "on Qt" << qVersion();

    QStringList media;
    for (const QString& arg : parser.positionalArguments()) {
        if (QFileInfo(arg).suffix().compare(QStringLiteral("veproj"), Qt::CaseInsensitive) == 0)
            window.openProjectFile(arg);
        else
            media << arg;
    }
    if (!media.isEmpty())
        window.importFiles(media);
    if (parser.isSet(seekOption))
        window.seekToFrame(parser.value(seekOption).toLongLong());
    if (parser.isSet(playOption))
        QTimer::singleShot(300, &window, [&window] { window.play(); });
    if (parser.isSet(screenshotOption)) {
        const QString file = parser.value(screenshotOption);
        QTimer::singleShot(parser.value(delayOption).toInt(), &window, [&window, file] {
            const bool ok = window.grab().save(file);
            qInfo().noquote() << "screenshot" << file << (ok ? "saved" : "FAILED");
            QCoreApplication::exit(ok ? 0 : 1);
        });
    }
    return app.exec();
}
