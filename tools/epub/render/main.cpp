#include "stdafx.h"
#include "readingSettings.hpp"
#include "readingNavigation.hpp"
#include "commonLibs/document/epub/epubAppearance.hpp"
#include "ownerMode.hpp"
#include "inputTransfer.hpp"
#include "commonLibs/document/epub/epubPackage.hpp"
#include <QWindow>
#include <QPointer>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QFileInfo>
#include <memory>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include "UniqueLibs/document/documentWebPeer.hpp"
#include "UniqueLibs/document/documentShortcutRouter.hpp"
#include <QAbstractSpinBox>

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
    struct Asset { QByteArray data; QByteArray mime; };
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
    Assets loadAssets(QJsonObject& Publication, const QString& Bundle)
    {
        if(Publication["schemaVersion"].toInt() != 1 || Publication["phase"] != "P0-B"
            || Publication["policy"] != "passive-reflowable-v1" || Publication["spine"].toArray().isEmpty())
            throw QString("Unsupported prepared publication");
        Assets Result;
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
            "<!doctype html><html lang='ko'><head><meta charset='utf-8'><title>EPUB P0</title>"
            "<style>html,body{margin:0;height:100%;overflow:hidden}#viewport{height:100%;margin:auto;max-width:100%}"
            "#reader{position:relative;width:100%;height:100%;margin:auto;max-width:100%}"
            ".readium-navigator-iframe{border:0;inset:0;width:100%;height:100%}</style>"
            "</head><body><main id='viewport' aria-label='EPUB 본문'><div id='reader'></div></main><script src='reader.js'></script></body></html>"), "text/html"});
        return Result;
    }

    class Handler final : public QWebEngineUrlSchemeHandler
    {
    public:
        explicit Handler(const Assets& Items, QObject* Parent) : QWebEngineUrlSchemeHandler(Parent), items_(Items)
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
                {"X-Content-Type-Options", "nosniff"}, {"Cache-Control", "no-store"}});
            auto* Buffer = new QBuffer(Job);
            Buffer->setData(Item.data);
            Buffer->open(QIODevice::ReadOnly);
            Job->reply(Item.mime, Buffer);
        }
    private:
        Assets items_;
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
        Window(const Assets& Items, const QJsonObject& Publication, const QString& Engine, const QString& Theme, const QString& Evidence, const QString& SettingsPath, qint64 Generation = 1)
            : evidence_(Evidence), documentId_(Publication["sourceSha256"].toString()), generation_(Generation)
        {
            engine_=Engine;
#ifdef HC_EPUB_PRODUCT_HELPER
            setWindowTitle(tr("EPUB 읽기 설정"));
#else
            setWindowTitle(tr("EPUB · Watercolor 읽기 설정 · P0 엔진 비교"));
#endif
            resize(QSize(1280,900).boundedTo(QGuiApplication::primaryScreen()->availableGeometry().size()-QSize(40,60)));
            settingsPath_ = SettingsPath;
            auto* Central = new QWidget(this);
            setCentralWidget(Central);
            auto* Layout = new QVBoxLayout(Central);
            auto* Header = new QLabel(Publication["title"].toString() + tr(" — EPUB 읽기"), Central);
            Header->setTextFormat(Qt::PlainText); Layout->addWidget(Header);
            const auto Missing = Publication["recovery"].toObject()["missingSpineItems"].toInt();
            if(Missing > 0) {
                auto* Notice = new QLabel(tr("읽기 순서에서 찾을 수 없는 항목 %1개를 제외하고, 존재하는 본문 순서로 복구해 표시합니다. 원본 파일은 변경하지 않았습니다.").arg(Missing), Central);
                Notice->setObjectName("epubRecoveryNotice"); Notice->setTextFormat(Qt::PlainText); Notice->setWordWrap(true); Layout->addWidget(Notice);
            }
            auto* Splitter = new QSplitter(Central); Layout->addWidget(Splitter, 1);
            auto* Scroll = new QScrollArea(Splitter); settingsScroll_ = Scroll; Scroll->setWidgetResizable(true); Scroll->setMinimumWidth(400);
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
                connect(Action,&QAction::triggered,this,[this,Name=Entry.first]{navigation_->Execute(Name);view_->setFocus(Qt::OtherFocusReason);});
            }
            location_=new QLabel(ReadingBar);location_->setObjectName("epubReadingLocation");location_->setAccessibleName(tr("현재 읽기 위치"));ReadingBar->addWidget(location_);
            Form->addRow(navigation_);
            Form->addRow(settings_);
            view_ = new QWebEngineView(Splitter); Splitter->setSizes({400, 880});
            profile_ = new QWebEngineProfile(this); profile_->setHttpCacheType(QWebEngineProfile::NoCache);
            profile_->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
            filter_ = new Filter(Items, profile_); profile_->setUrlRequestInterceptor(filter_);
            profile_->installUrlSchemeHandler("epub", new Handler(Items, profile_));
            page_ = new Page(profile_, view_); view_->setPage(page_);
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
                page_->runJavaScript("JSON.stringify({...((window.hcSnapshot && window.hcSnapshot()) || window.hcEvidence), startup: window.hcStartupDiagnostics ? window.hcStartupDiagnostics() : {}, bookFonts: window.hcBookFonts ? window.hcBookFonts() : []})", [this](const QVariant& Value) {
                    pending_ = false; if(finished_) return;
                    const auto Result = QJsonDocument::fromJson(Value.toString().toUtf8()).object();
                    latest_ = Result;
                    navigation_->Poll(Result);
                    if(Result["status"]=="ready") {
                        location_->setText(tr("%1 · 장 %2/%3 · 화면 %4/%5 · 전체 약 %6% (장 기준)")
                            .arg(Result["chapterTitle"].toString()).arg(Result["chapterIndex"].toInt()+1).arg(Result["chapterCount"].toInt())
                            .arg(Result["chapterPage"].toInt(1)).arg(Result["chapterPages"].toInt(1)).arg(qRound(Result["progress"].toDouble()*100)));
                        findChild<QAction*>("epub.back")->setEnabled(Result["canBack"].toBool());
                        findChild<QAction*>("epub.previous")->setEnabled(!Result["atStart"].toBool());
                        findChild<QAction*>("epub.next")->setEnabled(!Result["atEnd"].toBool());
                    }
                    if(Result["profile"].toObject()["customCss"]==settings_->Profile()["customCss"])
                        settings_->SetCssValidation(settings_->Profile()["customCss"].toString(),astError_.isEmpty()?Result["cssError"].toString():astError_);
                    QStringList Families;
                    for(const auto& Family : Result["bookFonts"].toArray()) if(Family.isString()) Families.append(Family.toString());
                    settings_->SetBookFonts(Families);
                    const auto Status = Result["status"].toString();
                    statusBar()->showMessage(tr("%1 · 읽기 전용 · 원본 유지").arg(Status));
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
                && ((!qobject_cast<QLineEdit*>(Focus) && !qobject_cast<QPlainTextEdit*>(Focus) && !qobject_cast<QAbstractSpinBox*>(Focus))
                    || Id=="epub.cycle-focus" || Id.startsWith("epub.focus-"));});
            shortcuts_->Apply({});
            auto* PanelKey=new QShortcut(QKeySequence("Ctrl+Shift+R"),this);connect(PanelKey,&QShortcut::activated,this,[this]{if(!composing_){settingsScroll_->setVisible(!settingsScroll_->isVisible());view_->setFocus();}});
            qApp->installEventFilter(this);
            view_->setUrl(QUrl("epub://app/app/index.html"));
        }
        void Appearance(const QJsonObject& Value) {
            if(hc::document::epubui::ApplyAppearance(Value)) { hostAppearance_ = Value; sendProfile(); }
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
            if(Kind == "snapshot") {
                auto State = latest_;const auto Navigation=navigation_->Snapshot();for(auto I=Navigation.begin();I!=Navigation.end();++I)State[I.key()]=I.value(); State["inputFocused"] = latest_["readerFocused"].toBool();
                State["escapeRequests"]=escapeRequests_;State["appearance"] = hostAppearance_; State["settingsPersistent"] = !settingsPath_.isEmpty();
                State["settingsBusy"] = settings_->IsBusy(); State["settingsDirty"] = settings_->IsDirty();
                State["readingProfile"] = settings_->Profile(); State["cssValid"]=settings_->CssValid(); State["cssValidationError"]=settings_->CssError(); Reply["state"] = State;
            }
            else if(Kind == "reading-settings") {
                if(Request.size()!=7 || !Request["profile"].isObject() || !Request["global"].isBool()
                    || !settings_->Execute(Request["action"].toString(),Request["profile"].toObject(),Request["global"].toBool())) return Fail("epub-settings-rejected");
            }
            else if(Kind=="reader-action") {
                if(Request.size()!=6 || !readAction(Request["action"].toString(),Request["value"]))return Fail("epub-reader-action-rejected");
            }
            else if(Kind == "appearance") {
                if(!hc::document::epubui::ApplyAppearance(Request["appearance"].toObject())) return Fail("epub-appearance-invalid");
                Appearance(Request["appearance"].toObject());
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
                if(Visible) focusReading();
                Reply["focusRequested"] = Visible;
            }
            else return Fail("epub-control-command");
            Reply["accepted"] = true;
            return QJsonDocument(Reply).toJson(QJsonDocument::Compact);
        }
    protected:
        bool eventFilter(QObject* Object,QEvent* Event) override {
            auto* Widget=qobject_cast<QWidget*>(Object);if(!Widget || (Widget!=this && !isAncestorOf(Widget)))return false;
            if(Event->type()==QEvent::InputMethod)composing_=!static_cast<QInputMethodEvent*>(Event)->preeditString().isEmpty();
            if(Event->type()==QEvent::KeyPress) {
                auto* Key=static_cast<QKeyEvent*>(Event);
                const bool Body=Widget==view_ || view_->isAncestorOf(Widget);
                if(Body && !composing_ && settings_->Profile()["layout"]=="paginated" && Key->modifiers()==Qt::NoModifier
                    && (Key->key()==Qt::Key_Left || Key->key()==Qt::Key_Right)) {
                    const bool Forward=(Key->key()==Qt::Key_Right)!=(latest_["readingProgression"]=="rtl");
                    readAction(Forward?"next":"previous",{});return true;
                }
                if(Body && !composing_ && !QApplication::activeModalWidget() && !QApplication::activePopupWidget()
                    && Key->key()==Qt::Key_Space && (Key->modifiers()==Qt::NoModifier || Key->modifiers()==Qt::ShiftModifier)) {
                    readAction(Key->modifiers()==Qt::ShiftModifier?"previous":"next",{});return true;
                }
                if(Key->key()==Qt::Key_Escape && Key->modifiers()==Qt::NoModifier && !Key->isAutoRepeat() && !composing_ && !QApplication::activeModalWidget() && !QApplication::activePopupWidget()) {
                    const QPointer<Window> Guard(this);const QPointer<QWidget> Receiver(Widget);
                    page_->runJavaScript("!!(window.hcDismissPopup && window.hcDismissPopup())",[Guard,Receiver](const QVariant& Closed){
                        if(!Guard || Closed.toBool())return;
                        if(auto* Input=qobject_cast<QLineEdit*>(Receiver.data());Input && !Input->text().isEmpty()){Input->clear();return;}
                        if(Guard->settings_->IsDirty()){Guard->settings_->Execute("cancel",{},false);return;}
                        if(Guard->settingsScroll_->isVisible()){Guard->settingsScroll_->hide();Guard->focusReading();return;}
                        ++Guard->escapeRequests_;
                    });return true;
                }
            }
            return QMainWindow::eventFilter(Object,Event);
        }
    private:
        void focusReading() {
            activateWindow();view_->setFocus(Qt::OtherFocusReason);
            page_->runJavaScript("window.hcFocusReading && window.hcFocusReading()");
        }
        bool readAction(const QString& Action,const QJsonValue& Value) {
            if(Action=="bindings")return Value.isObject() && shortcuts_->Apply(Value.toObject()).isEmpty();
            if(Action=="cycle-focus") {
                if(view_->hasFocus() || view_->isAncestorOf(QApplication::focusWidget())){settingsScroll_->show();navigation_->Execute("focus-toc");}
                else if(navigation_->isAncestorOf(QApplication::focusWidget())){settingsScroll_->show();settings_->findChild<QComboBox*>("scope")->setFocus(Qt::ShortcutFocusReason);}
                else focusReading();return true;
            }
            if(Action=="focus-settings"){settingsScroll_->show();settings_->findChild<QComboBox*>("scope")->setFocus(Qt::ShortcutFocusReason);return true;}
            if(Action=="focus-search" || Action=="focus-toc")settingsScroll_->show();
            return navigation_->Execute(Action,Value);
        }
        void command(const QString& Name, const QJsonValue& Value)
        {
            auto Args = QJsonDocument(QJsonArray{Name, Value}).toJson(QJsonDocument::Compact);
            Args = Args.mid(1, Args.size() - 2);
            page_->runJavaScript("window.hcCommand && window.hcCommand(" + QString::fromUtf8(Args) + ")");
        }
        void sendProfile()
        {
            if(!shellReady_) return;
            const auto Values=settings_->Profile(); const auto Css=Values["customCss"].toString();
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
                if(!Self->started_) {Self->started_=true;Self->command("start",Self->engine_);Self->poll_.start(250);}
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
            if(evidence_.isEmpty()) { statusBar()->showMessage(Result["status"].toString() + ": " + Result["message"].toString()); return; }
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
        qint64 generation_ = 1;
        QJsonObject latest_{{"status", "loading"}};
        QJsonObject hostAppearance_;
        hc::epub::poc::ReadingNavigation* navigation_=nullptr;
        QString settingsPath_,engine_,backgroundAsset_,astError_;
        quint64 profileSerial_=0;
        bool shellReady_=false,started_=false,composing_=false;
        int escapeRequests_=0;
        hc::epub::poc::ReadingSettings* settings_ = nullptr;
        QWebEngineView* view_ = nullptr;
        QLabel* location_=nullptr;
        hc::document::DocumentShortcutRouter* shortcuts_=nullptr;
        QScrollArea* settingsScroll_ = nullptr;
        QWebEngineProfile* profile_ = nullptr;
        Page* page_ = nullptr;
        Filter* filter_ = nullptr;
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
        ~OwnerController() override { delete reader_; }
        std::uintptr_t ReaderWindow() const { return nativeReader_; }
        void Refresh(QSize PhysicalSize)
        {
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
                if(configured_ || preparing_ || reader_ || Request.size() != 6 || !QFileInfo(Path).isAbsolute()
                    || !Path.endsWith("/epub-reading.json") || !hc::document::epubui::ApplyAppearance(Request["appearance"].toObject()))
                    return {{"error", "epub-configuration-invalid"}};
                settingsPath_ = Path; appearance_ = Request["appearance"].toObject(); configured_ = true;
                return {{"accepted", true}};
            }
            if(Kind.startsWith("input-")) {
                if(Kind == "input-cancel" && preparing_ && Request.size() == 4 && transfer_.Matches(Request)) {
                    cancelled_ = true;
                    return {{"accepted", true}, {"status", "cancelled"}};
                }
                auto Reply = transfer_.Handle(Request);
                if(Kind == "input-commit" && Reply["accepted"].toBool()) {
                    auto Input = transfer_.Take();
                    if(!Input) return {{"error", "epub-input-state"}};
                    preparing_ = true;
                    auto* Watcher = new QFutureWatcher<Prepared>(this);
                    connect(Watcher, &QFutureWatcher<Prepared>::finished, this,
                        [this, Watcher, Engine = Input->engine, Generation = Input->generation] {
                        const auto Result = Watcher->result(); Watcher->deleteLater(); preparing_ = false;
                        if(cancelled_) return;
                        if(!Result.error.isEmpty()) { error_ = Result.error; return; }
                        reader_ = new Window(Result.assets, Result.publication, Engine, "light", {}, settingsPath_, Generation);
                        if(!appearance_.isEmpty()) reader_->Appearance(appearance_);
                        reader_->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
                        reader_->winId(); reader_->windowHandle()->setParent(nativeParent_);
                        nativeReader_ = static_cast<std::uintptr_t>(reader_->winId()); reader_->show();
                    });
                    Watcher->setFuture(QtConcurrent::run([Input = std::move(*Input), Bundle = bundle_]() mutable {
                        Prepared Result;
                        try {
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
            if(Kind != "snapshot") return {{"error", "epub-control-not-ready"}};
            auto State = transfer_.Status();
            if(cancelled_) State["status"] = "cancelled";
            else if(!error_.isEmpty()) { State["status"] = "error"; State["message"] = error_; }
            else if(preparing_) State["status"] = "preparing";
            return {{"protocolVersion", 1}, {"documentId", Request["documentId"]}, {"generation", Request["generation"]}, {"state", State}};
        }
        QString bundle_, error_, settingsPath_;
        QJsonObject appearance_;
        bool configured_ = false;
        hc::epub::poc::InputTransfer transfer_;
        QWindow* nativeParent_ = nullptr;
        std::uintptr_t nativeReader_ = 0;
        Window* reader_ = nullptr;
        QTimer timer_;
        bool preparing_ = false, cancelled_ = false;
    };
}

int main(int Argc, char** Argv)
{
    hc::epub::poc::OwnerMode Owner;
    try {
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
        if(!qEnvironmentVariableIsEmpty("QTWEBENGINE_DISABLE_SANDBOX")
            || qEnvironmentVariable("QTWEBENGINE_CHROMIUM_FLAGS").contains("--no-sandbox")) throw QString("Sandbox bypass is not allowed");
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
        if(Parser.isSet("epub")) {
            const auto Prepared = hc::document::epub::PrepareFile(Parser.value("epub"));
            if(!Prepared) throw Prepared.error_ + ": " + Prepared.location_;
            Publication = Prepared.publication_;
        } else Publication = readObject(Parser.value("publication"), MaxSnapshot);
        const auto Items = loadAssets(Publication, Parser.value("bundle"));
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
