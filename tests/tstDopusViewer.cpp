#include "pluginHost/hostedWlxSession.hpp"
#include "pluginHost/hostedDataProviders.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"
#include "UniqueLibs/qtViewerHost.hpp"
#include "memoryStorage.hpp"
#include <QtTest>
#include <QApplication>
#include <qt_windows.h>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QFile>
#include <QScopeGuard>
#include <future>
#include <cstring>

using namespace hc::plugins;
using namespace hc::storage;
namespace
{
    template<class F> auto Worker(F&& Function)
    {
        auto Future = std::async(std::launch::async, std::forward<F>(Function));
        while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) QTest::qWait(5);
        return Future.get();
    }
    HWND Window(const QJsonObject& Reply, const char* Key)
    { return reinterpret_cast<HWND>(quintptr(Reply[Key].toString().toULongLong(nullptr, 16))); }
    std::shared_ptr<IContentSource> Content(const std::string& Name)
    {
        auto Provider = std::make_shared<hc::storage::test::MemoryProvider>();
        const auto Id = Provider->Add(Provider->Root(), Name, {std::byte{'x'}});
        struct VirtualSource final : IContentSource
        {
            std::shared_ptr<IContentSource> source_;
            EntrySnapshot Describe() const override { auto Entry = source_->Describe(); Entry.locator_ = {LocatorKind::Plugin, "test:memory:\\" + Entry.name_}; return Entry; }
            Task<Unit> Verify(OperationContext Context) override { return source_->Verify(Context); }
            Task<std::shared_ptr<IReadStream>> OpenRead(OperationContext Context) override { return source_->OpenRead(Context); }
        };
        auto Source = std::make_shared<VirtualSource>(); Source->source_ = std::make_shared<ProviderContentSource>(Provider, Provider->Stat(Id, {}).get().Value()); return Source;
    }
}
class DopusViewerTests : public QObject
{
    Q_OBJECT
private:
    bool prepare(QTemporaryDir& Package, WlxViewerOptions& Options, bool Optional = true)
    {
        Options.executable_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe";
        Options.manifestPath_ = Package.filePath("plugin.json"); Options.binaryPath_ = Package.filePath("viewer.dll");
        if (!QFile::copy(QCoreApplication::applicationDirPath() + "/hc_dopus_fixture.dll", Options.binaryPath_)) return false;
        QFile Binary(Options.binaryPath_); if (!Binary.open(QIODevice::ReadOnly)) return false;
        PluginManifest Manifest; Manifest.id_ = "hc.dopus.viewer"; Manifest.version_ = "1.0"; Manifest.binary_ = "viewer.dll";
        Manifest.sha256_ = QCryptographicHash::hash(Binary.readAll(), QCryptographicHash::Sha256).toHex().toStdString();
        Manifest.format_ = PluginFormat::DopusViewer; Manifest.extensions_ = {Extension::Viewer};
        if (Optional) { Manifest.extensions_.insert(Extension::Metadata); Manifest.extensions_.insert(Extension::ContentConvert); }
        Manifest.permissions_ = {Permission::ReadContent, Permission::UserInterface};
        Options.approval_ = {Manifest, true, false, Manifest.permissions_}; Options.limits_.requestMs_ = 1000;
        QFile Json(Options.manifestPath_); return Json.open(QIODevice::WriteOnly) && Json.write(EncodeManifest(Manifest)) > 0;
    }
    QString file(QTemporaryDir& Directory, const QString& Name)
    { QFile Value(Directory.filePath(Name)); if (!Value.open(QIODevice::WriteOnly) || Value.write("x") != 1) return {}; return Value.fileName(); }
private slots:
    void nativeAndBitmapLifecycle_data()
    { QTest::addColumn<QString>("Name"); QTest::newRow("bitmap") << "한글.bitmap"; QTest::newRow("native") << "한글.native"; QTest::newRow("plain-text") << "한글.text"; }
    void nativeAndBitmapLifecycle()
    {
        QFETCH(QString, Name); QTemporaryDir Package; WlxViewerOptions Options; QVERIFY(prepare(Package, Options));
        QWidget Parent; Parent.resize(480, 320); Parent.show(); QVERIFY(QTest::qWaitForWindowExposed(&Parent));
        auto Started = Worker([&] { return PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_); });
        QVERIFY(Started.HasValue()); auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Path = file(Package, Name); QVERIFY(!Path.isEmpty());
        auto Probe = Worker([&] { return Host->Probe(); }); QVERIFY(Probe.HasValue()); QCOMPARE(Probe.Value()["format"].toString(), QString("dopus-viewer"));
        for (int Index = 0; Index < 8; ++Index)
        {
            auto Opened = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, {{"parent", QString::number(Parent.winId(), 16)}, {"file", Path}, {"width", 480}, {"height", 320}, {"flags", 0}}); });
            QVERIFY2(Opened.HasValue(), Opened.HasValue() ? "" : Opened.Failure().code_.c_str());
            const auto Child = Window(Opened.Value(), "viewer"); QVERIFY(IsWindow(Child)); QVERIFY(IsChild(reinterpret_cast<HWND>(Parent.winId()), Child));
            QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerResize, {{"width", 300}, {"height", 200}}); }).HasValue());
            RECT Rect{}; QVERIFY(GetWindowRect(Child, &Rect)); QCOMPARE(Rect.right - Rect.left, 300L); QCOMPARE(Rect.bottom - Rect.top, 200L);
            QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerFocus, {}); }).HasValue());
            if (Name.endsWith(".native") || Name.endsWith(".text"))
            {
                wchar_t Text[1024]{}; DWORD_PTR Length = 0; QVERIFY(SendMessageTimeoutW(Child, WM_GETTEXT, 1024, reinterpret_cast<LPARAM>(Text), SMTO_ABORTIFHUNG, 1000, &Length));
                QVERIFY(QString::fromWCharArray(Text).contains(Name.endsWith(".native") ? Name : QString("converted fixture text")));
            }
            QVERIFY(Worker([&] { return Host->ViewerCall(WireKind::ViewerClose, {}); }).HasValue()); QVERIFY(!IsWindow(Child));
        }
        Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().forcedTermination_); QCOMPARE(Host->Snapshot().exitCode_.value_or(1), 0u);
    }
    void optionalServicesAndDenial()
    {
        for (bool Optional : {true, false})
        {
            QTemporaryDir Package; WlxViewerOptions Options; QVERIFY(prepare(Package, Options, Optional));
            auto Started = Worker([&] { return PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_); }); QVERIFY(Started.HasValue());
            auto Host = std::shared_ptr<PluginProcess>(std::move(Started.Value())); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host->Close(); }); });
            const auto Path = file(Package, "input.text");
            auto Converted = Worker([&] { return Host->DataCall(WireKind::ContentConvert, {{"path", Path}}); });
            if (!Optional)
            {
                QVERIFY(!Converted.HasValue()); QCOMPARE(Converted.Failure().category_, ErrorCategory::AccessDenied);
                QVERIFY(!Worker([&] { return Host->DataCall(WireKind::MetadataQuery, {{"action", "fields"}}); }).HasValue()); continue;
            }
            QVERIFY(Converted.HasValue()); QCOMPARE(QByteArray::fromBase64(Converted.Value()["bytes"].toString().toLatin1()), QByteArray("converted fixture text"));
            const auto Large = file(Package, "large.text");
            auto OverBudget = Worker([&] { return Host->DataCall(WireKind::ContentConvert, {{"path", Large}}); });
            QVERIFY(!OverBudget.HasValue()); QCOMPARE(OverBudget.Failure().category_, ErrorCategory::BudgetExceeded);
            for (const auto& [Name, Type] : std::vector<std::pair<QString, int>>{{"html.text", 2}, {"rich.text", 1}})
            {
                const auto Input = file(Package, Name);
                auto Reply = Worker([&] { return Host->DataCall(WireKind::ContentConvert, {{"path", Input}}); }); QVERIFY(Reply.HasValue()); QCOMPARE(Reply.Value()["textType"].toInt(), Type);
            }
            auto Materializer = MakeNativeMaterializer(Package.path().toStdString()); QVERIFY(Materializer.HasValue());
            auto Metadata = Worker([&] { return MakeHostedDopusMetadataProvider(Host, Materializer.Value()); }); QVERIFY(Metadata.HasValue());
            auto Fields = Worker([&] { return Metadata.Value()->Fields({}).get(); }); QVERIFY(Fields.HasValue()); QCOMPARE(Fields.Value().size(), 6u);
            auto Value = Worker([&] { return Metadata.Value()->Query(Content("sample.bitmap"), {1, 0, false}, {}).get(); });
            QVERIFY(Value.HasValue()); QCOMPARE(std::get<std::int64_t>(Value.Value().value_), 2);
            auto Converter = Worker([&] { return MakeHostedDopusConverter(Host, Materializer.Value()); }); QVERIFY(Converter.HasValue());
            auto ContentResult = Worker([&] { return Converter.Value()->Convert(Content("sample.text"), {}).get(); }); QVERIFY(ContentResult.HasValue());
            QCOMPARE(ContentResult.Value().type_, ConvertedTextType::Plain); QCOMPARE(ContentResult.Value().bytes_.size(), 22u);
            Worker([&] { Host->Close(); }); QVERIFY(!Host->Snapshot().forcedTermination_); QCOMPARE(Host->Snapshot().exitCode_.value_or(1), 0u);
        }
    }
    void automaticSurfaceAndCancellation()
    {
        QTemporaryDir Package; WlxViewerOptions Options; QVERIFY(prepare(Package, Options));
        QWidget Parent; Parent.resize(500, 300); Parent.show();
        QtViewerHost Host(&Parent, [&] { return MakeNativeMaterializer(Package.path().toStdString()); },
            [&] { return std::vector<QtViewerSurface::WlxChoice>{{"dopus", "DOpus", Options}}; }, [] { return QtViewerSurface::WlxSelectionMode::Automatic; });
        const auto Cleanup = qScopeGuard([&] { Host.CloseAll(); while (!Host.IsIdle()) QTest::qWait(5); });
        auto* Surface = Host.CreateSurface(&Parent); Surface->resize(500, 300); Surface->show();
        QSignalSpy Ready(Surface, &QtViewerSurface::Ready), Failed(Surface, &QtViewerSurface::Failed);
        Surface->Open(Content("sample.bitmap"), hc::viewer::ViewerPurpose::QuickPreview);
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000); QCOMPARE(Failed.size(), 0); QCOMPARE(Ready.size(), 1);
        Surface->Close(); QTRY_VERIFY_WITH_TIMEOUT(Host.IsIdle(), 4000);
        Surface->Open(Content("sample.native"), hc::viewer::ViewerPurpose::FullViewer);
        QTRY_VERIFY_WITH_TIMEOUT(Surface->IsReady(), 5000); Surface->Execute("cancel"); QTRY_VERIFY_WITH_TIMEOUT(Host.IsIdle(), 4000);
    }
    void crashAndTimeout_data()
    { QTest::addColumn<QString>("Name"); QTest::newRow("crash") << "crash.native"; QTest::newRow("timeout") << "hang.native"; }
    void crashAndTimeout()
    {
        QFETCH(QString, Name); QTemporaryDir Package; WlxViewerOptions Options; QVERIFY(prepare(Package, Options));
        QWidget Parent; Parent.show();
        auto Started = Worker([&] { return PluginProcess::Start(Options.executable_, Options.manifestPath_, Options.approval_, Options.limits_); }); QVERIFY(Started.HasValue());
        auto Host = std::move(Started.Value()); const auto Cleanup = qScopeGuard([&] { Worker([&] { Host.reset(); }); });
        const auto Input = file(Package, Name); QElapsedTimer Timer; Timer.start();
        auto Result = Worker([&] { return Host->ViewerCall(WireKind::ViewerOpen, {{"parent", QString::number(Parent.winId(), 16)}, {"file", Input}, {"width", 100}, {"height", 100}, {"flags", 0}}); });
        QVERIFY(!Result.HasValue()); QVERIFY(Timer.elapsed() < 4000); QTRY_VERIFY_WITH_TIMEOUT(!Host->Snapshot().running_, 2000);
    }
};
QTEST_MAIN(DopusViewerTests)
#include "tstDopusViewer.moc"
