#include "stdafx.h"
#include "readingSettings.hpp"
#include "UniqueLibs/fonts/startupFontWarmup.hpp"
#include "UniqueLibs/ProductDefaults_Edit.hpp"
#include "readingNavigation.hpp"
#include "commonLibs/document/epub/epubAppearance.hpp"
#include "ownerMode.hpp"
#include "inputTransfer.hpp"
#include "publicationResources.hpp"
#include "commonLibs/document/epub/epubPackage.hpp"
#include "commonLibs/document/epub/epubSourceLines.hpp"
#include "commonLibs/document/epub/epubScrollRequest.hpp"
#include <cmath>
#include <QWindow>
#include <QPointer>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QFileInfo>
#include <QDateTime>
#include <QElapsedTimer>
#include <memory>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include "UniqueLibs/document/documentWebPeer.hpp"
#include "UniqueLibs/document/documentShortcutRouter.hpp"
#include "UniqueLibs/document/documentStallTrace.hpp"
#include <QAbstractSpinBox>
#include <QAbstractItemView>

/// Product integration
#include "UniqueLibs/document/documentThemeAdapter.hpp"

/// Platform/framework headers
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <QBuffer>
#include <QCommandLineParser>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMutex>
#include <QMutexLocker>
#include <QScrollArea>
#include <QScreen>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTabWidget>
#include <QToolBar>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QUuid>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineView>
#include <QVBoxLayout>

