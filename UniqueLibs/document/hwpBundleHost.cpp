#include "hwpStartupTrace.hpp"
#include <QWebChannel>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include "hwpBundleHost.hpp"
#include "hwpEditBridge.hpp"
#include "hwpPrintBridge.hpp"
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QVBoxLayout>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEnginePermission>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineView>

namespace hc::document
{
    namespace
    {
        bool Registered = false;
        bool SafePath(const QString& Path)
        {
            static const QRegularExpression Syntax(QStringLiteral("^[A-Za-z0-9_./-]+$"));
            const auto Parts = Path.split('/');
            return Path.size() <= 512 && Syntax.match(Path).hasMatch() && Parts.size() <= 32
                && !Parts.contains("") && !Parts.contains(".") && !Parts.contains("..");
        }
        class Handler final : public QWebEngineUrlSchemeHandler
        {
        public:
            Handler(std::shared_ptr<const HwpBundle> Bundle, QObject* Parent, bool Editor)
                : QWebEngineUrlSchemeHandler(Parent), bundle_(std::move(Bundle)), editor_(Editor) {}
        protected:
            void requestStarted(QWebEngineUrlRequestJob* Job) override
            {
                if (Job->requestMethod() != "GET" || !bundle_->Allows(Job->requestUrl())) {
                    Job->fail(QWebEngineUrlRequestJob::RequestDenied); return;
                }
                auto* Buffer = new QBuffer(Job);
                Buffer->setData(bundle_->Bytes(Job->requestUrl()));
                Buffer->open(QIODevice::ReadOnly);
                const auto Path = Job->requestUrl().path();
                auto Mime = QMimeDatabase().mimeTypeForFile(Path).name().toLatin1();
                if (Path.endsWith(".js") || Path.endsWith(".mjs")) Mime = "text/javascript";
                if (Path.endsWith(".wasm")) Mime = "application/wasm";
                const QByteArray Policy = QByteArray("default-src 'none'; script-src 'self' 'wasm-unsafe-eval'; ")
                    + "style-src 'self' 'unsafe-inline'; "
                    + (editor_ ? "img-src 'self' data: blob:; font-src 'self' data: blob:; "
                        : "img-src 'self'; font-src 'self'; ")
                    + "connect-src 'self'; worker-src 'none'; object-src 'none'; "
                      "base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
                Job->setAdditionalResponseHeaders({{"Content-Security-Policy", Policy},
                    {"X-Content-Type-Options", "nosniff"}});
                Job->reply(Mime, Buffer);
            }
        private:
            std::shared_ptr<const HwpBundle> bundle_;
            bool editor_;
        };
        class Filter final : public QWebEngineUrlRequestInterceptor
        {
        public:
            Filter(std::shared_ptr<const HwpBundle> Bundle, QObject* Parent, bool Editor)
                : QWebEngineUrlRequestInterceptor(Parent), bundle_(std::move(Bundle)), editor_(Editor) {}
            void interceptRequest(QWebEngineUrlRequestInfo& Info) override
            {
                // Fail closed, including file/qrc/http/data/blob. Additional
                // renderer capabilities need their own reviewed bridge policy.
                const auto Scheme = Info.requestUrl().scheme();
                const bool MemoryAsset = editor_ && (Scheme == "data" || Scheme == "blob")
                    && (Info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeImage
                        || Info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeFontResource);
                if (Info.requestMethod() != "GET" || (!MemoryAsset && !bundle_->Allows(Info.requestUrl()))) Info.block(true);
            }
        private:
            std::shared_ptr<const HwpBundle> bundle_;
            bool editor_;
        };
        class Page final : public QWebEnginePage
        {
        public:
            Page(QWebEngineProfile* Profile, QObject* Parent, QUrl Entry) : QWebEnginePage(Profile, Parent), entry_(std::move(Entry)) {}
        protected:
            bool acceptNavigationRequest(const QUrl& Url, NavigationType, bool MainFrame) override
            { return MainFrame && Url == entry_; }
            QWebEnginePage* createWindow(WebWindowType) override { return nullptr; }
            QStringList chooseFiles(FileSelectionMode, const QStringList&, const QStringList&) override { return {}; }
        private:
            QUrl entry_;
        };
    }
    std::shared_ptr<const HwpBundle> HwpBundle::LoadPinned(const QString& Root,
        const QMap<QString, QByteArray>& Pins, QString& Error)
    {
        startup::Scope Verification("helper.bundleReadAndHash");
        Error.clear();
        auto Fail = [&](const char* Code) -> std::shared_ptr<const HwpBundle> { Error = QString::fromLatin1(Code); return {}; };
        if (Pins.isEmpty() || Pins.size() > 512 || !Pins.contains("index.html")) return Fail("rhwp-bundle-pins");
        const bool Resource = Root == QStringLiteral(":/hc/rhwp/bundle");
        if (!Resource && !QFileInfo(Root).isAbsolute()) return Fail("rhwp-bundle-root");
        const auto Canonical = Resource ? Root : QFileInfo(Root).canonicalFilePath();
        if (Canonical.isEmpty() || !QFileInfo(Canonical).isDir()) return Fail("rhwp-bundle-root");
        auto Bundle = std::make_shared<HwpBundle>();
        QSet<QString> Seen; qint64 Total = 0;
        for (auto At = Pins.cbegin(); At != Pins.cend(); ++At) {
            if (!SafePath(At.key()) || Seen.contains(At.key().toCaseFolded()) || At.value().size() != 64
                || QByteArray::fromHex(At.value()).toHex() != At.value()) return Fail("rhwp-bundle-entry");
            Seen.insert(At.key().toCaseFolded());
            const auto Path = Resource ? Canonical + '/' + At.key()
                : QFileInfo(Canonical + '/' + At.key()).canonicalFilePath();
            if (Path.isEmpty() || !Path.startsWith(Canonical + '/', Qt::CaseInsensitive) || !QFileInfo(Path).isFile())
                return Fail("rhwp-bundle-path");
            QFile File(Path);
            if (!File.open(QIODevice::ReadOnly)) return Fail("rhwp-bundle-read");
            const auto Size = File.size();
            if (Size < 0 || Size > 64LL * 1024 * 1024) return Fail("rhwp-bundle-size");
            // QIODevice may retain capacity for the requested read size. Reading
            // the per-file ceiling for every small resource made a 71-file bundle
            // reserve over 4 GiB before Ready. Allocate for the captured size and
            // one growth-detection byte instead; still reject concurrent changes.
            auto Bytes = File.read(Size + 1);
            Total += Bytes.size();
            if (File.error() != QFileDevice::NoError || !File.atEnd() || Bytes.size() != Size
                || Total > 256LL * 1024 * 1024) return Fail("rhwp-bundle-budget");
            if (QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex() != At.value()) return Fail("rhwp-bundle-hash");
            Bundle->files_.insert(At.key(), std::move(Bytes));
        }
        Verification.End(Total,Pins.size());
        return Bundle;
    }
    bool HwpBundle::Allows(const QUrl& Url) const
    {
        if (!Url.isValid() || Url.scheme() != "rhwp" || Url.host() != "bundle"
            || !Url.userInfo().isEmpty() || Url.port() != -1 || Url.hasQuery() || Url.hasFragment()) return false;
        const auto Encoded = Url.path(QUrl::FullyEncoded);
        if (!Encoded.startsWith('/') || Encoded.contains('%')) return false;
        const auto Relative = Encoded.mid(1);
        return SafePath(Relative) && files_.contains(Relative);
    }
    QByteArray HwpBundle::Bytes(const QUrl& Url) const
    { return Allows(Url) ? files_.value(Url.path().mid(1)) : QByteArray{}; }
    bool RegisterHwpScheme()
    {
        if (QCoreApplication::instance()) return false;
        if (Registered) return true;
        if (!QWebEngineUrlScheme::schemeByName("rhwp").name().isEmpty()) return false;
        QWebEngineUrlScheme Scheme("rhwp");
        Scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
        Scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::CorsEnabled
            | QWebEngineUrlScheme::FetchApiAllowed);
        QWebEngineUrlScheme::registerScheme(Scheme);
        Registered = true; return true;
    }
    bool HwpWebArgumentsAllowed(const QStringList& Arguments)
    {
        for (const auto& Argument : Arguments) {
            // Qt finds the WebEngine argument delimiter case-insensitively.
            // Reject the entire passthrough surface, not only selected switches.
            const auto Lower = Argument.toLower();
            if (Lower.startsWith("--webengineargs") || Lower.startsWith("--no-sandbox")
                || Lower.startsWith("--single-process") || Lower.startsWith("--disable-web-security")
                || Lower.startsWith("--remote-debugging")) return false;
        }
        return true;
    }
    bool HwpWebEnvironmentAllowed()
    {
        if (QCoreApplication::instance() && !HwpWebArgumentsAllowed(QCoreApplication::arguments())) return false;
        return !qEnvironmentVariableIsSet("QTWEBENGINE_DISABLE_SANDBOX")
            && qgetenv("QTWEBENGINE_CHROMIUM_FLAGS").isEmpty()
            && !qEnvironmentVariableIsSet("QTWEBENGINE_REMOTE_DEBUGGING");
    }
    std::unique_ptr<HwpBundleView> HwpBundleView::Create(std::shared_ptr<const HwpBundle> Bundle, QString& Error, HwpEditBridge* Editor, HwpPrintBridge* Print)
    {
        startup::Scope Creation("helper.viewCreate");
        Error.clear();
        if (!Registered || !qobject_cast<QApplication*>(QCoreApplication::instance())
            || QThread::currentThread() != QCoreApplication::instance()->thread()) {
            Error = "rhwp-web-initialization"; return {};
        }
        if (!Bundle || !Bundle->Allows(QUrl("rhwp://bundle/index.html"))) { Error = "rhwp-web-bundle"; return {}; }
        if (Editor && (!Bundle->Allows(QUrl("rhwp://bundle/editor/index.html")) || Editor->State().isEmpty())) { Error = "rhwp-editor-not-admitted"; return {}; }
        if (!HwpWebEnvironmentAllowed()) { Error = "rhwp-web-environment"; return {}; }
        QFile ChannelScript(":/qtwebchannel/qwebchannel.js");
        if (!ChannelScript.open(QIODevice::ReadOnly)) { Error = "rhwp-web-channel-resource"; return {}; }
        return std::unique_ptr<HwpBundleView>(new HwpBundleView(std::move(Bundle),
            QString::fromUtf8(ChannelScript.readAll()), Editor, Print));
    }
    HwpBundleView::HwpBundleView(std::shared_ptr<const HwpBundle> Bundle, const QString& ChannelScript, HwpEditBridge* Editor, HwpPrintBridge* Print)
        : profile_(new QWebEngineProfile(this)), view_(new QWebEngineView(this))
    {
        startup::Scope ViewSetup("helper.viewSetup");
        profile_->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
        profile_->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
        profile_->installUrlSchemeHandler("rhwp", new Handler(Bundle, profile_, Editor != nullptr));
        profile_->setUrlRequestInterceptor(new Filter(Bundle, profile_, Editor != nullptr));
        connect(profile_, &QWebEngineProfile::downloadRequested, this,
            [](QWebEngineDownloadRequest* Request) { Request->cancel(); });
        const QUrl Entry(Editor ? "rhwp://bundle/editor/index.html" : "rhwp://bundle/index.html");
        auto* Page = new hc::document::Page(profile_, view_, Entry);
        connect(Page, &QWebEnginePage::permissionRequested, this,
            [](QWebEnginePermission Permission) { Permission.deny(); });
        for (const auto Attribute : {QWebEngineSettings::JavascriptCanOpenWindows,
            QWebEngineSettings::JavascriptCanAccessClipboard, QWebEngineSettings::JavascriptCanPaste,
            QWebEngineSettings::LocalContentCanAccessRemoteUrls, QWebEngineSettings::LocalContentCanAccessFileUrls,
            QWebEngineSettings::LocalStorageEnabled, QWebEngineSettings::FullScreenSupportEnabled})
            Page->settings()->setAttribute(Attribute, false);
        Page->settings()->setUnknownUrlSchemePolicy(QWebEngineSettings::DisallowUnknownUrlSchemes);
        bridge_ = new HwpLoadBridge(this);
        auto* Channel = new QWebChannel(Page);
        Channel->registerObject("loadBridge", new HwpLoadEndpoint(bridge_, Page));
        if (Editor) Channel->registerObject("editBridge", new HwpEditEndpoint(Editor, Page));
        if (Print) Channel->registerObject("printBridge", new HwpPrintEndpoint(Print, Page));
        Page->setWebChannel(Channel);
        QWebEngineScript Script;
        Script.setName("rhwp-native-channel");
        Script.setInjectionPoint(QWebEngineScript::DocumentCreation);
        Script.setWorldId(QWebEngineScript::MainWorld);
        Script.setRunsOnSubFrames(false);
        auto Injected = ChannelScript;
        if(startup::Enabled()) Injected += QStringLiteral("\n;")+QString::fromUtf8(Bundle->Bytes(QUrl("rhwp://bundle/startupTrace.js")));
        Script.setSourceCode(Injected);
        Page->scripts().insert(Script);
        connect(Page, &QWebEnginePage::renderProcessTerminated, this,
            [this] { if (bridge_) bridge_->FailLoad(); });
        connect(Page, &QWebEnginePage::loadFinished, this,
            [this](bool Success) { if (!Success && bridge_) bridge_->FailLoad(); });
        view_->setPage(Page);
        auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0); Layout->addWidget(view_);
        startup::Data().Begin("helper.navigationToBridge");
        view_->load(Entry);
    }
    HwpBundleView::~HwpBundleView()
    {
        // Page must die before its profile and scheme/interceptor owners.
        delete view_;
        delete profile_;
    }
    bool HwpBundleView::IsOffTheRecord() const { return profile_->isOffTheRecord(); }
}
