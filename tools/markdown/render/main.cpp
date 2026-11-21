#include "stdafx.h"
#include <QWebEngineView>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineScript>
#include <QWebEngineLoadingInfo>
#include <QBuffer>
#include <QElapsedTimer>
#include <algorithm>
#include <QMimeDatabase>
#include <QRegularExpression>
#include "UniqueLibs/fonts/startupFontWarmup.hpp"
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineDownloadRequest>
#include <QWebEngineContextMenuRequest>
#include <QWebChannel>
#include <QLocalSocket>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QReadWriteLock>
#include <QShortcut>
#include <QAbstractNativeEventFilter>
#include "UniqueLibs/document/documentWebProcess.hpp"
#include "UniqueLibs/document/documentStallTrace.hpp"
#include <qt_windows.h>

// The host posts WebSurfaceResizeMessage instead of resizing this window from its own thread:
// the two GUI threads share one input queue, and a synchronous resize made the editor wait for
// a render here. The size is the current parent's client area; the message carries none.
class SurfaceResize final : public QAbstractNativeEventFilter
{
  public:
    explicit SurfaceResize(QWidget &Window)
        : window_(Window), message_(RegisterWindowMessageW(hc::document::WebSurfaceResizeMessage))
    {
    }
    bool nativeEventFilter(const QByteArray &Type, void *Message, qintptr *) override
    {
        const auto *Msg = static_cast<const MSG *>(Message);
        if (Type != "windows_generic_MSG" || !message_ || Msg->message != message_)
            return false;
        const auto Window = reinterpret_cast<HWND>(window_.winId());
        if (Msg->hwnd != Window)
            return false;
        RECT Parent{}, Own{};
        const auto Host = GetParent(Window);
        if (Host && GetClientRect(Host, &Parent) && GetClientRect(Window, &Own)
            && (Own.right != Parent.right || Own.bottom != Parent.bottom))
            SetWindowPos(Window, nullptr, 0, 0, Parent.right, Parent.bottom, SWP_NOACTIVATE | SWP_NOZORDER);
        return true;
    }