namespace
{
    struct Asset { QByteArray data; QByteArray mime; std::shared_ptr<hc::epub::poc::PublicationResources> store; qint64 size=0; };
    using Assets = QHash<QString, Asset>;
    constexpr qint64 MaxSnapshot = 48 * 1024 * 1024;
    QByteArray readBounded(const QString& Path, qint64 Limit)
    {
        QFile File(Path);
        if(!File.open(QIODevice::ReadOnly) || File.size() > Limit) throw QString("Input inaccessible or over limit: ") + Path;
        const auto Bytes = File.read(Limit + 1);
        if(Bytes.size() > Limit) throw QString("Input limit exceeded");
        return Bytes;
    }
    QJsonObject readObject(const QString& Path, qint64 Limit)
    {
        QJsonParseError Error;
        const auto Doc = QJsonDocument::fromJson(readBounded(Path, Limit), &Error);
        if(Error.error != QJsonParseError::NoError || !Doc.isObject()) throw QString("Invalid JSON: ") + Path;
        return Doc.object();
    }
    bool validPath(const QString& Path)
    {
        if(Path.isEmpty() || Path.startsWith('/') || Path.contains('\\') || Path.contains(':') || Path.contains(QChar(0))) return false;
        for(const auto& Part : Path.split('/')) if(Part.isEmpty() || Part == "." || Part == "..") return false;
        return true;
    }
    Assets loadAssets(QJsonObject& Publication, const QString& Bundle, const std::shared_ptr<hc::document::epub::PublicationSession>& Session = {})
    {
        if(Publication["schemaVersion"].toInt() != 1 || Publication["phase"] != "P0-B"
            || Publication["policy"] != "passive-reflowable-v1" || Publication["spine"].toArray().isEmpty())
            throw QString("Unsupported prepared publication");
        Assets Result;
        const auto Store=Session?std::make_shared<hc::epub::poc::PublicationResources>(Session):nullptr;
        qint64 Total = 0;
        const QSet<QString> Types{"application/xhtml+xml", "application/oebps-package+xml", "application/x-dtbncx+xml",
            "text/css", "image/svg+xml", "image/png", "image/jpeg", "image/gif", "image/webp", "font/ttf", "font/otf", "font/woff", "font/woff2"};
        QJsonArray Metadata;
        const auto Resources = Publication["resources"].toArray();
        if(Resources.size() > 4096) throw QString("Too many resources");
        for(const auto Value : Resources)
        {
            auto Item = Value.toObject();
            const auto Path = Item["path"].toString();
            const auto Type = Item["mediaType"].toString();
            if(Store) {
                if(!validPath(Path) || Result.contains("/book/"+Path) || !Types.contains(Type)) throw QString("Invalid resource");
                Result.insert("/book/"+Path,{{},Type.toLatin1(),Store,Item["bytes"].toInteger(-1)});
                Metadata.append(Item); continue;
            }
            const auto Decoded = QByteArray::fromBase64Encoding(Item["base64"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if(!Decoded || !validPath(Path) || Result.contains("/book/" + Path) || !Types.contains(Type)) throw QString("Invalid resource");
            const auto Data = Decoded.decoded;
            Total += Data.size();
            if(Data.size() > 16 * 1024 * 1024 || Total > 32 * 1024 * 1024 || Data.size() != Item["bytes"].toInteger()
                || QCryptographicHash::hash(Data, QCryptographicHash::Sha256).toHex() != Item["sha256"].toString().toLatin1())
                throw QString("Resource hash or budget mismatch");
            Result.insert("/book/" + Path, {Data, Type.toLatin1()});
            Item.remove("base64");
            Metadata.append(Item);
        }
        Publication["resources"] = Metadata;
        // Lowercase hexadecimal is also valid CSP base64-value syntax and survives custom-scheme header normalization.
        Publication["scriptNonce"] = QString::fromLatin1(QUuid::createUuid().toByteArray(QUuid::Id128));
        for(const auto Value : Publication["spine"].toArray())
            if(!Result.contains("/book/" + Value.toObject()["path"].toString())) throw QString("Missing spine item");
        Result.insert("/app/publication.json", {QJsonDocument(Publication).toJson(QJsonDocument::Compact), "application/json"});
        const auto Manifest = readObject(Bundle + "/bundle.json", 64 * 1024);
        const auto Files = Manifest["files"].toArray();
        if(Manifest["schemaVersion"].toInt() != 1 || Files.size() != 1) throw QString("Invalid bundle manifest");
        const auto Entry = Files[0].toObject();
        const auto Script = readBounded(Bundle + "/reader.js", 16 * 1024 * 1024);
        if(Entry["path"] != "reader.js" || QCryptographicHash::hash(Script, QCryptographicHash::Sha256).toHex()
            != Entry["sha256"].toString().toLatin1()) throw QString("Bundle hash mismatch");
        Result.insert("/app/reader.js", {Script, "text/javascript"});
        Result.insert("/app/index.html", {QByteArrayLiteral(
            "<!doctype html><html lang='ko'><head><meta charset='utf-8'><title>EPUB 본문</title>"
            "<style>html,body{margin:0;height:100%;overflow:hidden}#viewport{height:100%;margin:auto;max-width:100%}"
            "#reader{position:relative;width:100%;height:100%;margin:auto;max-width:100%}body.hc-preview-width #viewport{max-width:var(--hc-preview-width);outline:1px solid rgba(128,128,128,.45)}"
            ".readium-navigator-iframe{border:0;inset:0;width:100%;height:100%}</style>"
            "</head><body><main id='viewport' aria-label='EPUB 본문'><div id='reader'></div></main><script src='reader.js'></script></body></html>"), "text/html"});
        return Result;
    }

    class Handler final : public QWebEngineUrlSchemeHandler
    {
    public:
        // SourceLines: chapter paths that editing previews annotate with source lines; empty for readers.
        explicit Handler(const Assets& Items, QObject* Parent, std::function<void(const QString&)> Failure, QSet<QString> SourceLines = {})
            : QWebEngineUrlSchemeHandler(Parent), items_(Items), failure_(std::move(Failure)), sourceLines_(std::move(SourceLines))
        { nonce_ = QJsonDocument::fromJson(Items.value("/app/publication.json").data).object()["scriptNonce"].toString().toLatin1(); }
        void requestStarted(QWebEngineUrlRequestJob* Job) override
        {
            const auto Url = Job->requestUrl();
            const auto Path = Url.path();
            if(Url.host() != "app" || Url.port() != -1 || !Url.userInfo().isEmpty() || Url.hasQuery()
                || Job->requestMethod() != "GET" || !items_.contains(Path))
            { Job->fail(QWebEngineUrlRequestJob::RequestDenied); return; }
            const auto Item = items_.value(Path);
            const QByteArray ScriptPolicy = Path.startsWith("/book/") ? QByteArray("'none'") : "epub://app/app/ 'nonce-" + nonce_ + "'";
            Job->setAdditionalResponseHeaders({{"Content-Security-Policy",
                "default-src 'none'; script-src " + ScriptPolicy + "; style-src 'unsafe-inline' epub: blob:; img-src epub: data: blob:; "
                "font-src epub: data:; connect-src epub://app/; frame-src blob: epub:; base-uri epub:; object-src 'none'; form-action 'none'"},
                {"X-Content-Type-Options", "nosniff"}, {"Cache-Control", "private, max-age=3600, immutable"}});
            using Store=hc::epub::poc::PublicationResources;
            const auto Reply=[Guard=QPointer<QWebEngineUrlRequestJob>(Job),Mime=Item.mime](const QByteArray& Data,Store::Lease Lease={}) {
                if(!Guard)return;
                auto* Buffer=new QBuffer(Guard); Buffer->setData(Data); Buffer->open(QIODevice::ReadOnly);
                if(Lease) QObject::connect(Buffer,&QObject::destroyed,[Lease]{});
                Guard->reply(Mime,Buffer);
            };
            if(!Item.store) { Reply(Item.data); return; }
            const auto Resource=Path.mid(6);
            if(const auto Cached=Item.store->Cached(Resource)) { Reply(Cached->bytes_,Cached); return; }
            const auto Lease=Item.store->Reserve(Item.size);
            if(!Lease) { failure_("epub-resource-memory-limit: "+Resource); Job->fail(QWebEngineUrlRequestJob::RequestFailed); return; }
            auto* Watcher=new QFutureWatcher<Store::Lease>(this);
            connect(Watcher,&QFutureWatcher<Store::Lease>::finished,this,[this,Watcher,Guard=QPointer<QWebEngineUrlRequestJob>(Job),Resource,Cache=Item.store,Reply] {
                const auto Data=Watcher->result(); Watcher->deleteLater();
                if(!Guard)return;
                if(!Data->error_.isEmpty()) { failure_(Data->error_); Guard->fail(QWebEngineUrlRequestJob::RequestFailed); return; }
                Cache->Retain(Resource,Data); Reply(Data->bytes_,Data);
            });
            const bool Annotate=sourceLines_.contains(Resource) && Item.mime=="application/xhtml+xml";
            Watcher->setFuture(QtConcurrent::run(Store::Pool(),[Session=Item.store->Session(),Resource,Lease,Annotate] {
                auto Value=Store::Load(Session,Resource,Lease);
                // The cache keeps the annotated bytes, so each chapter is annotated once. Failures serve the original.
                if(Annotate && Value->error_.isEmpty()) {
                    auto Lines=hc::document::epub::AnnotateSourceLines(Value->bytes_);
                    if(Lines.skipped_.isEmpty() && Store::Grow(Value,Lines.bytes_.size()-Value->bytes_.size()))Value->bytes_=std::move(Lines.bytes_);
                }
                return Value;
            }));
        }
    private:
        Assets items_;
        std::function<void(const QString&)> failure_;
        QSet<QString> sourceLines_;
        QByteArray nonce_;
    };

    class Filter final : public QWebEngineUrlRequestInterceptor
    {
    public:
        Filter(const Assets& Items, QObject* Parent) : QWebEngineUrlRequestInterceptor(Parent)
        { for(auto It = Items.cbegin(); It != Items.cend(); ++It) paths_.insert(It.key()); }
        void interceptRequest(QWebEngineUrlRequestInfo& Info) override
        {
            const auto Url = Info.requestUrl();
            const bool Local = Url.scheme() == "epub" && Url.host() == "app" && Url.port() == -1
                && Url.userInfo().isEmpty() && !Url.hasQuery() && paths_.contains(Url.path());
            const bool Transient = Url.scheme() == "blob" || (Url.scheme() == "data"
                && (Info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeImage
                    || Info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeFontResource));
            const bool Allowed = Info.requestMethod() == "GET" && (Local || Transient);
            QMutexLocker Lock(&mutex_);
            ++requests_;
            if(!Allowed) { ++blocked_; if(denied_.size() < 40) denied_.append(Url.toString().left(512)); Info.block(true); }
        }
        QJsonObject Snapshot()
        { QMutexLocker Lock(&mutex_); return {{"requests", requests_}, {"blocked", blocked_}, {"denied", denied_}}; }
    private:
        QSet<QString> paths_;
        QMutex mutex_;
        int requests_ = 0;
        int blocked_ = 0;
        QJsonArray denied_;
    };

    class Page final : public QWebEnginePage
    {
    public:
        Page(QWebEngineProfile* Profile, QObject* Parent) : QWebEnginePage(Profile, Parent) {}
        QJsonArray messages;
    protected:
        void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel Level, const QString& Message, int, const QString&) override
        { if(messages.size() < 40) messages.append(QJsonObject{{"level", int(Level)}, {"message", Message.left(1200)}}); }
        bool acceptNavigationRequest(const QUrl& Url, NavigationType, bool Main) override
        {
            if(Main) return Url == QUrl("epub://app/app/index.html");
            return Url.scheme() == "blob" || Url.toString() == "about:blank" || Url.toString() == "about:srcdoc"
                || (Url.scheme() == "epub" && Url.host() == "app" && Url.path().startsWith("/book/"));
        }
        QWebEnginePage* createWindow(WebWindowType) override { return nullptr; }
    };

    class Window final : public QMainWindow
    {
    public:
        // HostChrome: the embedding host draws the reading row and the status line.
        Window(const Assets& Items, const QJsonObject& Publication, const QString& Engine, const QString& Theme, const QString& Evidence, const QString& SettingsPath, qint64 Generation = 1,bool InitiallyInteractive=true,bool SourceLines=false,const QString& GlobalSettingsPath={},bool HostChrome=false)
            : QMainWindow(nullptr,InitiallyInteractive?Qt::WindowFlags{}:Qt::WindowFlags(Qt::WindowDoesNotAcceptFocus)), evidence_(Evidence), documentId_(Publication["sourceSha256"].toString()), generation_(Generation)
        {
            setAttribute(Qt::WA_ShowWithoutActivating);previewAssets_=Items;engine_=Engine;interactive_=InitiallyInteractive;editingPreview_=SourceLines;
#ifdef HC_EPUB_PRODUCT_HELPER
            setWindowTitle(tr("EPUB 읽기"));
#else
            setWindowTitle(tr("EPUB · Watercolor 읽기 설정 · P0 엔진 비교"));
#endif
            resize(QSize(1280,900).boundedTo(QGuiApplication::primaryScreen()->availableGeometry().size()-QSize(40,60)));
            settingsPath_ = SourceLines && !GlobalSettingsPath.isEmpty()?GlobalSettingsPath:SettingsPath;
            auto* Central = new QWidget(this);
            setCentralWidget(Central);
            auto* Layout = new QVBoxLayout(Central);
            Layout->setContentsMargins(0,0,0,0); Layout->setSpacing(0);
            auto* Splitter = new QSplitter(Central);splitter_=Splitter; Layout->addWidget(Splitter, 1);
            auto* Scroll = new QScrollArea(Splitter); navigationScroll_=Scroll; Scroll->setWidgetResizable(true); Scroll->setMinimumWidth(220);
            auto* Panel = new QWidget; Scroll->setWidget(Panel);
            // The panel grows with the window so the lists below fill the remaining height.
            auto* Form = new QVBoxLayout(Panel);
            auto* EngineRow = new QWidget(Panel); auto* EngineLayout = new QHBoxLayout(EngineRow); EngineLayout->setContentsMargins(0,0,0,0);
            auto* EngineBox = new QComboBox(EngineRow); EngineBox->addItems({"EPUB.js", "Readium Web"});
            EngineBox->setCurrentIndex(Engine == "readium" ? 1 : 0);
            auto* EngineLabel = new QLabel(tr("엔진"), EngineRow); EngineLabel->setBuddy(EngineBox);
            EngineLayout->addWidget(EngineLabel); EngineLayout->addWidget(EngineBox, 1); Form->addWidget(EngineRow);
#ifdef HC_EPUB_PRODUCT_HELPER
            EngineRow->setVisible(false); EngineBox->setEnabled(false);
#endif
            settings_ = new hc::epub::poc::ReadingSettings(settingsPath_, Publication["sourceSha256"].toString(), Theme, Panel);
            if(SourceLines)settings_->SetEditingPreview();
            navigation_=new hc::epub::poc::ReadingNavigation(SettingsPath,documentId_,[this](const QJsonObject& Request){command("read",Request);},Panel);
            navigation_->SetPublication(Publication);
            {
                // Host chapter list: contents entries that lead into the reading order. Entries and title
                // lengths are bounded so the first snapshot stays well inside the pipe payload limit.
                QHash<QString,int> Spine;const auto Order=Publication["spine"].toArray();
                for(int Index=0;Index<Order.size();++Index)Spine.insert(Order[Index].toObject()["path"].toString(),Index);
                qsizetype Characters=0;
                for(const auto& Value:Publication["navigation"].toArray()) {
                    const auto Entry=Value.toObject();const int Chapter=Spine.value(Entry["path"].toString(),-1);
                    if(Chapter<0)continue;
                    const auto Title=(Entry["label"].toString().isEmpty()?Entry["path"].toString():Entry["label"].toString()).left(128);
                    if(toc_.size()>=512 || (Characters+=Title.size())>32768)break;
                    toc_.append(QJsonObject{{"title",Title},{"chapter",Chapter}});
                }
            }
            auto* ReadingBar=addToolBar(tr("독서"));ReadingBar->setObjectName("epubReadingToolbar");ReadingBar->setMovable(false);ReadingBar->setVisible(!SourceLines);
            for(const auto& Entry:QList<QPair<QString,QString>>{{"back",tr("이전 위치")},{"previous",tr("이전")},{"next",tr("다음")}}) {
                auto* Action=ReadingBar->addAction(Entry.second);Action->setObjectName("epub."+Entry.first);
                if(Entry.first=="previous")Action->setToolTip(tr("이전 화면 · PgUp / 숫자 키패드 PgUp"));
                if(Entry.first=="next")Action->setToolTip(tr("다음 화면 · PgDown / 숫자 키패드 PgDown"));
                connect(Action,&QAction::triggered,this,[this,Name=Entry.first]{navigation_->Execute(Name);view_->setFocus(Qt::OtherFocusReason);});
            }
            location_=new QLabel(this);location_->setTextFormat(Qt::PlainText);location_->setObjectName("epubReadingLocation");location_->setAccessibleName(tr("현재 읽기 위치"));location_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);location_->setMinimumWidth(0);statusBar()->addPermanentWidget(location_,1);
            Form->addWidget(navigation_, 1);
            settingsScroll_=new QScrollArea(Splitter);settingsScroll_->setObjectName("epubQuickSettings");settingsScroll_->setWidgetResizable(true);settingsScroll_->setMinimumWidth(320);settingsScroll_->setWidget(settings_);
            view_ = new QWebEngineView(Splitter);view_->setFocusPolicy(InitiallyInteractive?Qt::StrongFocus:Qt::NoFocus); Splitter->setSizes({260,360,880});
            fitNavigationWidth();
            navigationScroll_->hide();settingsScroll_->hide();
            // Book frames report their own right clicks; this covers the page margins around them
            // and keeps the browser's own menu away.
            view_->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(view_,&QWidget::customContextMenuRequested,this,[this](const QPoint& Position){
                if(!contextMenus_)return;
                const auto At=view_->mapTo(this,Position);
                contextRequest_={{"serial",++contextSerial_},{"x",At.x()},{"y",At.y()},{"selection",""},{"link",""},{"anchor",QJsonObject{}},
                    {"locator",latest_.value("locator").toString()},{"chapter",latest_.value("chapterIndex").toInt(-1)},{"target",QJsonObject{{"kind","margin"}}}};
            });
            for(const auto& Entry:QList<QPair<QString,QString>>{{"focus-toc",tr("목차")},{"focus-search",tr("검색")},{"focus-bookmarks",tr("책갈피")},{"focus-notes",tr("형광펜·메모")},{"focus-settings",tr("Aa · 읽기 설정")}}) {
                if(SourceLines && Entry.first=="focus-settings")continue;
                auto* Action=ReadingBar->addAction(Entry.second);Action->setObjectName("epub."+Entry.first);
                Action->setCheckable(true);
                connect(Action,&QAction::triggered,this,[this,Name=Entry.first]{readAction("toggle-"+Name.mid(6),{});});
            }
            layoutChoice_=new QComboBox(ReadingBar);layoutChoice_->setAccessibleName(tr("읽기 방식"));layoutChoice_->addItems({tr("세로 스크롤"),tr("가로 페이지")});ReadingBar->addWidget(layoutChoice_)->setVisible(!SourceLines);
            connect(layoutChoice_,&QComboBox::currentIndexChanged,this,[this](int Index){
                const bool Dirty=settings_->IsDirty();
                if(settings_->Execute("preview",{{"layout",Index==1?"paginated":"scroll"}},false)) {
                    if(!Dirty && settings_->CssValid())settings_->Execute("apply",{},false);else readAction("focus-settings",{});
                }
                focusReading();
            });
            columnsChoice_=new QComboBox(ReadingBar);columnsChoice_->setObjectName("epubColumns");columnsChoice_->setAccessibleName(tr("페이지 단 수"));
            columnsChoice_->addItems({tr("자동 단 수"),tr("1단"),tr("2단")});ReadingBar->addWidget(columnsChoice_)->setVisible(!SourceLines);
            columnsChoice_->setToolTip(tr("가로 페이지에서 단 수를 선택합니다. 화면이 좁으면 1단으로 표시합니다."));
            connect(columnsChoice_,&QComboBox::currentIndexChanged,this,[this](int Index){
                const bool Dirty=settings_->IsDirty();
                if(settings_->Execute("preview",{{"columns",Index}},false)) {
                    if(!Dirty && settings_->CssValid())settings_->Execute("apply",{},false);else readAction("focus-settings",{});
                }
                focusReading();
            });
            if(HostChrome) {
                // The host draws the reading row and shows the location in its own status bar;
                // the hidden actions keep their keys, enablement and the state they report.
                ReadingBar->setVisible(false); statusBar()->setVisible(false);
            }
            profile_ = new QWebEngineProfile(this); profile_->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);profile_->setHttpCacheMaximumSize(32*1024*1024);
            profile_->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
            filter_ = new Filter(Items, profile_); profile_->setUrlRequestInterceptor(filter_);
            // Editing previews read source lines from their chapters; readers keep the publisher bytes.
            QSet<QString> Chapters;if(SourceLines)for(const auto& Value:Publication["spine"].toArray())Chapters.insert(Value.toObject()["path"].toString());
            profile_->installUrlSchemeHandler("epub", new Handler(Items, profile_,[this](const QString& Error){resourceFailure_=Error;},std::move(Chapters)));
            page_ = new Page(profile_, view_); view_->setPage(page_);page_->settings()->setAttribute(QWebEngineSettings::FocusOnNavigationEnabled,false);
            for(auto Setting : {QWebEngineSettings::LocalContentCanAccessFileUrls, QWebEngineSettings::LocalContentCanAccessRemoteUrls,
                QWebEngineSettings::JavascriptCanOpenWindows, QWebEngineSettings::JavascriptCanAccessClipboard,
                QWebEngineSettings::LocalStorageEnabled, QWebEngineSettings::PluginsEnabled, QWebEngineSettings::ScreenCaptureEnabled})
                page_->settings()->setAttribute(Setting, false);
            connect(profile_, &QWebEngineProfile::downloadRequested, this, [](QWebEngineDownloadRequest* Request) { Request->cancel(); });
            connect(page_, &QWebEnginePage::permissionRequested, this, [](QWebEnginePermission Permission) { Permission.deny(); });
            connect(page_, &QWebEnginePage::renderProcessTerminated, this, [this](auto, int Code) { finish({{"status", "error"}, {"message", "render process terminated"}, {"exitCode", Code}}); });
            debounce_.setSingleShot(true); debounce_.setInterval(160);
            connect(&debounce_, &QTimer::timeout, this, [this] { sendProfile(); });
            connect(settings_, &hc::epub::poc::ReadingSettings::ProfileChanged, this, [this] { debounce_.start(); });
            connect(EngineBox, &QComboBox::currentIndexChanged, this, [this](int Index) { command("start", Index ? "readium" : "epubjs"); });
            connect(page_, &QWebEnginePage::loadFinished, this, [this, Engine](bool Ok) {
                if(!Ok) { finish({{"status", "error"}, {"message", "shell load failed"}}); return; }
                shellReady_=true; sendProfile();
                if(contextMenus_)command("read",QJsonObject{{"action","context-menu"},{"value",true}});
            });
            connect(&poll_, &QTimer::timeout, this, [this] {
                if(pending_) return;
                pending_ = true;
                // While the preview scrolls, the periodic detailed snapshot waits; only commands still ask for one.
                const bool Scrolling=scrollActive_.isValid() && scrollActive_.elapsed()<500;
                const bool Detailed=detailPending_ || !detailClock_.isValid() || (!Scrolling && detailClock_.elapsed()>=500);
                if(Detailed){detailPending_=false;detailClock_.restart();++detailSnapshots_;}
                const auto Snapshot=Detailed?QStringLiteral("window.hcSnapshot"):QStringLiteral("window.hcNativeSnapshot || window.hcSnapshot");
                page_->runJavaScript("JSON.stringify({...(("+Snapshot+")?.() || window.hcEvidence), startup: window.hcStartupDiagnostics ? window.hcStartupDiagnostics() : {}})", [this,Detailed](const QVariant& Value) {
                    pending_ = false; if(finished_) return;
                    auto Result = QJsonDocument::fromJson(Value.toString().toUtf8()).object();
                    if(!resourceFailure_.isEmpty()) { Result["status"]="error"; Result["message"]=resourceFailure_; }
                    if(Result["frameId"]!=latest_["frameId"] && !Detailed)detailPending_=true;
                    if(!Detailed){auto Merged=latest_;for(auto At=Result.begin();At!=Result.end();++At)Merged[At.key()]=At.value();Result=std::move(Merged);}
                    latest_ = Result;
                    previewReported();
                    // Frame clicks are relative to the web view; the host places its menu in this window.
                    const auto Context=Result.value("contextRequest").toObject();const auto ContextSerial=Context["serial"].toInteger();
                    if(ContextSerial>0 && ContextSerial!=pageContextSeen_) {
                        pageContextSeen_=ContextSerial;const auto Offset=view_->mapTo(this,QPoint{});
                        contextRequest_=Context;contextRequest_["serial"]=++contextSerial_;
                        contextRequest_["x"]=Context["x"].toInt()+Offset.x();contextRequest_["y"]=Context["y"].toInt()+Offset.y();
                    }
                    const auto ScrollSerial=Result["previewScroll"].toObject()["serial"].toInteger(-1);
                    if(ScrollSerial!=scrollSerialSeen_){scrollSerialSeen_=ScrollSerial;scrollActive_.restart();}
                    const auto Stamp=QFileInfo(settingsPath_).lastModified().toMSecsSinceEpoch();
                    if(Stamp!=settingsStamp_ && !settings_->IsDirty() && !settings_->IsBusy()){settingsStamp_=Stamp;settings_->Execute("reload",{},false);}
                    if(!Result["readerBusy"].toBool()) {
                        navigation_->Poll(Result);
                        const auto Click=Result["annotationClicked"].toObject();if(Click["serial"].toInteger()>annotationClickSeen_){annotationClickSeen_=Click["serial"].toInteger();readAction("annotation-select",Click["id"]);}
                        while(!pendingBookmarks_.isEmpty())navigation_->Execute("bookmark-add",pendingBookmarks_.takeAt(0));
                    }
                    const bool Fixed=Result["contentLayout"]=="pre-paginated";settings_->SetFixedLayout(Fixed);const auto WritingModes=Result["writingModes"].toArray();const bool Vertical=WritingModes.contains("vertical-rl") || WritingModes.contains("vertical-lr");settings_->SetVerticalWriting(Vertical);
                    {QSignalBlocker Block(layoutChoice_);layoutChoice_->setCurrentIndex(Fixed || settings_->Profile()["layout"]=="paginated"?1:0);layoutChoice_->setEnabled(!Fixed);}
                    {QSignalBlocker Block(columnsChoice_);columnsChoice_->setCurrentIndex(Fixed || Vertical?1:settings_->Profile()["columns"].toInt());columnsChoice_->setEnabled(!Fixed && !Vertical && settings_->Profile()["layout"]=="paginated");}
                    if(Result["status"]=="ready") {
                        location_->setText(tr("%1 · 장 %2/%3 · 화면 %4/%5 · 전체 약 %6% (장 기준)")
                            .arg(Result["chapterTitle"].toString()).arg(Result["chapterIndex"].toInt()+1).arg(Result["chapterCount"].toInt())
                            .arg(screens(Result,"chapterPage")).arg(screens(Result,"chapterPages")).arg(qRound(Result["progress"].toDouble()*100)));
                        findChild<QAction*>("epub.back")->setEnabled(Result["canBack"].toBool());
                        // Busy navigation is already bounded by the reader queue.
                        // Keep toolbar contrast stable between interior pages.
                        for(const auto& Entry:QList<QPair<QString,bool>>{{"previous",!Result["atStart"].toBool()},{"next",!Result["atEnd"].toBool()}}) {
                            auto* Action=findChild<QAction*>("epub."+Entry.first);
                            if(Action->isEnabled()!=Entry.second){++toolbarAvailabilityChanges_;Action->setEnabled(Entry.second);}
                        }
                    }
                    if(Result["profile"].toObject()["customCss"]==settings_->Profile()["customCss"])
                        settings_->SetCssValidation(settings_->Profile()["customCss"].toString(),astError_.isEmpty()?Result["cssError"].toString():astError_);
                    QStringList Families;
                    for(const auto& Family : Result["bookFonts"].toArray()) if(Family.isString()) Families.append(Family.toString());
                    settings_->SetBookFonts(Families);
                    const auto Status = Result["status"].toString();
                    // A scrolling preview reports every 33 ms for half a second, then falls back to the idle rate.
                    poll_.setInterval(Status=="ready" && !Result["readerBusy"].toBool() && !(scrollActive_.isValid() && scrollActive_.elapsed()<500)?250:33);
                    statusBar()->clearMessage();
                    if(!evidence_.isEmpty() && Status == "ready" && !testing_) {
                        testing_ = true;
                        if(!Families.isEmpty()) {
                            auto* Fonts = settings_->findChild<QComboBox*>("font");
                            const int Index = Fonts->findData(Families.first());
                            if(Index >= 0) { expectedBookFont_ = Families.first(); Fonts->setCurrentIndex(Index); debounce_.stop(); sendProfile(); }
                        }
                        command("test", QJsonValue());
                    }
                    if(Status == "error" || Status == "failed" || Status == "passed") finish(Result);
                });
            });
            if(!Evidence.isEmpty()) QTimer::singleShot(60000, this, [this] { if(!finished_) finish({{"status", "timeout"}}); });
            view_->setAccessibleName(tr("EPUB 본문"));
            shortcuts_=new hc::document::DocumentShortcutRouter(this);
            for(const auto& Entry:hc::document::DocumentShortcutRegistry::Commands())if(Entry.scope_=="epub") {
                auto* Action=new QAction(Entry.label_,this);addAction(Action);shortcuts_->Register(Entry.id_,Action);
                connect(Action,&QAction::triggered,this,[this,Name=Entry.id_.mid(5)]{readAction(Name,{});});
            }
            shortcuts_->SetEligibility([this](const QString& Id,QWidget* Focus){return Id.startsWith("epub.") && !composing_
                && (latest_["popup"].isNull() || latest_["popup"].isUndefined() || Id=="epub.fullscreen")
                && ((!qobject_cast<QLineEdit*>(Focus) && !qobject_cast<QPlainTextEdit*>(Focus) && !qobject_cast<QAbstractSpinBox*>(Focus) && !qobject_cast<QComboBox*>(Focus) && !qobject_cast<QAbstractItemView*>(Focus))
                    || Id=="epub.cycle-focus" || Id.startsWith("epub.focus-"));});
            shortcuts_->Apply({});
            auto* PanelKey=new QShortcut(QKeySequence(hc::defaults::edit::AuxiliaryShortcut("reading.panel")),this);connect(PanelKey,&QShortcut::activated,this,[this]{if(!composing_){if(navigationScroll_->isVisible()){navigationScroll_->hide();focusReading();}else readAction("focus-toc",{});}});
            qApp->installEventFilter(this);
            view_->setUrl(QUrl("epub://app/app/index.html"));
        }
        bool Appearance(const QJsonObject& Value) {
            if(!hc::document::epubui::ApplyAppearance(Value))return false;
            hostAppearance_=Value;const auto Palette=fm::style::ThemeManager::instance().colors().toPalette();QApplication::setPalette(Palette);setPalette(Palette);fitNavigationWidth();sendProfile();return true;
        }
        ~Window() override { finished_ = true; poll_.stop(); delete view_; view_ = nullptr; delete profile_; profile_ = nullptr; }
        // A reader made by a reload answers to the identity the owner opened the helper with.
        void SetControlIdentity(const QString& DocumentId,qint64 Generation) { controlId_=DocumentId; controlGeneration_=Generation; }
        // Serials the owner compares across replacement keep growing in the new reader.
        void InheritCounters(const Window& Old) {
            escapeRequests_=Old.escapeRequests_;fullscreenRequests_=Old.fullscreenRequests_;
            contextSerial_=Old.contextSerial_;contextRequest_=QJsonObject{{"serial",contextSerial_}};
        }
        QJsonObject Control(const QJsonObject& Request)
        {
            const auto& Id=controlId_.isEmpty()?documentId_:controlId_;const auto Generation=controlId_.isEmpty()?generation_:controlGeneration_;
            QJsonObject Reply{{"protocolVersion", 1}, {"documentId", Id}, {"generation", Generation}};
            const auto Fail = [&](const QString& Error) { Reply["error"] = Error; return Reply; };
            if(Request["protocolVersion"] != QJsonValue(1)
                || Request["documentId"] != Id || Request["generation"] != QJsonValue(Generation)) return Fail("epub-control-identity");
            const auto Kind = Request["kind"].toString();
            if(Kind == "snapshot" || Kind == "snapshot-delta") {
                auto State = latest_;const auto Navigation=navigation_->Snapshot();for(auto I=Navigation.begin();I!=Navigation.end();++I)State[I.key()]=I.value(); State["inputFocused"] = latest_["readerFocused"].toBool();
                State["escapeRequests"]=escapeRequests_;State["fullscreenRequests"]=fullscreenRequests_;State["appearance"] = hostAppearance_; State["settingsPersistent"] = !settingsPath_.isEmpty();State["inputEnabled"]=interactive_;
                State["chromeWindowColor"]=palette().color(QPalette::Window).name();State["toolbarWindowColor"]=findChild<QToolBar*>("epubReadingToolbar")->palette().color(QPalette::Window).name();State["openPanel"]=openPanel();State["toolbarPreviousEnabled"]=findChild<QAction*>("epub.previous")->isEnabled();State["toolbarNextEnabled"]=findChild<QAction*>("epub.next")->isEnabled();State["toolbarAvailabilityChanges"]=toolbarAvailabilityChanges_;
                State["readingToolbarVisible"]=findChild<QToolBar*>("epubReadingToolbar")->isVisible();
                State["toolbarColumns"]=columnsChoice_->currentIndex();State["columnsControlEnabled"]=columnsChoice_->isEnabled();
                State["readingSettingsAvailable"] = !editingPreview_;State["settingsBusy"] = settings_->IsBusy(); State["settingsDirty"] = settings_->IsDirty();State["detailSnapshots"]=detailSnapshots_;
                State["readingProfile"] = settings_->Profile(); State["cssValid"]=settings_->CssValid(); State["cssValidationError"]=settings_->CssError();
                // Host chrome: chapter list, button availability, screens as the reader counts them, menus and messages.
                // Unchanged values, such as the chapter list, are not repeated in deltas.
                State["toc"]=toc_;State["readerMessage"]=readerMessage_;State["contextRequest"]=contextRequest_;
                // The editing preview's request state: accepted, checked and shown revisions of the chapter.
                if(editingPreview_)State["previewPatch"]=previewResult_;
                for(const auto* Key:{"canBack","atStart","atEnd"})State[QLatin1String(Key)]=latest_.value(QLatin1String(Key)).toBool();
                if(latest_.contains("chapterPages")){State["chapterScreen"]=screens(latest_,"chapterPage");State["chapterScreens"]=screens(latest_,"chapterPages");}
                if(Kind=="snapshot-delta") {
                    QJsonObject Delta;QJsonArray Removed;
                    for(auto At=State.begin();At!=State.end();++At)if(!sentState_.contains(At.key()) || sentState_[At.key()]!=At.value())Delta[At.key()]=At.value();
                    for(auto At=sentState_.begin();At!=sentState_.end();++At)if(!State.contains(At.key()))Removed.append(At.key());
                    Reply["stateDelta"]=true;Reply["replaceState"]=sentState_.isEmpty();Reply["removed"]=Removed;Reply["state"]=Delta;sentState_=State;
                } else Reply["state"]=State;
            }
            else if(Kind=="flush-reading") {
                if(Request.size()!=4 || !navigation_->Flush())return Fail("epub-reading-save");
            }
            else if(Kind == "reading-settings") {
                if(Request.size()!=7 || !Request["profile"].isObject() || !Request["global"].isBool()
                    || !settings_->Execute(Request["action"].toString(),Request["profile"].toObject(),Request["global"].toBool())) return Fail("epub-settings-rejected");
            }
            else if(Kind=="reader-action") {
                if(Request.size()!=6 || !readAction(Request["action"].toString(),Request["value"]))return Fail("epub-reader-action-rejected");
            }
            else if(Kind == "appearance") {
                if(!Appearance(Request["appearance"].toObject()))return Fail("epub-appearance-invalid");
            }
            else if(Kind == "start") {
                const auto Engine = Request["engine"].toString();
                if(Engine != "epubjs" && Engine != "readium") return Fail("epub-control-engine");
#ifdef HC_EPUB_PRODUCT_HELPER
                if(Engine != "epubjs") return Fail("epub-product-engine");
#endif
                latest_ = {{"status", "loading"}}; command("start", Engine);
            }
            else if(Kind == "test") { latest_ = {{"status", "testing"}}; command("test", QJsonValue()); }
            else if(Kind == "focus") {
                // Native ancestors include the owner tab and its parking window.
                // A late command must not activate a tab that has been hidden.
                const bool Visible = IsWindowVisible(reinterpret_cast<HWND>(winId()));
                if(Visible && interactive_) focusReading();
                Reply["focusRequested"] = Visible && interactive_;
            }
            else if(Kind == "source-scroll") {
                // Scrolling bypasses the reader command queue: no busy state and no detailed snapshot.
                if(Request.size()!=5 || !sourceScroll(Request["value"]))return Fail("epub-source-scroll-invalid");
            }
            else return Fail("epub-control-command");
            Reply["accepted"] = true;
            return Reply;
        }
    protected:
        // The reading row cannot be hidden from a context menu; the host owns that chrome.
        QMenu* createPopupMenu() override { return nullptr; }
        void resizeEvent(QResizeEvent* Event) override {
            QMainWindow::resizeEvent(Event);
            if(splitter_)splitter_->setOrientation(width()<700?Qt::Vertical:Qt::Horizontal);
        }
        bool eventFilter(QObject* Object,QEvent* Event) override {
            auto* Widget=qobject_cast<QWidget*>(Object);if(!Widget || (Widget!=this && !isAncestorOf(Widget)))return false;
            if(Event->type()==QEvent::InputMethod)composing_=!static_cast<QInputMethodEvent*>(Event)->preeditString().isEmpty();
            if(Event->type()==QEvent::KeyPress) {
                auto* Key=static_cast<QKeyEvent*>(Event);
                if(Key->key()==Qt::Key_Escape && Key->modifiers()==Qt::NoModifier && !Key->isAutoRepeat() && !composing_ && !QApplication::activeModalWidget() && !QApplication::activePopupWidget()) {
                    const QPointer<Window> Guard(this);const QPointer<QWidget> Receiver(Widget);
                    page_->runJavaScript("!!(window.hcDismissPopup && window.hcDismissPopup())",[Guard,Receiver](const QVariant& Closed){
                        if(!Guard || Closed.toBool())return;
                        if(auto* Input=qobject_cast<QLineEdit*>(Receiver.data());Input && !Input->text().isEmpty()){Input->clear();return;}
                        if(Guard->settings_->IsDirty()){Guard->settings_->Execute("cancel",{},false);return;}
                        if(Guard->settingsScroll_->isVisible() || Guard->navigationScroll_->isVisible()){Guard->settingsScroll_->hide();Guard->navigationScroll_->hide();Guard->updatePanelActions();Guard->focusReading();return;}
                        ++Guard->escapeRequests_;
                    });return true;
                }
            }
            return QMainWindow::eventFilter(Object,Event);
        }
    private:
        // Screens per chapter as the reader shows them: a spread of columns is one screen.
        static int screens(const QJsonObject& State,const char* Key) {
            const int Columns=qMax(1,State.value("columnCount").toInt(1));
            return (qMax(1,State.value(QLatin1String(Key)).toInt(1))+Columns-1)/Columns;
        }
        // Tab names stay whole: the side panel is never narrower than the tab row.
        void fitNavigationWidth() {
            if(!navigationScroll_ || !navigation_)return;
            const int Frame=2*navigationScroll_->frameWidth()+navigationScroll_->verticalScrollBar()->sizeHint().width();
            const auto* Layout=navigation_->parentWidget()?navigation_->parentWidget()->layout():nullptr;
            const int Margins=Layout?Layout->contentsMargins().left()+Layout->contentsMargins().right():0;
            navigationScroll_->setMinimumWidth(qMax(220,navigation_->minimumSizeHint().width()+Margins+Frame));
        }
        QString openPanel() const {
            if(settingsScroll_->isVisible())return "settings";
            if(!navigationScroll_->isVisible())return {};
            const auto Index=navigation_->findChild<QTabWidget*>()->currentIndex();
            return Index==1?"search":Index==2?"bookmarks":Index==3?"notes":"toc";
        }
        void updatePanelActions() {
            const auto Panel=openPanel();
            for(const auto& Name:QStringList{"toc","search","bookmarks","notes","settings"})
                if(auto* Action=findChild<QAction*>("epub.focus-"+Name)){QSignalBlocker Block(Action);Action->setChecked(Panel==Name);}
        }
        void focusReading() {
            if(!interactive_)return;activateWindow();view_->setFocus(Qt::OtherFocusReason);
            page_->runJavaScript("window.hcFocusReading && window.hcFocusReading()");
        }
        bool sourceScroll(const QJsonValue& Value) {
            const auto Request=Value.toObject();
            if(!started_ || !Value.isObject() || !hc::document::epub::ValidSourceScrollRequest(Request))return false;
            page_->runJavaScript("window.hcSourceScroll && window.hcSourceScroll("+QString::fromUtf8(QJsonDocument(Request).toJson(QJsonDocument::Compact))+")");
            scrollActive_.restart();if(poll_.interval()>33)poll_.start(33);return true;
        }
        bool readAction(const QString& Action,const QJsonValue& Value) {
            if(Action=="preview-chapter" || Action=="preview-edit")return previewSource(Action,Value.toObject());
            if(Action=="enable-input"){if(!Value.isBool() || !windowHandle())return false;interactive_=Value.toBool();view_->setFocusPolicy(interactive_?Qt::StrongFocus:Qt::NoFocus);windowHandle()->setFlag(Qt::WindowDoesNotAcceptFocus,!interactive_);return true;}
            if(Action=="fullscreen"){++fullscreenRequests_;return true;}
            // The host shows the body menu. It enables it on load, possibly while the saved position is
            // still being restored, so this skips the reading-state gate and the reader's result slot.
            if(Action=="context-menu"){if(!Value.isBool())return false;contextMenus_=Value.toBool();command("read",QJsonObject{{"action",Action},{"value",Value}});return true;}
            if(Action=="bindings") {
                if(!Value.isObject() || !shortcuts_->Apply(Value.toObject()).isEmpty())return false;
                command("bindings",hc::document::DocumentShortcutRegistry::Bindings(Value.toObject()));return true;
            }
            if(Action.startsWith("toggle-") && QStringList{"toc","search","bookmarks","notes","settings"}.contains(Action.mid(7))) {
                if(openPanel()==Action.mid(7)){settingsScroll_->hide();navigationScroll_->hide();updatePanelActions();focusReading();return true;}
                return readAction("focus-"+Action.mid(7),Value);
            }
            if(Action=="cycle-focus") {
                if(view_->hasFocus() || view_->isAncestorOf(QApplication::focusWidget()))return readAction("focus-toc",{});
                if(!editingPreview_ && navigation_->isAncestorOf(QApplication::focusWidget()))return readAction("focus-settings",{});
                focusReading();return true;
            }
            if(Action=="focus-settings" && editingPreview_)return false;
            if(Action=="focus-settings"){navigationScroll_->hide();settingsScroll_->show();updatePanelActions();settings_->findChild<QTabWidget*>()->setFocus(Qt::ShortcutFocusReason);return true;}
            if(Action=="annotation-select"){settingsScroll_->hide();navigationScroll_->show();navigation_->Execute("focus-notes");}
            if(Action=="focus-search" || Action=="focus-toc" || Action=="focus-bookmarks" || Action=="focus-notes"){settingsScroll_->hide();navigationScroll_->show();}
            if(Action=="bookmark-add" && latest_["readerBusy"].toBool()) {
                if(pendingBookmarks_.size()>=16)return false;
                pendingBookmarks_.append(Value);return true;
            }
            const auto Result=navigation_->Execute(Action,Value);updatePanelActions();return Result;
        }
        void command(const QString& Name, const QJsonValue& Value)
        {
            if(finished_)return;
            const bool PageMove=Name=="read" && QStringList{"next","previous"}.contains(Value.toObject()["action"].toString());
            if(!PageMove)detailPending_=true;
            auto Args = QJsonDocument(QJsonArray{Name, Value}).toJson(QJsonDocument::Compact);
            Args = Args.mid(1, Args.size() - 2);
            page_->runJavaScript("window.hcCommand && window.hcCommand(" + QString::fromUtf8(Args) + ")");
            if(started_ && (Name=="read" || Name=="profile" || Name=="chapter" || Name=="preview-chapter"))poll_.start(16);
        }
        // Editing preview: the owner sends a chapter once ("preview-chapter") and then the bytes that
        // changed ("preview-edit", UTF-8 offsets into the accepted text). The helper keeps the text,
        // checks the whole chapter off the GUI thread and sends the reader the top-level elements that
        // changed, or the chapter when that is not possible. One request is in flight at a time.
        bool previewSource(const QString& Action,const QJsonObject& Patch) {
            const auto Path=Patch["path"].toString();const auto Asset=previewAssets_.value("/book/"+Path);
            const auto Serial=Patch["serial"].toInteger(-1),Revision=Patch["revision"].toInteger(-1);
            const bool Replace=Action=="preview-chapter";
            if(!editingPreview_ || !started_ || previewResult_["status"]!="done" || Serial<=previewResult_["serial"].toInteger()
                || Revision<0 || Revision>9007199254740991LL || Patch["chapter"].toInt(-1)<0 || !Patch["scroll"].isObject()
                || !Asset.store || Asset.mime!="application/xhtml+xml" || Patch.size()!=(Replace?6:10) || !Patch[Replace?"text":"inserted"].isString())return false;
            const auto Finish=[this,Serial,Path](const QString& Error) {
                const auto& Source=previewSources_[Path];
                previewResult_={{"serial",Serial},{"status","done"},{"path",Path},{"accepted",Source.accepted_},{"validated",Source.validated_},{"rendered",Source.rendered_}};
                if(!Error.isEmpty())previewResult_["error"]=Error;
            };
            auto& Source=previewSources_[Path];
            if(Replace)Source.bytes_=Patch["text"].toString().toUtf8();
            else {
                const auto Base=Patch["baseRevision"].toInteger(-2),Length=Patch["baseLength"].toInteger(-1),Start=Patch["start"].toInteger(-1),Removed=Patch["removed"].toInteger(-1);
                // An edit applies only to the exact text it was computed on; anything else asks for the chapter.
                if(Base<0 || Base!=Source.accepted_ || Length!=Source.bytes_.size() || Start<0 || Removed<0 || Start>Length-Removed || Revision<=Base){Finish("preview-resync");return true;}
                Source.bytes_.replace(Start,Removed,Patch["inserted"].toString().toUtf8());
            }
            Source.accepted_=Revision;
            if(Source.bytes_.isEmpty() || Source.bytes_.size()>hc::document::epub::InputLimit){Finish("xml-limit");return true;}
            previewResult_={{"serial",Serial},{"status","checking"},{"path",Path},{"accepted",Revision},{"validated",Source.validated_},{"rendered",Source.rendered_}};
            // Element patches need a scrolled, horizontal, reflowable chapter without user CSS; the reader checks its frame again.
            const auto Profile=settings_->Profile();const auto Modes=latest_["writingModes"].toArray();
            const bool Elements=Source.rendered_>=0 && Profile["layout"]=="scroll" && !Profile["cssEnabled"].toBool()
                && latest_["contentLayout"]!="pre-paginated" && !Modes.contains("vertical-rl") && !Modes.contains("vertical-lr");
            auto* Watcher=new QFutureWatcher<PreviewWork>(this);
            connect(Watcher,&QFutureWatcherBase::finished,this,[this,Watcher,Patch,Serial,Path,Revision,Finish]{
                auto Work=Watcher->result();Watcher->deleteLater();
                if(finished_ || previewResult_["serial"].toInteger()!=Serial)return;
                // An invalid revision stays accepted, so the next edit applies to it; the reader keeps its last good frame.
                // The owner compares this digest with its buffer once no edit followed (verifyPreviewSource).
                if(!Work.checked_){Finish(Work.checked_.error_);previewResult_["sha256"]=QString::fromLatin1(Work.sha256_);return;}
                previewSources_[Path].validated_=Revision;previewResult_["sha256"]=QString::fromLatin1(Work.sha256_);
                QJsonObject Message{{"serial",Serial},{"attempt",1},{"chapter",Patch["chapter"]},{"path",Path},{"scroll",Patch["scroll"]}};
                if(Work.elements_.valid_) {
                    const auto& Change=Work.elements_;
                    Message["elements"]=QJsonObject{{"from",Change.from_},{"removed",Change.removed_},{"inserted",Change.inserted_},{"blocks",Change.blocks_},
                        {"previous",Change.blocks_-Change.inserted_+Change.removed_},{"html",QString::fromUtf8(Change.html_)},{"namespaces",QString::fromUtf8(Change.namespaces_)},{"shift",Change.shift_}};
                } else Message["text"]=QString::fromUtf8(Work.annotated_);
                previewChecked_=std::move(Work.checked_.bytes_);previewAnnotated_=std::move(Work.annotated_);
                previewMessage_=Message;previewResult_["status"]="rendering";previewResult_["validated"]=Revision;
                command("preview-chapter",Message);
            });
            Watcher->setFuture(QtConcurrent::run([Session=Asset.store->Session(),Path,Bytes=Source.bytes_,Rendered=Elements?Source.renderedSource_:QByteArray{}] {
                PreviewWork Work;Work.sha256_=QCryptographicHash::hash(Bytes,QCryptographicHash::Sha256).toHex();
                Work.checked_=Session->PreviewChapter(Path,Bytes);if(!Work.checked_)return Work;
                // A chapter that cannot be annotated is shown without source lines, as the first build serves it.
                auto Lines=hc::document::epub::AnnotateSourceLines(Work.checked_.bytes_);const bool Annotated=Lines.skipped_.isEmpty();
                Work.annotated_=Annotated?std::move(Lines.bytes_):Work.checked_.bytes_;
                if(!Rendered.isEmpty() && Annotated)Work.elements_=hc::document::epub::DiffBodyElements(Rendered,Work.checked_.bytes_,Work.annotated_);
                return Work;
            }));
            return true;
        }
        // The reader's answer to the request in flight. A refused element patch is retried once with the chapter.
        void previewReported() {
            if(previewResult_["status"]!="rendering")return;
            const auto Reported=latest_["previewPatch"].toObject();
            if(Reported["serial"].toInteger(-1)!=previewResult_["serial"].toInteger() || Reported["attempt"]!=previewMessage_["attempt"])return;
            auto& Source=previewSources_[previewResult_["path"].toString()];
            if(Reported.contains("error") && previewMessage_.contains("elements")) {
                auto Message=previewMessage_;Message.remove("elements");Message["text"]=QString::fromUtf8(previewAnnotated_);Message["attempt"]=2;
                previewMessage_=Message;previewResult_["elementsRefused"]=Reported["error"];command("preview-chapter",Message);return;
            }
            previewResult_["status"]="done";
            if(Reported.contains("error"))previewResult_["error"]=Reported["error"];
            else {
                Source.rendered_=previewResult_["accepted"].toInteger();Source.renderedSource_=previewChecked_;
                previewResult_["rendered"]=Source.rendered_;previewResult_["mode"]=Reported["mode"];previewResult_["ms"]=Reported["ms"];
            }
            previewChecked_.clear();previewAnnotated_.clear();previewMessage_={};
        }
        void sendProfile()
        {
            if(!shellReady_) return;
            const auto Values=settings_->Profile(); const auto Css=Values["customCss"].toString();
            navigation_->ConfigureProfile(Values);
            const auto Serial=++profileSerial_; const QPointer<Window> Guard(this);
            const auto Apply=[Guard,Values,Serial](const QString& CssError) {
                if(!Guard || Guard->profileSerial_!=Serial)return;
                auto* Self=Guard.data(); const auto Css=Values["customCss"].toString();
                Self->astError_=CssError; Self->settings_->SetCssValidation(Css,CssError);
                auto Rendered=Values; if(!CssError.isEmpty()) Rendered["cssEnabled"]=false;
                if(Self->hostAppearance_.isEmpty()) hc::document::DocumentThemeAdapter::SetMode(Values["theme"]=="dark"?"dark":"light");
                const auto Palette=fm::style::ThemeManager::instance().colors().toPalette();
                Self->command("palette",QJsonObject{{"background",Palette.color(QPalette::Base).name()},{"foreground",Palette.color(QPalette::Text).name()}});
                const auto Asset=Values["background"]=="image"?Values["backgroundAsset"].toString():QString{};
                if(Asset!=Self->backgroundAsset_) {
                    QString Error; const auto Data=Asset.isEmpty()?QByteArray{}:hc::epub::poc::ReadingAssets::Load(Self->settingsPath_,Asset,Error);
                    Self->command("background",QJsonObject{{"asset",Data.isEmpty()?QString{}:Asset},
                        {"data",Data.isEmpty()?QString{}:QString("data:image/png;base64,")+QString::fromLatin1(Data.toBase64())}});
                    Self->backgroundAsset_=Error.isEmpty()?Asset:QString{};
                    // The status bar may be hidden behind the host's; the snapshot carries the message.
                    if(!Error.isEmpty()) {Self->statusBar()->showMessage(Error);Self->readerMessage_=Error;}
                    else if(!Self->finished_) Self->readerMessage_.clear();
                }
                else if(Asset.isEmpty() && !Self->finished_) Self->readerMessage_.clear();
                Self->command("profile",Rendered);
                if(!Self->started_) {Self->started_=true;Self->command("start",Self->engine_);Self->poll_.start(33);}
            };
            if(Css.size()>16384 || Css.toUtf8().size()>32768) {Apply(tr("CSS 크기 한도를 초과했습니다."));return;}
            const auto Encoded=QJsonDocument(QJsonArray{Css}).toJson(QJsonDocument::Compact);
            const auto Literal=QString::fromUtf8(Encoded.mid(1,Encoded.size()-2));
            page_->runJavaScript("JSON.stringify(window.hcValidateCss ? window.hcValidateCss("+Literal+") : {ready:false})",
                [Guard,Apply](const QVariant& Value) {
                    if(!Guard)return;
                    const auto Result=QJsonDocument::fromJson(Value.toString().toUtf8()).object();
                    if(!Result["ready"].toBool())return;
                    Apply(Result["ok"].toBool()?QString{}:tr("%1행 %2열: %3").arg(Result["line"].toInt()).arg(Result["column"].toInt()).arg(Result["error"].toString()));
                });
        }
        void finish(QJsonObject Result)
        {
            if(finished_) return;
            if(evidence_.isEmpty()) {latest_=Result;if(Result["status"]=="error"){finished_=true;poll_.stop();}readerMessage_=Result["status"].toString() + ": " + Result["message"].toString();statusBar()->showMessage(readerMessage_); return; }
#ifndef HC_EPUB_PRODUCT_HELPER
            // A hidden panel retains its startup scrollbar range. Measure the
            // comparison window only after showing and laying out its settings.
            if(settingsScroll_->isHidden()) {
                poll_.stop();readAction("focus-settings",{});
                QTimer::singleShot(100,this,[this,Result]{finish(Result);});return;
            }
#endif
            finished_ = true; poll_.stop();
            if(!expectedBookFont_.isEmpty()) {
                const auto Final = Result["final"].toObject();
                const auto Paragraphs = Final["paragraphs"].toArray();
                const bool Selected = !Paragraphs.isEmpty() && Final["profile"].toObject()["font"] == expectedBookFont_
                    && Paragraphs.first().toObject()["font"].toString().contains(expectedBookFont_);
                Result["nativeBookFontSelection"] = Selected;
                if(!Selected && Result["status"] == "passed") { Result["status"] = "failed"; Result["message"] = "Native book font selection not applied"; }
            }
            Result["network"] = filter_->Snapshot(); Result["console"] = page_->messages;
            Result["qtVersion"] = qVersion(); Result["sandboxDisabledByTool"] = false;
            const bool Overflow = settingsScroll_->horizontalScrollBar()->maximum() > 0;
            Result["settingsHorizontalOverflow"] = Overflow;
            if(Overflow && Result["status"] == "passed") { Result["status"] = "failed"; Result["message"] = "Settings panel horizontal overflow"; }
            QFile Output(evidence_ + "/result.json");
            bool Written = Output.open(QIODevice::WriteOnly | QIODevice::NewOnly);
            if(Written) { const auto Data = QJsonDocument(Result).toJson(); Written = Output.write(Data) == Data.size(); Output.close(); }
            QTimer::singleShot(200, this, [this, Result, Written] {
                const bool Captured = grab().save(evidence_ + "/screen.png");
                settings_->findChild<QTabWidget*>()->setCurrentIndex(1);
                QTimer::singleShot(200, this, [this, Result, Written, Captured] {
                    const bool DetailCaptured = grab().save(evidence_ + "/screen-detail.png");
                    qApp->exit(Written && Captured && DetailCaptured && settingsScroll_->horizontalScrollBar()->maximum() == 0
                        && Result["status"] == "passed" ? 0 : 2);
                });
            });
        }
        QString evidence_, documentId_, expectedBookFont_, controlId_;
        qint64 annotationClickSeen_=0, controlGeneration_=0;
        qint64 generation_ = 1;
        QJsonObject latest_{{"status", "loading"}},sentState_;
        QElapsedTimer detailClock_,scrollActive_;bool detailPending_=true;qint64 scrollSerialSeen_=-1;int detailSnapshots_=0;
        QJsonObject hostAppearance_;
        hc::epub::poc::ReadingNavigation* navigation_=nullptr;
        QString settingsPath_,engine_,backgroundAsset_,astError_,resourceFailure_;
        quint64 profileSerial_=0;
        bool editingPreview_=false;
        qint64 settingsStamp_=0;
        bool shellReady_=false,started_=false,composing_=false,interactive_=true;
        int toolbarAvailabilityChanges_=0;
        int escapeRequests_=0;
        int fullscreenRequests_=0;
        hc::epub::poc::ReadingSettings* settings_ = nullptr;
        QWebEngineView* view_ = nullptr;
        QLabel* location_=nullptr;
        hc::document::DocumentShortcutRouter* shortcuts_=nullptr;
        QScrollArea* settingsScroll_ = nullptr;
        QScrollArea* navigationScroll_=nullptr;
        QSplitter* splitter_=nullptr;
        QComboBox* layoutChoice_=nullptr;
        QComboBox* columnsChoice_=nullptr;
        QWebEngineProfile* profile_ = nullptr;
        Page* page_ = nullptr;
        Filter* filter_ = nullptr;
        Assets previewAssets_;
        // accepted_: the owner's last revision of a chapter; validated_: the last that passed the chapter
        // checks; rendered_: the one the reader holds, whose text (renderedSource_) element patches start from.
        struct PreviewSource {QByteArray bytes_,renderedSource_;qint64 accepted_=-1,validated_=-1,rendered_=-1;};
        struct PreviewWork {hc::document::epub::ResourceResult checked_;QByteArray annotated_,sha256_;hc::document::epub::ElementPatch elements_;};
        QHash<QString,PreviewSource> previewSources_;
        QJsonObject previewResult_{{"serial",0},{"status","done"}},previewMessage_;
        QByteArray previewChecked_,previewAnnotated_;
        QJsonArray pendingBookmarks_;
        QTimer debounce_, poll_;
        bool pending_ = false, testing_ = false, finished_ = false;
        // Host chrome state: chapter list, last reader error and the latest body menu request.
        QJsonArray toc_;QString readerMessage_;
        bool contextMenus_=false;QJsonObject contextRequest_{{"serial",0}};qint64 contextSerial_=0,pageContextSeen_=0;
    };

    // The caller retains the stable native surface through retirement. Parsing runs
    // off the UI thread; a cancelled preparation can never install a late reader.
    class OwnerController final : public QObject
    {
    public:
        OwnerController(QString Bundle, QWindow* Parent) : bundle_(std::move(Bundle)), nativeParent_(Parent)
        { connect(&timer_, &QTimer::timeout, this, [this] { transfer_.Expire(); advanceReload(); }); timer_.start(100); }
        ~OwnerController() override { cancellation_->store(true); if(reload_.cancel_) reload_.cancel_->store(true); delete candidate_; delete reader_; }
        std::uintptr_t ReaderWindow() const { return nativeReader_; }
        void Refresh(QSize PhysicalSize)
        {
            installPrepared();
            if(!reader_ || !nativeReader_ || !PhysicalSize.isValid()) return;
            const auto Dpr = reader_->devicePixelRatioF();
            const QRect Geometry(0, 0, qRound(PhysicalSize.width() / Dpr), qRound(PhysicalSize.height() / Dpr));
            for(auto* Each : {reader_, candidate_}) {
                if(!Each) continue;
                if(Each->geometry() != Geometry) Each->setGeometry(Geometry);
                if(Each->windowHandle()->geometry() != Geometry) Each->windowHandle()->setGeometry(Geometry);
            }
        }
        // RequestId: the frame being answered. A request too large for one frame arrives as an envelope.
        QByteArray Control(const QByteArray& Bytes, std::uint64_t RequestId)
        {
            QJsonParseError Parse; const auto Document = QJsonDocument::fromJson(Bytes, &Parse);
            QJsonObject Reply;
            if(Parse.error != QJsonParseError::NoError || !Document.isObject()) Reply = {{"error", "epub-input-json"}};
            else if(Document.object()["kind"] == "payload") Reply = unwrap(Document.object(), RequestId);
            else Reply = handle(Document.object());
            return QJsonDocument(Reply).toJson(QJsonDocument::Compact);
        }
    private:
        struct Prepared { QJsonObject publication; Assets assets; QString error; };
        static bool hashText(const QString& Text)
        { const auto Bytes = Text.toLatin1(); return Text.size() == 64 && QString::fromLatin1(Bytes) == Text && QByteArray::fromHex(Bytes).toHex() == Bytes; }
        // The owner wrote the request to a section duplicated read-only into this process. The envelope is
        // bound to this frame's request id and to the open document; the request inside names the same
        // document, is a reader action and never another envelope. The handle is closed on every path once
        // the envelope is well formed.
        QJsonObject unwrap(const QJsonObject& Envelope, std::uint64_t RequestId)
        {
            const auto Length = Envelope["byteLength"].toInteger(-1);
            bool Valid = false; const auto RawHandle = Envelope["handle"].toString().toULongLong(&Valid);
            if(Envelope.size() != 8 || !transfer_.Matches(Envelope) || Envelope["requestId"].toInteger(-1) != qint64(RequestId)
                || Length <= qint64(hc::document::HostInlineLimit) || quint64(Length) > hc::document::HostEnvelopeLimit
                || !hashText(Envelope["sha256"].toString()) || !Valid || !RawHandle || RawHandle > quintptr(-1)
                || QString::number(RawHandle) != Envelope["handle"].toString()) return {{"error", "epub-payload-admission"}};
            QJsonObject Reply{{"protocolVersion", 1}, {"documentId", Envelope["documentId"]}, {"generation", Envelope["generation"]}};
            const auto Mapping = reinterpret_cast<HANDLE>(quintptr(RawHandle));
            const auto* View = MapViewOfFile(Mapping, FILE_MAP_READ, 0, 0, SIZE_T(Length));
            QByteArray Bytes;
            if(View) { try { Bytes = QByteArray(static_cast<const char*>(View), qsizetype(Length)); } catch(...) {} UnmapViewOfFile(View); }
            CloseHandle(Mapping);
            if(Bytes.size() != Length) { Reply["error"] = "epub-payload-mapping"; return Reply; }
            if(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex() != Envelope["sha256"].toString().toLatin1()) { Reply["error"] = "epub-payload-integrity"; return Reply; }
            QJsonParseError Parse; const auto Inner = QJsonDocument::fromJson(Bytes, &Parse).object();
            if(Parse.error != QJsonParseError::NoError || Inner["kind"] != "reader-action"
                || Inner["protocolVersion"] != QJsonValue(1) || Inner["documentId"] != Envelope["documentId"] || Inner["generation"] != Envelope["generation"])
            { Reply["error"] = "epub-payload-request"; return Reply; }
            return handle(Inner);
        }
        QJsonObject handle(const QJsonObject& Request)
        {
            const auto Kind = Request["kind"].toString();
#ifdef HC_EPUB_PRODUCT_HELPER
            if(Kind == "input-begin" && Request["engine"] != QJsonValue("epubjs"))
                return {{"error", "epub-product-engine"}};
#endif
            if(Kind == "configure") {
                const auto Path = Request["settingsPath"].toString();
                if(configured_ || preparing_ || reader_ || Request.size()<6 || Request.size()>10 || Request.size()!=6+Request.contains("initiallyInteractive")+Request.contains("sourceLines")+Request.contains("globalSettingsPath")+Request.contains("hostChrome")
                    || (Request.contains("initiallyInteractive") && !Request["initiallyInteractive"].isBool()) || (Request.contains("sourceLines") && !Request["sourceLines"].isBool())
                    || (Request.contains("hostChrome") && !Request["hostChrome"].isBool()) || !QFileInfo(Path).isAbsolute()
                    || (Request.contains("globalSettingsPath") && (!QFileInfo(Request["globalSettingsPath"].toString()).isAbsolute() || !Request["globalSettingsPath"].toString().endsWith("/epub-reading.json")))
                    || !Path.endsWith("/epub-reading.json") || !hc::document::epubui::ApplyAppearance(Request["appearance"].toObject()))
                    return {{"error", "epub-configuration-invalid"}};
                globalSettingsPath_=Request["globalSettingsPath"].toString();settingsPath_ = Path; appearance_ = Request["appearance"].toObject(); initiallyInteractive_=Request["initiallyInteractive"].toBool(true); sourceLines_=Request["sourceLines"].toBool(false); hostChrome_=Request["hostChrome"].toBool(false); configured_ = true;
                return {{"accepted", true}, {"epubInputVersion",2}};
            }
            if(Kind.startsWith("input-")) {
                if(Kind == "input-cancel" && preparing_ && Request.size() == 4 && transfer_.Matches(Request)) {
                    cancelled_ = true;
                    cancellation_->store(true);
                    return {{"accepted", true}, {"status", "cancelled"}};
                }
                auto Reply = transfer_.Handle(Request);
                if((Kind == "input-commit" || Kind=="input-map") && Reply["accepted"].toBool()) {
                    auto Input = transfer_.Take();
                    if(!Input) return {{"error", "epub-input-state"}};
                    // Every later reader of this helper answers to the identity of this input.
                    controlId_ = Input->documentId; controlGeneration_ = Input->generation;
                    preparing_ = true;
                    auto* Watcher = new QFutureWatcher<Prepared>(this);
                    connect(Watcher, &QFutureWatcher<Prepared>::finished, this,
                        [this, Watcher, Engine = Input->engine, Generation = Input->generation] {
                        const auto Result = Watcher->result(); Watcher->deleteLater(); preparing_ = false;
                        if(cancelled_) return;
                        if(!Result.error.isEmpty()) { error_ = Result.error; return; }
                        prepared_=Result;preparedEngine_=Engine;preparedGeneration_=Generation;preparing_=true;installPrepared();
                    });
                    Watcher->setFuture(QtConcurrent::run([Input = std::move(*Input), Bundle = bundle_, Cancellation = cancellation_]() mutable {
                        Prepared Result;
                        try {
                            if(Input.inputVersion==2) {
                                const auto Session=hc::document::epub::PublicationSession::Open(std::move(Input.bytes),[Cancellation]{return Cancellation->load();});
                                const auto Index=Session->Index();
                                if(!Index) throw Index.error_+": "+Index.location_;
                                Result.publication=Index.publication_;
                                if(Result.publication["sourceSha256"]!=Input.documentId) throw QString("epub-publication-identity");
                                Result.assets=loadAssets(Result.publication,Bundle,Session);
                                return Result;
                            }
                            QJsonParseError Parse; const auto Doc = QJsonDocument::fromJson(Input.bytes, &Parse);
                            if(Parse.error != QJsonParseError::NoError || !Doc.isObject()) throw QString("epub-publication-json");
                            Result.publication = Doc.object();
                            if(Result.publication["sourceSha256"] != Input.documentId) throw QString("epub-publication-identity");
                            Result.assets = loadAssets(Result.publication, Bundle);
                        } catch(const QString& Error) { Result.error = Error; }
                        return Result;
                    }));
                    Reply["status"] = "preparing";
                }
                return Reply;
            }
            if(!transfer_.Matches(Request)) return {{"error", "epub-control-identity"}};
            if(Kind == "reload") return reload(Request);
            if(reader_) {
                auto Reply = reader_->Control(Request);
                const bool Accepted = Reply["accepted"].toBool();
                // A reload replays the editing preview's reader settings into the new reader.
                if(Accepted && Kind == "appearance") appearance_ = Request["appearance"].toObject();
                if(Accepted && Kind == "reader-action" && QStringList{"bindings", "annotations-transient", "context-menu", "preview-width", "enable-input"}.contains(Request["action"].toString()))
                    sticky_[Request["action"].toString()] = Request["value"];
                if(Kind == "snapshot" || Kind == "snapshot-delta") Reply["reload"] = QJsonObject{{"serial", reload_.serial_}, {"status", reload_.status_}, {"error", reload_.error_}};
                return Reply;
            }
            if(Kind != "snapshot" && Kind != "snapshot-delta") return {{"error", "epub-control-not-ready"}};
            auto State = transfer_.Status();
            if(cancelled_) State["status"] = "cancelled";
            else if(!error_.isEmpty()) { State["status"] = "error"; State["message"] = error_; }
            else if(preparing_) State["status"] = "preparing";
            return {{"protocolVersion", 1}, {"documentId", Request["documentId"]}, {"generation", Request["generation"]}, {"state", State}};
        }
        // Reload of an editing preview after a change outside the shown chapter (package, styles, images,
        // another chapter). The new publication opens in a second reader below the shown one, inside this
        // helper. It takes over only after the reading settings, the chapter and the source position are
        // restored; a failure keeps the shown reader and is reported as a refresh error.
        QJsonObject reload(const QJsonObject& Request)
        {
            QJsonObject Reply{{"protocolVersion", 1}, {"documentId", Request["documentId"]}, {"generation", Request["generation"]}};
            const auto Serial = Request["reloadSerial"].toInteger(-1), Length = Request["byteLength"].toInteger(-1), Chapter = Request["chapter"].toInteger(-2);
            bool Valid = false; const auto RawHandle = Request["handle"].toString().toULongLong(&Valid);
            const auto Scroll = Request["scroll"].toObject();
            if(Request.size() != 11 || Serial <= reload_.serial_ || Serial > 9007199254740991LL || Length < 1 || Length > hc::document::epub::InputLimit
                || !hashText(Request["sha256"].toString()) || !Valid || !RawHandle || RawHandle > quintptr(-1) || QString::number(RawHandle) != Request["handle"].toString()
                || Chapter < -1 || Chapter > 65535 || !Request["locator"].isString() || Request["locator"].toString().size() > 4096
                || !Request["scroll"].isObject() || (!Scroll.isEmpty() && !hc::document::epub::ValidSourceScrollRequest(Scroll)))
            { Reply["error"] = "epub-reload-admission"; return Reply; }
            const auto Mapping = reinterpret_cast<HANDLE>(quintptr(RawHandle));
            if(!reader_ || !sourceLines_) { CloseHandle(Mapping); Reply["error"] = "epub-reload-state"; return Reply; }
            discardCandidate();
            reload_ = {}; reload_.serial_ = Serial; reload_.status_ = "preparing"; reload_.cancel_ = std::make_shared<std::atomic_bool>(false);
            reload_.chapter_ = int(Chapter); reload_.locator_ = Request["locator"].toString(); reload_.scroll_ = Scroll;
            auto* Watcher = new QFutureWatcher<Prepared>(this);
            connect(Watcher, &QFutureWatcher<Prepared>::finished, this, [this, Watcher, Serial] {
                auto Result = Watcher->result(); Watcher->deleteLater();
                if(Serial != reload_.serial_ || reload_.status_ != "preparing") return;
                if(!Result.error.isEmpty()) { failReload(Result.error); return; }
                reload_.prepared_ = std::move(Result); reload_.status_ = "loading"; advanceReload();
            });
            Watcher->setFuture(QtConcurrent::run([Mapping, Length, Sha = Request["sha256"].toString().toLatin1(), Bundle = bundle_, Cancel = reload_.cancel_] {
                Prepared Result; QByteArray Bytes;
                if(const auto* View = MapViewOfFile(Mapping, FILE_MAP_READ, 0, 0, SIZE_T(Length))) {
                    try { Bytes = QByteArray(static_cast<const char*>(View), qsizetype(Length)); } catch(...) {}
                    UnmapViewOfFile(View);
                }
                CloseHandle(Mapping);
                try {
                    if(Bytes.size() != Length) throw QString("epub-reload-mapping");
                    if(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex() != Sha) throw QString("epub-reload-integrity");
                    const auto Session = hc::document::epub::PublicationSession::Open(std::move(Bytes), [Cancel] { return Cancel->load(); });
                    const auto Index = Session->Index();
                    if(!Index) throw Index.error_ + ": " + Index.location_;
                    Result.publication = Index.publication_;
                    if(Result.publication["sourceSha256"].toString().toLatin1() != Sha) throw QString("epub-publication-identity");
                    Result.assets = loadAssets(Result.publication, Bundle, Session);
                } catch(const QString& Error) { Result.error = Error; }
                catch(const std::bad_alloc&) { Result.error = "memory-limit"; }
                return Result;
            }));
            Reply["accepted"] = true; Reply["reloadSerial"] = Serial; return Reply;
        }
        QJsonObject candidateRequest(QJsonObject Request) const
        {
            Request["protocolVersion"] = 1; Request["documentId"] = controlId_; Request["generation"] = controlGeneration_;
            return candidate_->Control(Request);
        }
        bool candidateAction(const QString& Action, const QJsonValue& Value) const
        { return candidateRequest({{"kind", "reader-action"}, {"action", Action}, {"value", Value}})["accepted"].toBool(); }
        void advanceReload()
        {
            if(reload_.status_ != "loading" || !reader_) return;
            if(!candidate_) {
                // As for the first reader: a background owner gets no new native window, which could activate.
                const auto Parent = reinterpret_cast<HWND>(nativeParent_->winId());
                if(!IsWindowVisible(Parent) || GetAncestor(GetForegroundWindow(), GA_ROOT) != GetAncestor(Parent, GA_ROOT)) return;
                auto Result = std::move(*reload_.prepared_); reload_.prepared_.reset();
                candidate_ = new Window(Result.assets, Result.publication, "epubjs", "light", {}, settingsPath_, preparedGeneration_, false, sourceLines_, globalSettingsPath_, hostChrome_);
                candidate_->SetControlIdentity(controlId_, controlGeneration_);
                if(!appearance_.isEmpty()) candidate_->Appearance(appearance_);
                candidate_->setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
                candidate_->winId(); candidate_->windowHandle()->setParent(nativeParent_); candidate_->setAttribute(Qt::WA_ShowWithoutActivating);
                candidate_->setGeometry(reader_->geometry()); candidate_->windowHandle()->setGeometry(reader_->windowHandle()->geometry());
                candidate_->show();
                // Below the shown reader: it lays out and paints unseen until it takes over.
                SetWindowPos(reinterpret_cast<HWND>(candidate_->winId()), reinterpret_cast<HWND>(reader_->winId()), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                reload_.clock_.start(); return;
            }
            if(reload_.clock_.elapsed() > 30000) { failReload("epub-reload-timeout"); return; }
            const auto State = candidateRequest({{"kind", "snapshot"}})["state"].toObject();
            if(State["status"] == "error") { failReload(State["message"].toString("epub-reload-render")); return; }
            if(State["status"] != "ready" || State["readerBusy"].toBool() || State["settingsBusy"].toBool() || !State["readingStateLoaded"].toBool()) return;
            // Input is enabled here, below the shown reader: changing a visible window's focus flag can
            // lay out and display the frame again, which must happen before the position is restored.
            if(reload_.stage_ == 0) {
                for(const auto* Name : {"bindings", "annotations-transient", "context-menu", "preview-width", "enable-input"})
                    if(sticky_.contains(QLatin1String(Name))) candidateAction(QLatin1String(Name), sticky_[QLatin1String(Name)]);
                reload_.stage_ = 1; return;
            }
            if(reload_.stage_ == 1) {
                if(sticky_.value("annotations-transient").toBool() && !State["annotationsTransient"].toBool()) return;
                if(sticky_.contains("enable-input") && State["inputEnabled"].toBool() != sticky_.value("enable-input").toBool()) return;
                // The frame those settings may have displayed again stays the same for a moment first.
                const auto Frame = State["frameId"].toString();
                if(Frame.isEmpty() || Frame != reload_.frame_) { reload_.frame_ = Frame; reload_.frameClock_.start(); return; }
                if(reload_.frameClock_.elapsed() < 600) return;
                // The owner names the chapter of the source it edits; otherwise the shown reader's position is kept.
                const int Old = reader_->Control(QJsonObject{{"protocolVersion", 1}, {"documentId", controlId_}, {"generation", controlGeneration_}, {"kind", "snapshot"}})["state"].toObject()["chapterIndex"].toInt(-1);
                const auto Chapter = reload_.chapter_; const auto& Locator = reload_.locator_;
                const bool Located = Chapter >= 0 && (Chapter != Old || (Locator.isEmpty() && Chapter != State["chapterIndex"].toInt(-1)))
                    ? candidateAction("chapter", Chapter) : !Locator.isEmpty() ? candidateAction("preview-location", Locator) : false;
                reload_.action_ = Located ? State["readerAction"].toObject()["serial"].toInteger() + 1 : -1; reload_.stage_ = 2; return;
            }
            if(reload_.stage_ == 2) {
                if(reload_.action_ >= 0 && State["readerAction"].toObject()["serial"].toInteger() < reload_.action_) return;
                reload_.stage_ = 4;
                if(!reload_.scroll_.isEmpty() && State["chapterIndex"].toInt(-1) == reload_.chapter_ && State["previewScroll"].toObject()["supported"].toBool()
                    && candidateRequest({{"kind", "source-scroll"}, {"value", reload_.scroll_}})["accepted"].toBool()) { reload_.stage_ = 3; reload_.scrollClock_.start(); return; }
            }
            if(reload_.stage_ == 3) {
                // Never show a frame whose source position has not been restored.
                if(State["previewScroll"].toObject()["applied"].toInteger() < reload_.scroll_["serial"].toInteger()) {
                    if(reload_.scrollClock_.elapsed() > 5000) failReload("epub-preview-scroll-timeout");
                    return;
                }
            }
            swapReader();
        }
        void swapReader()
        {
            auto* Old = std::exchange(reader_, candidate_); candidate_ = nullptr;
            reader_->InheritCounters(*Old);
            // The input queue is shared with the owner: GetFocus names the owner's window while its editor
            // has the keyboard. Only a reader that holds it passes it on.
            const auto OldWindow = reinterpret_cast<HWND>(Old->winId()), Focus = GetFocus();
            const bool Focused = Focus && (Focus == OldWindow || IsChild(OldWindow, Focus));
            SetWindowPos(reinterpret_cast<HWND>(reader_->winId()), HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            nativeReader_ = static_cast<std::uintptr_t>(reader_->winId());
            if(Focused) candidateRequestTo(reader_, {{"kind", "focus"}});
            Old->hide(); Old->deleteLater();
            reload_.status_ = "ready"; reload_.cancel_.reset();
        }
        QJsonObject candidateRequestTo(Window* Target, QJsonObject Request) const
        {
            Request["protocolVersion"] = 1; Request["documentId"] = controlId_; Request["generation"] = controlGeneration_;
            return Target->Control(Request);
        }
        void failReload(const QString& Error) { discardCandidate(); reload_.status_ = "failed"; reload_.error_ = Error; }
        void discardCandidate()
        {
            if(reload_.cancel_) reload_.cancel_->store(true);
            reload_.prepared_.reset();
            if(candidate_) { candidate_->hide(); candidate_->deleteLater(); candidate_ = nullptr; }
        }
        struct Reload {
            qint64 serial_ = 0, action_ = -1; int chapter_ = -1, stage_ = 0;
            QString status_ = "idle", error_, locator_, frame_; QJsonObject scroll_;
            std::shared_ptr<std::atomic_bool> cancel_; std::optional<Prepared> prepared_;
            QElapsedTimer clock_, scrollClock_, frameClock_;
        } reload_;
        Window* candidate_ = nullptr;
        QJsonObject sticky_;
        QString controlId_; qint64 controlGeneration_ = 0;
        void installPrepared() {
            if(!prepared_ || cancelled_ || reader_)return;
            const auto Parent=reinterpret_cast<HWND>(nativeParent_->winId());
            // A passive preview may finish parsing after the user has switched
            // applications. Keep it prepared until its owner is foreground;
            // creating/reparenting a foreign Qt HWND can itself activate it.
            if(!initiallyInteractive_ && (!IsWindowVisible(Parent)
                || GetAncestor(GetForegroundWindow(),GA_ROOT)!=GetAncestor(Parent,GA_ROOT)))return;
            auto Result=std::move(*prepared_);prepared_.reset();
            reader_ = new Window(Result.assets,Result.publication,preparedEngine_,"light",{},settingsPath_,preparedGeneration_,initiallyInteractive_,sourceLines_,globalSettingsPath_,hostChrome_);
            if(!appearance_.isEmpty())reader_->Appearance(appearance_);
            reader_->setWindowFlags(Qt::Window | Qt::FramelessWindowHint | (initiallyInteractive_?Qt::WindowFlags{}:Qt::WindowFlags(Qt::WindowDoesNotAcceptFocus)));
            reader_->winId();reader_->windowHandle()->setParent(nativeParent_);reader_->setAttribute(Qt::WA_ShowWithoutActivating);
            nativeReader_=static_cast<std::uintptr_t>(reader_->winId());reader_->show();preparing_=false;
        }
        std::optional<Prepared> prepared_;QString preparedEngine_;qint64 preparedGeneration_=1;
        QString bundle_, error_, settingsPath_, globalSettingsPath_;
        QJsonObject appearance_;
        bool configured_ = false;
        std::shared_ptr<std::atomic_bool> cancellation_=std::make_shared<std::atomic_bool>(false);
        hc::epub::poc::InputTransfer transfer_;
        QWindow* nativeParent_ = nullptr;
        std::uintptr_t nativeReader_ = 0;
        Window* reader_ = nullptr;
        QTimer timer_;
        bool preparing_ = false, cancelled_ = false,initiallyInteractive_=true,sourceLines_=false,hostChrome_=false;
    };
}

int main(int Argc, char** Argv)
{
    hc::fonts::StartupFontWarmup FontWarmup(Argc, Argv);
    hc::epub::poc::OwnerMode Owner;
    try {
        if(!qEnvironmentVariableIsEmpty("QTWEBENGINE_DISABLE_SANDBOX")
            || qEnvironmentVariable("QTWEBENGINE_CHROMIUM_FLAGS").contains("--no-sandbox")) throw QString("Sandbox bypass is not allowed");
        Owner = hc::epub::poc::ParseOwnerMode();
#ifdef HC_EPUB_PRODUCT_HELPER
        // The product executable has no standalone file or comparison-engine entry.
        if(!Owner.enabled) throw QStringLiteral("Authenticated EPUB owner is required");
#endif
    }
    catch(const QString& Error) { qCritical().noquote() << Error; return 2; }
    QWebEngineUrlScheme Scheme("epub");
    Scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    Scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::CorsEnabled | QWebEngineUrlScheme::FetchApiAllowed);
    QWebEngineUrlScheme::registerScheme(Scheme);
    // Match V8 collection pressure to this helper's bounded job instead of a
    // multi-gigabyte desktop-browser heap. Publisher scripts remain disabled.
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--js-flags=--max-old-space-size=64");
    QApplication App(Argc, Argv);
    FontWarmup.WatchFirstWindow();
    // On only when the owner passed HC_UI_STALL_TRACE_CHILD=1; writes under the helper's TEMP.
    hc::document::DocumentStallTrace::InstallHelper(QStringLiteral("epub"));
    hc::document::DocumentThemeAdapter::Install(App);
    QCommandLineParser Parser; Parser.addHelpOption();
    Parser.addOptions({{{"p", "publication"}, "Prepared P0-B JSON", "path"}, {{"b", "bundle"}, "Verified developer bundle", "directory"},
        {{"e", "engine"}, "epubjs or readium", "name", "epubjs"}, {{"t", "theme"}, "light or dark", "name", "light"},
        {{"o", "evidence"}, "New self-test evidence directory (session-only settings)", "directory"},
        {"settings-file", "Developer reading preferences JSON (separate from product settings)", "path"},
        {"epub", "Original EPUB file (native bounded package preparation)", "path"},
        {"web-owner", "Authenticated development owner mode"}, {"pipe", "Owner pipe", "name"},
        {"connection", "Connection nonce", "hex"}, {"parent-process", "Owner PID", "id"}, {"parent", "Owner HWND", "id"}});
    Parser.process(App);
    try
    {
        if(Parser.value("engine") != "epubjs" && Parser.value("engine") != "readium") throw QString("Invalid engine");
        hc::document::DocumentWebPeer Peer;
        if(Owner.enabled && !Peer.Connect(Owner.pipe, Owner.connection, Owner.processId, Owner.parentWindow)) throw QString("Owner authentication failed");
        const auto Root = QCoreApplication::applicationDirPath();
        if(Owner.enabled) {
            App.setQuitOnLastWindowClosed(false);
            hc::epub::poc::OwnerSurface Surface;
            if(!Surface.Create(Owner)) throw QString("Owner attachment surface unavailable");
            std::unique_ptr<QWindow> NativeParent(QWindow::fromWinId(static_cast<WId>(Surface.Window())));
            if(!NativeParent) throw QString("Owner native parent unavailable");
            // Construct the complete reader only after transfer. Binding an empty
            // Qt widget first lets later WebEngine creation reset its native parent.
            #ifdef HC_EPUB_PRODUCT_HELPER
            OwnerController Controller(Root + "/Libs/epub-bundle", NativeParent.get());
#else
            OwnerController Controller(Root + "/bundle", NativeParent.get());
#endif
            if(!Peer.Ready(Surface.Window(), [&Controller, &Peer](const QByteArray& Request) { return Controller.Control(Request, Peer.CurrentRequest()); }, [] { qApp->quit(); }))
                throw QString("Owner native attachment failed");
            const auto FitParent = [&Surface, &Controller] {
                if(!Surface.Refresh(Controller.ReaderWindow())) qApp->quit();
                else Controller.Refresh(Surface.Size());
            };
            // The owner posts a wake after each resize of the parent; the poll is the fallback.
            Surface.SetResizeHandler(FitParent);
            QTimer ParentTimer;
            QObject::connect(&ParentTimer, &QTimer::timeout, &Controller, FitParent); ParentTimer.start(100);
            return App.exec();
        }
#ifndef HC_EPUB_PRODUCT_HELPER
        if(Parser.isSet("epub") == Parser.isSet("publication")) throw QString("Specify exactly one of --epub or --publication");
        QJsonObject Publication;
        std::shared_ptr<hc::document::epub::PublicationSession> Session;
        if(Parser.isSet("epub")) {
            Session=hc::document::epub::PublicationSession::Open(readBounded(Parser.value("epub"),hc::document::epub::InputLimit));
            const auto Prepared = Session->Index();
            if(!Prepared) throw Prepared.error_ + ": " + Prepared.location_;
            Publication = Prepared.publication_;
        } else Publication = readObject(Parser.value("publication"), MaxSnapshot);
        const auto Items = loadAssets(Publication, Parser.value("bundle"),Session);
        const auto Evidence = Parser.value("evidence");
        if(!Evidence.isEmpty() && (QDir(Evidence).exists() || !QDir().mkpath(Evidence))) throw QString("Evidence directory must be new");
        if(!Evidence.isEmpty() && Parser.isSet("settings-file")) throw QString("Evidence mode must not write persistent settings");
        const auto SettingsPath = Evidence.isEmpty() && !Owner.enabled ? (Parser.isSet("settings-file") ? Parser.value("settings-file")
            : hc::defaults::edit::EpubStandaloneSettingsFile()) : QString();
        Window Main(Items, Publication, Parser.value("engine"), Parser.value("theme"), Evidence, SettingsPath);
        if(!Evidence.isEmpty()) Main.setAttribute(Qt::WA_DontShowOnScreen);
        Main.show();
        return App.exec();
#else
        throw QStringLiteral("Authenticated EPUB owner is required");
#endif
    }
    catch(const QString& Error) { qCritical().noquote() << Error; return 2; }
}
