#include "stdafx.h"
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineDownloadRequest>
#include <QWebChannel>
#include <QLocalSocket>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QReadWriteLock>
#include <QShortcut>

class MarkdownBridge final : public QObject
{
    Q_OBJECT
  public:
    using QObject::QObject;
  signals:
    void request(const QString &Message);
    void reported(const QJsonObject &Message);
  public slots:
    void report(const QString &Message)
    {
        const auto Object = QJsonDocument::fromJson(Message.toUtf8()).object();
        if (!Object.isEmpty())
            emit reported(Object);
    }
};
class ResourcePolicy final : public QWebEngineUrlRequestInterceptor
{
  public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;
    QString directory_;
    bool remote_ = false;
    QReadWriteLock mutex_;
    void interceptRequest(QWebEngineUrlRequestInfo &Request) override
    {
        QReadLocker Lock(&mutex_);
        const auto Url = Request.requestUrl();
        const auto Type = Request.resourceType();
        if (Url.scheme() == "qrc" &&
            (Url.path().startsWith("/markdown/") || Url.path() == "/qtwebchannel/qwebchannel.js"))
            return;
        if (Type == QWebEngineUrlRequestInfo::ResourceTypeImage)
        {
            if (Url.scheme() == "data")
                return;
            if (remote_ && (Url.scheme() == "https" || Url.scheme() == "http"))
                return;
            if (Url.isLocalFile() && !directory_.isEmpty())
            {
                const auto Path = QFileInfo(Url.toLocalFile()).canonicalFilePath();
                if (!Path.isEmpty() && Path.startsWith(directory_ + '/', Qt::CaseInsensitive))
                    return;
            }
        }
        Request.block(true);
    }
};
class MarkdownPage final : public QWebEnginePage
{
  public:
    MarkdownPage(QWebEngineProfile *Profile, QObject *Parent) : QWebEnginePage(Profile, Parent)
    {
    }

  protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &Message, int Line,
                                  const QString &Source) override
    {
        if (!qEnvironmentVariableIsEmpty("HC_MARKDOWN_DIAGNOSTICS"))
        {
            QFile Log(qEnvironmentVariable("HC_MARKDOWN_DIAGNOSTICS"));
            if (Log.open(QIODevice::Append))
                Log.write((Source + ":" + QString::number(Line) + ": " + Message + '\n').toUtf8());
        }
    }
    bool acceptNavigationRequest(const QUrl &Url, NavigationType, bool Main) override
    {
        return !Main || Url == QUrl("qrc:/markdown/shell.html");
    }
};
int main(int Argc, char **Argv)
{
    QApplication App(Argc, Argv);
    App.setQuitOnLastWindowClosed(true);
    const auto Args = App.arguments();
    if (Args.size() != 3)
        return 2;
    QLocalSocket Socket;
    QByteArray Input;
    bool Ready = false;
    QJsonObject Pending;
    auto Send = [&](QJsonObject Value) { Socket.write(QJsonDocument(Value).toJson(QJsonDocument::Compact) + '\n'); };
    QWebEngineProfile Profile;
    Profile.setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    Profile.setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    ResourcePolicy Policy;
    Profile.setUrlRequestInterceptor(&Policy);
    QObject::connect(&Profile, &QWebEngineProfile::downloadRequested,
                     [](QWebEngineDownloadRequest *Download) { Download->cancel(); });
    QWebEngineView View;
    View.resize(800, 600);
    View.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    auto *Page = new MarkdownPage(&Profile, &View);
    View.setPage(Page);
    Page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    Page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    Page->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    Page->settings()->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    QWebChannel Channel;
    MarkdownBridge Bridge;
    Channel.registerObject("markdown", &Bridge);
    Page->setWebChannel(&Channel);
    auto Dispatch = [&](const QJsonObject &Message) {
        const auto Type = Message["type"].toString();
        if (!qEnvironmentVariableIsEmpty("HC_MARKDOWN_DIAGNOSTICS"))
        {
            QFile Log(qEnvironmentVariable("HC_MARKDOWN_DIAGNOSTICS"));
            if (Log.open(QIODevice::Append))
                Log.write(("dispatch " + Type + '\n').toUtf8());
        }
        if (Type == "attached")
        {
            View.show();
            return;
        }
        if (Type == "shortcuts")
        {
            for (auto *Shortcut : View.findChildren<QShortcut *>())
                delete Shortcut;
            const auto Bindings = Message["bindings"].toObject();
            for (auto At = Bindings.begin(); At != Bindings.end(); ++At)
            {
                const auto Key = QKeySequence::fromString(At.value().toString(), QKeySequence::PortableText);
                if (Key.isEmpty())
                    continue;
                auto *Shortcut = new QShortcut(Key, &View);
                Shortcut->setContext(Qt::WindowShortcut);
                QObject::connect(Shortcut, &QShortcut::activated, &App,
                                 [&, Id = At.key()] { Send({{"type", "command"}, {"id", Id}}); });
            }
            return;
        }
        if (Type == "render")
        {
            QWriteLocker Lock(&Policy.mutex_);
            Policy.directory_ = QFileInfo(Message["path"].toString()).dir().canonicalPath();
            Policy.remote_ = Message["options"].toObject()["remoteImages"].toBool();
            Pending = Message;
        }
        if (Type == "zoom")
        {
            View.setZoomFactor(qBound(.5, Message["value"].toDouble(1), 3.));
            return;
        }
        if (Type == "find")
        {
            Page->findText(Message["text"].toString());
            return;
        }
        if (Type == "copy")
        {
            Page->triggerAction(QWebEnginePage::Copy);
            return;
        }
        if (Type == "selectAll")
        {
            Page->triggerAction(QWebEnginePage::SelectAll);
            return;
        }
        if (Type == "focus")
        {
            View.setFocus();
            return;
        }
        if (Ready)
            emit Bridge.request(QString::fromUtf8(QJsonDocument(Message).toJson(QJsonDocument::Compact)));
    };
    QObject::connect(&Bridge, &MarkdownBridge::reported, &App, [&](const QJsonObject &Message) {
        if (!qEnvironmentVariableIsEmpty("HC_MARKDOWN_DIAGNOSTICS"))
        {
            QFile Log(qEnvironmentVariable("HC_MARKDOWN_DIAGNOSTICS"));
            if (Log.open(QIODevice::Append))
                Log.write(("report " + Message["type"].toString() + '\n').toUtf8());
        }
        if (Message["type"] == "ready")
        {
            Ready = true;
            Send({{"type", "ready"}, {"window", QString::number(quintptr(View.winId()))}, {"token", Args[2]}});
            if (!Pending.isEmpty())
                Dispatch(Pending);
        }
        else
            Send(Message);
    });
    QObject::connect(&Socket, &QLocalSocket::disconnected, &App, &QApplication::quit);
    QObject::connect(&Socket, &QLocalSocket::errorOccurred, &App, [&] { App.exit(3); });
    QObject::connect(&Socket, &QLocalSocket::readyRead, &App, [&] {
        Input += Socket.readAll();
        if (Input.size() > 32 * 1024 * 1024)
        {
            App.exit(4);
            return;
        }
        while (Input.contains('\n'))
        {
            const auto End = Input.indexOf('\n');
            const auto Message = QJsonDocument::fromJson(Input.left(End)).object();
            Input.remove(0, End + 1);
            if (Message["type"] == "close")
            {
                App.quit();
                return;
            }
            Dispatch(Message);
        }
    });
    QObject::connect(Page, &QWebEnginePage::renderProcessTerminated, &App, [&] {
        Send({{"type", "error"},
              {"message", QObject::tr("미리보기 프로세스가 종료되었습니다. 새로고침으로 다시 시도하세요.")}});
    });
    Socket.connectToServer(Args[1]);
    View.load(QUrl("qrc:/markdown/shell.html"));
    QTimer::singleShot(15000, &App, [&] {
        if (!Ready)
            App.exit(5);
    });
    return App.exec();
}
#include "main.moc"