  private:
    QWidget &window_;
    UINT message_ = 0;
};

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
// Serve the unsaved UTF-8 buffer without setHtml's data-URL size limit.
class HtmlDocument final : public QWebEngineUrlSchemeHandler
{
  public:
    QUrl url_;
    QByteArray source_;
    QString directory_;
    QJsonObject assets_;
    void requestStarted(QWebEngineUrlRequestJob *Job) override
    {
        const auto Url = Job->requestUrl();
        if (Url.host() != "document" || Job->requestMethod() != "GET") {
            Job->fail(QWebEngineUrlRequestJob::RequestDenied);
            return;
        }
        if (Url.adjusted(QUrl::RemoveFragment) == url_) {
            auto *Buffer = new QBuffer(Job);
            Buffer->setData(source_);
            Buffer->open(QIODevice::ReadOnly);
            Job->setAdditionalResponseHeaders({{"Content-Security-Policy",
                "default-src 'none'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; font-src 'self'; form-action 'none'; base-uri 'none'"}});
            Job->reply("text/html", Buffer);
            return;
        }
        const auto Relative = Url.path(QUrl::FullyDecoded).mid(1);
        if (directory_.isEmpty() || Relative.contains(':') || Relative.contains('\\') || Relative.startsWith('/')
            || Relative.split('/').contains("..")) {
            Job->fail(QWebEngineUrlRequestJob::RequestDenied);
            return;
        }
        const auto Name = assets_.isEmpty() ? Relative : assets_.value(Relative).toString();
        if (Name.isEmpty()) { Job->fail(QWebEngineUrlRequestJob::UrlNotFound); return; }
        const QFileInfo Info(QDir(directory_), Name);
        const auto Path = Info.canonicalFilePath();
        const auto Prefix = QDir::cleanPath(directory_ + "/.") + (directory_.endsWith('/') ? "" : "/");
        if (Path.isEmpty() || !Path.startsWith(Prefix, Qt::CaseInsensitive) || !Info.isFile()) {
            Job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        auto *File = new QFile(Path, Job);
        if (!File->open(QIODevice::ReadOnly)) {
            Job->fail(QWebEngineUrlRequestJob::RequestFailed);
            return;
        }
        Job->reply(QMimeDatabase().mimeTypeForFile(Relative, QMimeDatabase::MatchExtension).name().toLatin1(), File);
    }
};

class ResourcePolicy final : public QWebEngineUrlRequestInterceptor
{
  public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;
    QString directory_;
    bool html_ = false;
    bool remote_ = false;
    QReadWriteLock mutex_;
    void interceptRequest(QWebEngineUrlRequestInfo &Request) override
    {
        QReadLocker Lock(&mutex_);
        const auto Url = Request.requestUrl();
        const auto Type = Request.resourceType();
        if (html_) {
            using Info = QWebEngineUrlRequestInfo;
            const bool Local = Url.scheme() == "hc-html" && Url.host() == "document";
            const bool Asset = Type == Info::ResourceTypeStylesheet || Type == Info::ResourceTypeImage || Type == Info::ResourceTypeFontResource;
            Request.block(!(Local && (Type == Info::ResourceTypeMainFrame || Asset)) && !(Url.scheme() == "data" && Type == Info::ResourceTypeImage));
            return;
        }
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

    bool html_ = false;
    QUrl documentUrl_;
    std::function<void(const QUrl &)> externalLink_;
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
    bool acceptNavigationRequest(const QUrl &Url, NavigationType Type, bool Main) override
    {
        if (html_) {
            if (Main && Url.adjusted(QUrl::RemoveFragment) == documentUrl_) return true;
            if (Main && Type == NavigationTypeLinkClicked && (Url.scheme() == "https" || Url.scheme() == "http"))
                externalLink_(Url);
            return false;
        }
        return !Main || Url == QUrl("qrc:/markdown/shell.html");
    }
};
int main(int Argc, char **Argv)
{
    hc::fonts::StartupFontWarmup FontWarmup(Argc, Argv);
    // Qt's cache folder follows the application identity; without one it was %LOCALAPPDATA%\hc-markdown-render.
    QCoreApplication::setOrganizationName(QStringLiteral("HyperCommander")); QCoreApplication::setApplicationName(QStringLiteral("hc-markdown-render"));
    QWebEngineUrlScheme Scheme("hc-html");
    Scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    QWebEngineUrlScheme::registerScheme(Scheme);
    QApplication App(Argc, Argv);
    FontWarmup.WatchFirstWindow();
    App.setQuitOnLastWindowClosed(true);
    // On only while the editor records (HC_UI_STALL_TRACE is inherited); writes under TEMP.
    hc::document::DocumentStallTrace::InstallHelper(QStringLiteral("markdown"));
    const auto Args = App.arguments();
    const bool Html = Args.size() == 4 && Args[3] == "--html";
    if (!Html && Args.size() != 3)
        return 2;
    QLocalSocket Socket;
    QByteArray Input;
    bool Ready = false;
    QJsonObject Pending;
    auto Send = [&](QJsonObject Value) { Socket.write(QJsonDocument(Value).toJson(QJsonDocument::Compact) + '\n'); };
    HtmlDocument Document;
    QWebEngineProfile Profile;
    if (Html) Profile.installUrlSchemeHandler("hc-html", &Document);
    Profile.setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    Profile.setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    ResourcePolicy Policy;
    Policy.html_ = Html;
    Profile.setUrlRequestInterceptor(&Policy);
    QObject::connect(&Profile, &QWebEngineProfile::downloadRequested,
                     [](QWebEngineDownloadRequest *Download) { Download->cancel(); });
    QPointF HtmlScroll;
    QList<int> HeadingLines;
    QElapsedTimer LastSourceScroll;
    QTimer ScrollReport;
    ScrollReport.setSingleShot(true);
    ScrollReport.setInterval(75);
    QWebEngineView View;
    View.resize(800, 600);
    SurfaceResize Resize(View);
    App.installNativeEventFilter(&Resize);
    auto *Escape = new QShortcut(QKeySequence(Qt::Key_Escape), &View);
    Escape->setObjectName("previewEscape");
    Escape->setContext(Qt::WindowShortcut);
    QObject::connect(Escape, &QShortcut::activated, &App, [&] { Send({{"type", "escape"}}); });
    View.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    View.setAttribute(Qt::WA_ShowWithoutActivating);
    auto *Page = new MarkdownPage(&Profile, &View);
    Page->html_ = Html;
    Page->externalLink_ = [&](const QUrl &Url) { Send({{"type", "link"}, {"revision", Pending["revision"]}, {"url", Url.toString()}}); };
    View.setPage(Page);
    Page->settings()->setDefaultTextEncoding("UTF-8");
    // The host draws the menu. The page reports its own right clicks; Chromium asks for a menu only
    // before the script is ready or outside the document, and that request becomes the same report.
    View.setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(&View, &QWidget::customContextMenuRequested, &App, [&](const QPoint &Position) {
        const auto *Request = View.lastContextMenuRequest();
        const double Zoom = qMax(.1, View.zoomFactor());
        Send({{"type", "context"},
              {"x", Position.x() / Zoom},
              {"y", Position.y() / Zoom},
              {"selection", Request ? Request->selectedText().left(1024) : QString()},
              {"link", Request ? Request->linkUrl().toString() : QString()},
              {"line", 0}});
    });
    // Loading or refreshing a passive preview must not take source-editor input.
    Page->settings()->setAttribute(QWebEngineSettings::FocusOnNavigationEnabled, false);
    Page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    Page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    Page->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    Page->settings()->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    QWebChannel Channel;
    MarkdownBridge Bridge;
    Channel.registerObject("markdown", &Bridge);
    if (!Html) Page->setWebChannel(&Channel);
    else {
        Page->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, false);
        Page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
        Page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    }
    if (Html) {
        QObject::connect(Page, &QWebEnginePage::loadingChanged, &App, [&](const QWebEngineLoadingInfo &Info) {
            if (Info.status() == QWebEngineLoadingInfo::LoadFailedStatus && Info.url() == Document.url_)
                Send({{"type", "error"}, {"message", QObject::tr("HTML 미리보기를 열 수 없습니다. %1").arg(Info.errorString())}});
        });
        QObject::connect(Page, &QWebEnginePage::scrollPositionChanged, &ScrollReport, [&] { ScrollReport.start(); });
        QObject::connect(&ScrollReport, &QTimer::timeout, &App, [&] {
            const auto Revision = Pending["revision"];
            Page->runJavaScript(QStringLiteral(R"JS((() => {
                const headings = Array.from(document.querySelectorAll('h1,h2,h3,h4,h5,h6'));
                const index = headings.findLastIndex(h => h.getBoundingClientRect().top <= 1);
                const extent = Math.max(0, document.documentElement.scrollHeight - innerHeight);
                return {index, heading:headings[index]?.textContent.trim() || '', top:scrollY <= 1,
                        bottom:scrollY >= extent - 1, progress:extent ? Math.round(scrollY * 100 / extent) : 0};
            })())JS"), QWebEngineScript::ApplicationWorld, [&, Revision](const QVariant &Value) {
                if (!Value.isValid() || Revision != Pending["revision"]) return;
                auto State = QJsonObject::fromVariantMap(Value.toMap());
                State["type"] = "scroll";
                State["revision"] = Revision;
                State["headingLine"] = HeadingLines.value(State["index"].toInt(-1), 1);
                State["line"] = State["headingLine"];
                State["user"] = View.hasFocus() && (!LastSourceScroll.isValid() || LastSourceScroll.elapsed() > 200);
                Send(State);
            });
        });
    }
    if (Html) QObject::connect(Page, &QWebEnginePage::loadFinished, &App, [&](bool Ok) {
        const auto Url = Document.url_;
        if (!Ok || Page->url().adjusted(QUrl::RemoveFragment) != Url) return;
        const auto Revision = Pending["revision"];
        Page->runJavaScript(QStringLiteral("window.scrollTo(%1,%2); Array.from(document.querySelectorAll('h1,h2,h3,h4,h5,h6'), h => ({text:h.textContent.trim(),level:Number(h.tagName[1])}))")
            .arg(HtmlScroll.x()).arg(HtmlScroll.y()), QWebEngineScript::ApplicationWorld,
            [&, Url, Revision](const QVariant &Value) {
                if (!Value.isValid() || Document.url_ != Url) return;
                auto Headings = QJsonArray::fromVariantList(Value.toList());
                for (qsizetype I = 0; I < Headings.size(); ++I) {
                    auto Row = Headings[I].toObject(); Row["line"] = HeadingLines.value(I, 1); Headings[I] = Row;
                }
                Send({{"type", "rendered"}, {"revision", Revision}, {"headings", Headings}});
            });
    });
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
                if (Shortcut != Escape) delete Shortcut;
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
        if (Html && Type == "diagnostics") {
            Page->runJavaScript(QStringLiteral("({color:getComputedStyle(document.body).color, imageWidth:document.images[0]?.naturalWidth || 0})"),
                QWebEngineScript::ApplicationWorld, [&, Id = Message["id"]](const QVariant &Value) {
                    if (Value.isValid()) Send({{"type", "diagnostics"}, {"id", Id}, {"page", QJsonObject::fromVariantMap(Value.toMap())}});
                });
            return;
        }
        if (Html && Type == "render") {
            HtmlScroll = Page->scrollPosition();
            Pending = Message;
            const auto Text = Message["text"].toString();
            Document.source_ = QByteArray::fromHex("efbbbf") + Text.toUtf8();
            Document.directory_ = QFileInfo(Message["resourcePath"].toString()).canonicalPath();
            if (Message["resourcePath"].toString().isEmpty()) Document.directory_.clear();
            Document.assets_ = Message["assets"].toObject();
            if (!Document.assets_.isEmpty()) Document.directory_ = Message["assetDirectory"].toString();
            Document.url_ = QUrl("hc-html://document/index.html");
            Document.url_.setQuery("revision=" + QString::number(Message["revision"].toInteger()));
            Page->documentUrl_ = Document.url_;
            HeadingLines.clear();
            static const QRegularExpression Heading("<h[1-6](?=[\\s>])", QRegularExpression::CaseInsensitiveOption);
            qsizetype Offset = 0; int Line = 1;
            for (auto Matches = Heading.globalMatch(Text); Matches.hasNext();) {
                const auto Start = Matches.next().capturedStart();
                Line += QStringView(Text).mid(Offset, Start - Offset).count('\n');
                Offset = Start; HeadingLines.append(Line);
            }
            Page->load(Document.url_);
            return;
        }
        if (Html && (Type == "position" || Type == "scroll")) {
            const int Line = Message["line"].toInt();
            if ((Message["reveal"].toBool() || Type == "scroll") && !HeadingLines.isEmpty()) {
                const auto End = std::upper_bound(HeadingLines.cbegin(), HeadingLines.cend(), Line);
                const auto Index = qMax<qsizetype>(0, End - HeadingLines.cbegin() - 1);
                LastSourceScroll.restart();
                Page->runJavaScript(QString("document.querySelectorAll('h1,h2,h3,h4,h5,h6')[%1]?.scrollIntoView()").arg(Index), QWebEngineScript::ApplicationWorld);
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
            View.activateWindow();
            View.setFocus(Qt::OtherFocusReason);
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
    if (Html) QObject::connect(&Socket, &QLocalSocket::connected, &App, [&] {
        Ready = true; Send({{"type", "ready"}, {"window", QString::number(quintptr(View.winId()))}, {"token", Args[2]}});
    });
    Socket.connectToServer(Args[1]);
    if (!Html) View.load(QUrl("qrc:/markdown/shell.html"));
    QTimer::singleShot(15000, &App, [&] {
        if (!Ready)
            App.exit(5);
    });
    return App.exec();
}
#include "main.moc"
