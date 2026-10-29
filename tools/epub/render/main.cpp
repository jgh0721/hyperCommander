#include "stdafx.h"
#include "readingSettings.hpp"
#include "readingNavigation.hpp"
#include "commonLibs/document/epub/epubAppearance.hpp"
#include "ownerMode.hpp"
#include "inputTransfer.hpp"
#include "publicationResources.hpp"
#include "commonLibs/document/epub/epubPackage.hpp"
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
#include <QFormLayout>
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
        explicit Handler(const Assets& Items, QObject* Parent, std::function<void(const QString&)> Failure)
            : QWebEngineUrlSchemeHandler(Parent), items_(Items), failure_(std::move(Failure))
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
            Watcher->setFuture(QtConcurrent::run(Store::Pool(),[Session=Item.store->Session(),Resource,Lease] { return Store::Load(Session,Resource,Lease); }));
        }
    private:
        Assets items_;
        std::function<void(const QString&)> failure_;
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
        Window(const Assets& Items, const QJsonObject& Publication, const QString& Engine, const QString& Theme, const QString& Evidence, const QString& SettingsPath, qint64 Generation = 1,bool InitiallyInteractive=true)
            : QMainWindow(nullptr,InitiallyInteractive?Qt::WindowFlags{}:Qt::WindowFlags(Qt::WindowDoesNotAcceptFocus)), evidence_(Evidence), documentId_(Publication["sourceSha256"].toString()), generation_(Generation)
        {
            setAttribute(Qt::WA_ShowWithoutActivating);engine_=Engine;interactive_=InitiallyInteractive;
#ifdef HC_EPUB_PRODUCT_HELPER
            setWindowTitle(tr("EPUB 읽기"));
#else
            setWindowTitle(tr("EPUB · Watercolor 읽기 설정 · P0 엔진 비교"));
#endif
            resize(QSize(1280,900).boundedTo(QGuiApplication::primaryScreen()->availableGeometry().size()-QSize(40,60)));
            settingsPath_ = SettingsPath;
            auto* Central = new QWidget(this);
            setCentralWidget(Central);
            auto* Layout = new QVBoxLayout(Central);
            Layout->setContentsMargins(0,0,0,0); Layout->setSpacing(0);
            auto* Splitter = new QSplitter(Central);splitter_=Splitter; Layout->addWidget(Splitter, 1);
            auto* Scroll = new QScrollArea(Splitter); navigationScroll_=Scroll; Scroll->setWidgetResizable(true); Scroll->setMinimumWidth(220);
            auto* Panel = new QWidget; Scroll->setWidget(Panel);
            auto* Form = new QFormLayout(Panel);
            Form->setRowWrapPolicy(QFormLayout::WrapLongRows);
            Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            auto* EngineBox = new QComboBox(Panel); EngineBox->addItems({"EPUB.js", "Readium Web"});
            EngineBox->setCurrentIndex(Engine == "readium" ? 1 : 0); Form->addRow(tr("엔진"), EngineBox);
#ifdef HC_EPUB_PRODUCT_HELPER
            Form->setRowVisible(EngineBox,false); EngineBox->setEnabled(false);
#endif
            settings_ = new hc::epub::poc::ReadingSettings(SettingsPath, Publication["sourceSha256"].toString(), Theme, Panel);
            navigation_=new hc::epub::poc::ReadingNavigation(SettingsPath,documentId_,[this](const QJsonObject& Request){command("read",Request);},Panel);
            navigation_->SetPublication(Publication);
            auto* ReadingBar=addToolBar(tr("독서"));ReadingBar->setObjectName("epubReadingToolbar");ReadingBar->setMovable(false);
            for(const auto& Entry:QList<QPair<QString,QString>>{{"back",tr("이전 위치")},{"previous",tr("이전")},{"next",tr("다음")}}) {
                auto* Action=ReadingBar->addAction(Entry.second);Action->setObjectName("epub."+Entry.first);
                if(Entry.first=="previous")Action->setToolTip(tr("이전 화면 · PgUp / 숫자 키패드 PgUp"));
                if(Entry.first=="next")Action->setToolTip(tr("다음 화면 · PgDown / 숫자 키패드 PgDown"));
                connect(Action,&QAction::triggered,this,[this,Name=Entry.first]{navigation_->Execute(Name);view_->setFocus(Qt::OtherFocusReason);});
            }
            location_=new QLabel(this);location_->setTextFormat(Qt::PlainText);location_->setObjectName("epubReadingLocation");location_->setAccessibleName(tr("현재 읽기 위치"));location_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);location_->setMinimumWidth(0);statusBar()->addPermanentWidget(location_,1);
            Form->addRow(navigation_);
            settingsScroll_=new QScrollArea(Splitter);settingsScroll_->setObjectName("epubQuickSettings");settingsScroll_->setWidgetResizable(true);settingsScroll_->setMinimumWidth(320);settingsScroll_->setWidget(settings_);
            view_ = new QWebEngineView(Splitter);view_->setFocusPolicy(InitiallyInteractive?Qt::StrongFocus:Qt::NoFocus); Splitter->setSizes({260,360,880});
            navigationScroll_->hide();settingsScroll_->hide();
            for(const auto& Entry:QList<QPair<QString,QString>>{{"focus-toc",tr("목차")},{"focus-search",tr("검색")},{"focus-bookmarks",tr("책갈피")},{"focus-notes",tr("형광펜·메모")},{"focus-settings",tr("Aa · 읽기 설정")}}) {
                auto* Action=ReadingBar->addAction(Entry.second);Action->setObjectName("epub."+Entry.first);
                Action->setCheckable(true);
                connect(Action,&QAction::triggered,this,[this,Name=Entry.first]{readAction("toggle-"+Name.mid(6),{});});
            }
            layoutChoice_=new QComboBox(ReadingBar);layoutChoice_->setAccessibleName(tr("읽기 방식"));layoutChoice_->addItems({tr("세로 스크롤"),tr("가로 페이지")});ReadingBar->addWidget(layoutChoice_);
            connect(layoutChoice_,&QComboBox::currentIndexChanged,this,[this](int Index){
                const bool Dirty=settings_->IsDirty();
                if(settings_->Execute("preview",{{"layout",Index==1?"paginated":"scroll"}},false)) {
                    if(!Dirty && settings_->CssValid())settings_->Execute("apply",{},false);else readAction("focus-settings",{});
                }
                focusReading();
            });
            columnsChoice_=new QComboBox(ReadingBar);columnsChoice_->setObjectName("epubColumns");columnsChoice_->setAccessibleName(tr("페이지 단 수"));
            columnsChoice_->addItems({tr("자동 단 수"),tr("1단"),tr("2단")});ReadingBar->addWidget(columnsChoice_);
            columnsChoice_->setToolTip(tr("가로 페이지에서 단 수를 선택합니다. 화면이 좁으면 1단으로 표시합니다."));
            connect(columnsChoice_,&QComboBox::currentIndexChanged,this,[this](int Index){
                const bool Dirty=settings_->IsDirty();
                if(settings_->Execute("preview",{{"columns",Index}},false)) {
                    if(!Dirty && settings_->CssValid())settings_->Execute("apply",{},false);else readAction("focus-settings",{});
                }
                focusReading();
            });
            profile_ = new QWebEngineProfile(this); profile_->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);profile_->setHttpCacheMaximumSize(32*1024*1024);
            profile_->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
            filter_ = new Filter(Items, profile_); profile_->setUrlRequestInterceptor(filter_);
            profile_->installUrlSchemeHandler("epub", new Handler(Items, profile_,[this](const QString& Error){resourceFailure_=Error;}));
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
            });
            connect(&poll_, &QTimer::timeout, this, [this] {
                if(pending_) return;
                pending_ = true;
                const bool Detailed=detailPending_ || !detailClock_.isValid() || detailClock_.elapsed()>=500;
                if(Detailed){detailPending_=false;detailClock_.restart();}
                const auto Snapshot=Detailed?QStringLiteral("window.hcSnapshot"):QStringLiteral("window.hcNativeSnapshot || window.hcSnapshot");
                page_->runJavaScript("JSON.stringify({...(("+Snapshot+")?.() || window.hcEvidence), startup: window.hcStartupDiagnostics ? window.hcStartupDiagnostics() : {}})", [this,Detailed](const QVariant& Value) {
                    pending_ = false; if(finished_) return;
                    auto Result = QJsonDocument::fromJson(Value.toString().toUtf8()).object();
                    if(!resourceFailure_.isEmpty()) { Result["status"]="error"; Result["message"]=resourceFailure_; }
                    if(Result["frameId"]!=latest_["frameId"] && !Detailed)detailPending_=true;
                    if(!Detailed){auto Merged=latest_;for(auto At=Result.begin();At!=Result.end();++At)Merged[At.key()]=At.value();Result=std::move(Merged);}
                    latest_ = Result;
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
                            .arg((qMax(1,Result["chapterPage"].toInt(1))+qMax(1,Result["columnCount"].toInt(1))-1)/qMax(1,Result["columnCount"].toInt(1))).arg((qMax(1,Result["chapterPages"].toInt(1))+qMax(1,Result["columnCount"].toInt(1))-1)/qMax(1,Result["columnCount"].toInt(1))).arg(qRound(Result["progress"].toDouble()*100)));
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
                    poll_.setInterval(Status=="ready" && !Result["readerBusy"].toBool()?250:33);
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
            auto* PanelKey=new QShortcut(QKeySequence("Ctrl+Shift+R"),this);connect(PanelKey,&QShortcut::activated,this,[this]{if(!composing_){if(navigationScroll_->isVisible()){navigationScroll_->hide();focusReading();}else readAction("focus-toc",{});}});
            qApp->installEventFilter(this);
            view_->setUrl(QUrl("epub://app/app/index.html"));
        }
        bool Appearance(const QJsonObject& Value) {
            if(!hc::document::epubui::ApplyAppearance(Value))return false;
            hostAppearance_=Value;const auto Palette=fm::style::ThemeManager::instance().colors().toPalette();QApplication::setPalette(Palette);setPalette(Palette);sendProfile();return true;
        }
        ~Window() override { finished_ = true; poll_.stop(); delete view_; view_ = nullptr; delete profile_; profile_ = nullptr; }
        QByteArray Control(const QByteArray& Bytes)
        {
            QJsonParseError Parse;
            const auto Request = QJsonDocument::fromJson(Bytes, &Parse).object();
            QJsonObject Reply{{"protocolVersion", 1}, {"documentId", documentId_}, {"generation", generation_}};
            const auto Fail = [&](const QString& Error) { Reply["error"] = Error; return QJsonDocument(Reply).toJson(QJsonDocument::Compact); };
            if(Parse.error != QJsonParseError::NoError || Request["protocolVersion"] != QJsonValue(1)
                || Request["documentId"] != documentId_ || Request["generation"] != QJsonValue(generation_)) return Fail("epub-control-identity");
            const auto Kind = Request["kind"].toString();
            if(Kind == "snapshot" || Kind == "snapshot-delta") {
                auto State = latest_;const auto Navigation=navigation_->Snapshot();for(auto I=Navigation.begin();I!=Navigation.end();++I)State[I.key()]=I.value(); State["inputFocused"] = latest_["readerFocused"].toBool();
                State["escapeRequests"]=escapeRequests_;State["fullscreenRequests"]=fullscreenRequests_;State["appearance"] = hostAppearance_; State["settingsPersistent"] = !settingsPath_.isEmpty();State["inputEnabled"]=interactive_;
                State["chromeWindowColor"]=palette().color(QPalette::Window).name();State["toolbarWindowColor"]=findChild<QToolBar*>("epubReadingToolbar")->palette().color(QPalette::Window).name();State["openPanel"]=openPanel();State["toolbarPreviousEnabled"]=findChild<QAction*>("epub.previous")->isEnabled();State["toolbarNextEnabled"]=findChild<QAction*>("epub.next")->isEnabled();State["toolbarAvailabilityChanges"]=toolbarAvailabilityChanges_;
                State["toolbarColumns"]=columnsChoice_->currentIndex();State["columnsControlEnabled"]=columnsChoice_->isEnabled();
                State["settingsBusy"] = settings_->IsBusy(); State["settingsDirty"] = settings_->IsDirty();
                State["readingProfile"] = settings_->Profile(); State["cssValid"]=settings_->CssValid(); State["cssValidationError"]=settings_->CssError();
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
            else return Fail("epub-control-command");
            Reply["accepted"] = true;
            return QJsonDocument(Reply).toJson(QJsonDocument::Compact);
        }
    protected:
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
        bool readAction(const QString& Action,const QJsonValue& Value) {
            if(Action=="enable-input"){if(!Value.isBool() || !windowHandle())return false;interactive_=Value.toBool();view_->setFocusPolicy(interactive_?Qt::StrongFocus:Qt::NoFocus);windowHandle()->setFlag(Qt::WindowDoesNotAcceptFocus,!interactive_);return true;}
            if(Action=="fullscreen"){++fullscreenRequests_;return true;}
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
                if(navigation_->isAncestorOf(QApplication::focusWidget()))return readAction("focus-settings",{});
                focusReading();return true;
            }
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
            if(finished_)return;detailPending_=true;
            auto Args = QJsonDocument(QJsonArray{Name, Value}).toJson(QJsonDocument::Compact);
            Args = Args.mid(1, Args.size() - 2);
            page_->runJavaScript("window.hcCommand && window.hcCommand(" + QString::fromUtf8(Args) + ")");
            if(started_ && (Name=="read" || Name=="profile" || Name=="chapter"))poll_.start(16);
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
                    if(!Error.isEmpty()) Self->statusBar()->showMessage(Error);
                }
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
            if(evidence_.isEmpty()) {latest_=Result;if(Result["status"]=="error"){finished_=true;poll_.stop();}statusBar()->showMessage(Result["status"].toString() + ": " + Result["message"].toString()); return; }
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
        QString evidence_, documentId_, expectedBookFont_;
        qint64 annotationClickSeen_=0;
        qint64 generation_ = 1;
        QJsonObject latest_{{"status", "loading"}},sentState_;
        QElapsedTimer detailClock_;bool detailPending_=true;
        QJsonObject hostAppearance_;
        hc::epub::poc::ReadingNavigation* navigation_=nullptr;
        QString settingsPath_,engine_,backgroundAsset_,astError_,resourceFailure_;
        quint64 profileSerial_=0;
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
        QJsonArray pendingBookmarks_;
        QTimer debounce_, poll_;
        bool pending_ = false, testing_ = false, finished_ = false;
    };

    // The caller retains the stable native surface through retirement. Parsing runs
    // off the UI thread; a cancelled preparation can never install a late reader.
    class OwnerController final : public QObject
    {
    public:
        OwnerController(QString Bundle, QWindow* Parent) : bundle_(std::move(Bundle)), nativeParent_(Parent)
        { connect(&timer_, &QTimer::timeout, this, [this] { transfer_.Expire(); }); timer_.start(100); }
        ~OwnerController() override { cancellation_->store(true); delete reader_; }
        std::uintptr_t ReaderWindow() const { return nativeReader_; }
        void Refresh(QSize PhysicalSize)
        {
            installPrepared();
            if(!reader_ || !nativeReader_ || !PhysicalSize.isValid()) return;
            const auto Dpr = reader_->devicePixelRatioF();
            const QRect Geometry(0, 0, qRound(PhysicalSize.width() / Dpr), qRound(PhysicalSize.height() / Dpr));
            if(reader_->geometry() != Geometry) reader_->setGeometry(Geometry);
            if(reader_->windowHandle()->geometry() != Geometry) reader_->windowHandle()->setGeometry(Geometry);
        }
        QByteArray Control(const QByteArray& Bytes)
        {
            QJsonParseError Parse; const auto Document = QJsonDocument::fromJson(Bytes, &Parse);
            QJsonObject Reply;
            if(Parse.error != QJsonParseError::NoError || !Document.isObject()) Reply = {{"error", "epub-input-json"}};
            else Reply = handle(Document.object(), Bytes);
            return QJsonDocument(Reply).toJson(QJsonDocument::Compact);
        }
    private:
        struct Prepared { QJsonObject publication; Assets assets; QString error; };
        QJsonObject handle(const QJsonObject& Request, const QByteArray& Bytes)
        {
            const auto Kind = Request["kind"].toString();
#ifdef HC_EPUB_PRODUCT_HELPER
            if(Kind == "input-begin" && Request["engine"] != QJsonValue("epubjs"))
                return {{"error", "epub-product-engine"}};
#endif
            if(Kind == "configure") {
                const auto Path = Request["settingsPath"].toString();
                if(configured_ || preparing_ || reader_ || (Request.size() != 6 && Request.size()!=7) || (Request.contains("initiallyInteractive") && !Request["initiallyInteractive"].isBool()) || !QFileInfo(Path).isAbsolute()
                    || !Path.endsWith("/epub-reading.json") || !hc::document::epubui::ApplyAppearance(Request["appearance"].toObject()))
                    return {{"error", "epub-configuration-invalid"}};
                settingsPath_ = Path; appearance_ = Request["appearance"].toObject(); initiallyInteractive_=Request["initiallyInteractive"].toBool(true); configured_ = true;
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
            if(reader_) return QJsonDocument::fromJson(reader_->Control(Bytes)).object();
            if(Kind != "snapshot" && Kind != "snapshot-delta") return {{"error", "epub-control-not-ready"}};
            auto State = transfer_.Status();
            if(cancelled_) State["status"] = "cancelled";
            else if(!error_.isEmpty()) { State["status"] = "error"; State["message"] = error_; }
            else if(preparing_) State["status"] = "preparing";
            return {{"protocolVersion", 1}, {"documentId", Request["documentId"]}, {"generation", Request["generation"]}, {"state", State}};
        }
        void installPrepared() {
            if(!prepared_ || cancelled_ || reader_)return;
            const auto Parent=reinterpret_cast<HWND>(nativeParent_->winId());
            // A passive preview may finish parsing after the user has switched
            // applications. Keep it prepared until its owner is foreground;
            // creating/reparenting a foreign Qt HWND can itself activate it.
            if(!initiallyInteractive_ && (!IsWindowVisible(Parent)
                || GetAncestor(GetForegroundWindow(),GA_ROOT)!=GetAncestor(Parent,GA_ROOT)))return;
            auto Result=std::move(*prepared_);prepared_.reset();
            reader_ = new Window(Result.assets,Result.publication,preparedEngine_,"light",{},settingsPath_,preparedGeneration_,initiallyInteractive_);
            if(!appearance_.isEmpty())reader_->Appearance(appearance_);
            reader_->setWindowFlags(Qt::Window | Qt::FramelessWindowHint | (initiallyInteractive_?Qt::WindowFlags{}:Qt::WindowFlags(Qt::WindowDoesNotAcceptFocus)));
            reader_->winId();reader_->windowHandle()->setParent(nativeParent_);reader_->setAttribute(Qt::WA_ShowWithoutActivating);
            nativeReader_=static_cast<std::uintptr_t>(reader_->winId());reader_->show();preparing_=false;
        }
        std::optional<Prepared> prepared_;QString preparedEngine_;qint64 preparedGeneration_=1;
        QString bundle_, error_, settingsPath_;
        QJsonObject appearance_;
        bool configured_ = false;
        std::shared_ptr<std::atomic_bool> cancellation_=std::make_shared<std::atomic_bool>(false);
        hc::epub::poc::InputTransfer transfer_;
        QWindow* nativeParent_ = nullptr;
        std::uintptr_t nativeReader_ = 0;
        Window* reader_ = nullptr;
        QTimer timer_;
        bool preparing_ = false, cancelled_ = false,initiallyInteractive_=true;
    };
}

int main(int Argc, char** Argv)
{
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
            OwnerController Controller(Root + "/bundle", NativeParent.get());
            if(!Peer.Ready(Surface.Window(), [&Controller](const QByteArray& Request) { return Controller.Control(Request); }, [] { qApp->quit(); }))
                throw QString("Owner native attachment failed");
            QTimer ParentTimer;
            QObject::connect(&ParentTimer, &QTimer::timeout, &Controller, [&Surface, &Controller] {
                if(!Surface.Refresh(Controller.ReaderWindow())) qApp->quit();
                else Controller.Refresh(Surface.Size());
            }); ParentTimer.start(100);
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
            : QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/HyperCommander/EpubPoc/reading-profiles.json") : QString();
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
