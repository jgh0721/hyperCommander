#include "UniqueLibs/qtPluginManagementPage.hpp"
#include "pluginHost/pluginApplicationRule.hpp"
#include "UniqueLibs/builtinPluginCatalog.hpp"
#include "UniqueLibs/archive/creatorRegistry.hpp"
#include "UniqueLibs/qtPluginHostSettings.hpp"
#include "pluginHost/manifestFile.hpp"
#include "UniqueLibs/tcPluginPackage.hpp"
#include <QtTest>
#include <QtWidgets>
#include <QtCore/private/qzipwriter_p.h>

using hc::plugins::PluginApplicationRule;
namespace {
QJsonObject rule(const QString& Mode, const QJsonValue& Value) {
    return {{"version", 1}, {"enabled", true}, {"mode", Mode}, {Mode == "extensions" ? "extensions" : "expression", Value}};
}
QTreeWidgetItem* external(QPluginManagementPage& Page) {
    auto* Tree = Page.findChild<QTreeWidget*>("pluginCatalog");
    for (int Index = 0; Index < Tree->topLevelItemCount(); ++Index) {
        auto* Group = Tree->topLevelItem(Index);
        for (int Row = 0; Row < Group->childCount(); ++Row) if (Group->child(Row)->data(0, Qt::UserRole + 1).toString() == "0:0") return Group->child(Row);
    }
    return nullptr;
}
QByteArray fixture(const QString& Name = "hc_wcx_fixture.wcx64") {
    QFile File(QCoreApplication::applicationDirPath() + '/' + Name);
    return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{};
}
bool package(const QString& Path, const QList<QPair<QString, QByteArray>>& Files) {
    QZipWriter Zip(Path);
    for (const auto& File : Files) Zip.addFile(File.first, File.second);
    Zip.close(); return Zip.status() == QZipWriter::NoError;
}
auto install(const QString& Source, const QString& Root) {
    return std::async(std::launch::async, [Source, Root] {
        hc::storage::OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        return hc::plugins::InstallTcPluginPackage(Source, Root, Context);
    }).get();
}
}
class PluginManagementTests : public QObject {
    Q_OBJECT
private slots:
    void wlxNativeDetectionLoadsAndRestores() {
        using namespace hc::plugins;
        QTemporaryDir Temp; const auto Root = QCoreApplication::applicationDirPath();
        const auto Binary = Temp.filePath("fixture.wlx64"); QVERIFY(QFile::copy(Root + "/hc_wlx_sdk_fixture.wlx64", Binary));
        const auto Inspected = std::async(std::launch::async, [&] { return InspectPluginBinary(Binary, PluginFormat::Wlx); }).get();
        QVERIFY(Inspected.HasValue());
        const auto Saved = std::async(std::launch::async, [&] { return SavePluginManifest(Inspected.Value(), Temp.path()); }).get();
        QVERIFY(Saved.HasValue());
        QFile Ini(Temp.filePath("lsplugin.ini")); QVERIFY(Ini.open(QIODevice::WriteOnly));
        Ini.write("[fixture]\r\nvalue=123\r\nmode=normal\r\nexpression=EXT=\"EXE\" | EXT=\"DLL\"\r\n"); Ini.close();
        const QJsonObject Record{{"path", Binary}, {"hostEnabled", true}, {"hostManifest", Saved.Value()}, {"hostReviewed", hc::settings::JsonObject(EncodeManifest(Inspected.Value().manifest_))}};
        hc::settings::Repository Settings(Temp.path()); QVERIFY(Settings.Put(hc::settings::Section::Plugins, QJsonObject{{"wlx", QJsonArray{Record}}}));
        QPluginManagementPage Page; Page.Load(Settings); Page.resize(1160, 760); Page.show();
        auto* Tree = Page.findChild<QTreeWidget*>("pluginCatalog");
        for (int I = 0; I < Tree->topLevelItemCount(); ++I)
            for (int J = 0; J < Tree->topLevelItem(I)->childCount(); ++J)
                if (Tree->topLevelItem(I)->child(J)->data(0, Qt::UserRole + 1) == "1:0") Tree->setCurrentItem(Tree->topLevelItem(I)->child(J));
        auto* Detect = Page.findChild<QPlainTextEdit*>("pluginNativeDetect");
        const QString Original = "EXT=\"EXE\" | EXT=\"DLL\"";
        QTRY_COMPARE_WITH_TIMEOUT(Detect->toPlainText(), Original, 10000);
        QVERIFY(!Page.Document()["wlx"].toArray()[0].toObject()["detectOverride"].toBool());
        Detect->setPlainText("EXT=\"TXT\""); QVERIFY(Page.Validate());
        QVERIFY(Page.Document()["wlx"].toArray()[0].toObject()["detectOverride"].toBool());
        Page.findChild<QPushButton*>("pluginReadNativeDetect")->click();
        QTRY_COMPARE_WITH_TIMEOUT(Detect->toPlainText(), Original, 10000);
        QVERIFY(!Page.Document()["wlx"].toArray()[0].toObject()["detectOverride"].toBool());
        const auto Capture = qEnvironmentVariable("HC_WLX_DETECT_CAPTURE");
        if (!Capture.isEmpty()) { QTest::qWait(100); QVERIFY(Page.grab().save(Capture)); }
    }
    void wlxDetectionCanBeEditedAndSaved() {
        const QJsonObject Record{{"path", "sample.wlx64"}, {"hostEnabled", false}, {"detect", "EXT=\"EXE\""}, {"nativeDetect", "EXT=\"EXE\""}, {"detectOverride", false}};
        QPluginManagementPage Page; Page.LoadDocument({{"wlx", QJsonArray{Record}}});
        auto* Tree = Page.findChild<QTreeWidget*>("pluginCatalog");
        for (int I = 0; I < Tree->topLevelItemCount(); ++I)
            for (int J = 0; J < Tree->topLevelItem(I)->childCount(); ++J)
                if (Tree->topLevelItem(I)->child(J)->data(0, Qt::UserRole + 1) == "1:0") Tree->setCurrentItem(Tree->topLevelItem(I)->child(J));
        auto* Detect = Page.findChild<QPlainTextEdit*>("pluginNativeDetect"); QVERIFY(!Detect->isReadOnly());
        QCOMPARE(Detect->toPlainText(), QString("EXT=\"EXE\""));
        Detect->setPlainText("EXT=\"EXE\" | EXT=\"DLL\""); QVERIFY(Page.Validate());
        auto Saved = Page.Document()["wlx"].toArray()[0].toObject();
        QCOMPARE(Saved["detect"].toString(), Detect->toPlainText()); QVERIFY(Saved["detectOverride"].toBool());
        QCOMPARE(Saved["nativeDetect"], Record["nativeDetect"]);
        Detect->clear(); QVERIFY(Page.Validate());
        Saved = Page.Document()["wlx"].toArray()[0].toObject(); QVERIFY(Saved["detectOverride"].toBool()); QVERIFY(Saved["detect"].toString().isEmpty());
    }
    void builtinArchiveExclusions() {
        const QJsonObject RuleObject{{"version", 1}, {"mode", "excludeExtensions"}, {"extensions", QJsonArray{"zip", "7z", "001"}}};
        const auto Rule = PluginApplicationRule::Parse(RuleObject); QVERIFY(Rule.HasValue());
        for (const auto* Name : {"a.zip", "a.z01", "a.7z.002", "a.002"}) QVERIFY(!hc::plugins::BuiltinArchiveAllowed(Rule.Value(), Name));
        QVERIFY(hc::plugins::BuiltinArchiveAllowed(Rule.Value(), "a.rar"));
        QTemporaryDir Temp; hc::settings::Repository Settings(Temp.path());
        auto Plugins = Settings.Read(hc::settings::Section::Plugins).toObject();
        Plugins["builtin"] = QJsonObject{{"builtin.sevenzip", QJsonObject{{"application", RuleObject}}}, {"builtin.iso", QJsonObject{{"application", QJsonObject{{"version", 1}, {"enabled", false}}}}}};
        QVERIFY(Settings.Put(hc::settings::Section::Plugins, Plugins));
        const auto Catalog = hc::archive::CreatorCatalog(Settings);
        for (const auto& Creator : Catalog)
            if (Creator.descriptor_.id_.starts_with("builtin.")) { QVERIFY(!Creator.available_); QVERIFY(Creator.descriptor_.formats_.empty()); }
    }
    void builtinExclusionsAndPersistence() {
        QPluginManagementPage Page; Page.LoadDocument({});
        auto* Tree = Page.findChild<QTreeWidget*>("pluginCatalog");
        QTreeWidgetItem* Viewer = nullptr; QTreeWidgetItem* Editor = nullptr;
        for (int I = 0; I < Tree->topLevelItem(0)->childCount(); ++I) {
            auto* Item = Tree->topLevelItem(0)->child(I);
            if (Item->data(0, Qt::UserRole + 1) == "builtin.hyperviewer") Viewer = Item;
            if (Item->data(0, Qt::UserRole + 1) == "builtin.hypereditor") Editor = Item;
        }
        QVERIFY(Viewer); QVERIFY(Editor); QVERIFY(Viewer->text(0).contains("EPUB"));
        Tree->setCurrentItem(Editor);
        auto* Extensions = Page.findChild<QListWidget*>("builtinApplicationExtensions");
        QVERIFY(Extensions->count() > 50);
        for (const auto* Name : {"epub", "cbz", "avif", "pdf"}) QVERIFY(!Extensions->findItems(Name, Qt::MatchExactly).isEmpty());
        Extensions->findItems("pdf", Qt::MatchExactly).front()->setCheckState(Qt::Unchecked);
        Page.findChild<QLineEdit*>("builtinExcludedExtensions")->setText("*.FOO; .tar.gz");
        QVERIFY(Page.Validate()); const auto Saved = Page.Document();
        const auto Rule = PluginApplicationRule::Parse(hc::plugins::BuiltinApplication(Saved, "builtin.hypereditor"));
        QVERIFY(Rule.HasValue()); QVERIFY(!Rule.Value().Matches("a.PDF").Value());
        QVERIFY(!Rule.Value().Matches("unknown.foo").Value()); QVERIFY(!Rule.Value().Matches("a.tar.gz").Value());
        QVERIFY(Rule.Value().Matches("unknown.newformat").Value()); QVERIFY(Rule.Value().Matches("README").Value());
        QVERIFY(Rule.Value().Matches("book.epub").Value());
        Tree->setCurrentItem(Viewer); QVERIFY(Page.findChild<QCheckBox*>("pluginApplicationEnabled")->isEnabled());
        Page.findChild<QCheckBox*>("pluginApplicationEnabled")->setChecked(false);
        const auto Disabled = Page.Document();
        QVERIFY(!PluginApplicationRule::Parse(hc::plugins::BuiltinApplication(Disabled, "builtin.hyperviewer")).Value().Matches("any.txt").Value());
        QTemporaryDir Temp; hc::settings::Repository Settings(Temp.path());
        QVERIFY(Settings.Put(hc::settings::Section::Plugins, Disabled)); QVERIFY(Settings.Commit());
        hc::settings::Repository Reload(Temp.path());
        QCOMPARE(Reload.Read(hc::settings::Section::Plugins).toObject()["builtin"], Disabled["builtin"]);
        const auto Capture = qEnvironmentVariable("HC_BUILTIN_PLUGIN_CAPTURE");
        if (!Capture.isEmpty()) { Page.resize(1160, 760); Page.show(); QVERIFY(QTest::qWaitForWindowExposed(&Page)); QTest::qWait(100); QVERIFY(Page.grab().save(Capture)); }
    }
    void builtinUnknownRulePreserved() {
        const QJsonObject Future{{"application", QJsonObject{{"version", 99}, {"mode", "future"}}}, {"opaque", 42}};
        QPluginManagementPage Page; Page.LoadDocument({{"builtin", QJsonObject{{"builtin.hyperviewer", Future}}}});
        auto* Tree = Page.findChild<QTreeWidget*>("pluginCatalog");
        for (int I = 0; I < Tree->topLevelItem(0)->childCount(); ++I)
            if (Tree->topLevelItem(0)->child(I)->data(0, Qt::UserRole + 1) == "builtin.hyperviewer") Tree->setCurrentItem(Tree->topLevelItem(0)->child(I));
        QVERIFY(!Page.findChild<QCheckBox*>("pluginApplicationEnabled")->isEnabled());
        QCOMPARE(Page.Document()["builtin"].toObject()["builtin.hyperviewer"].toObject(), Future);
    }
    void addTypesAreUnified() {
        QPluginManagementPage Page; Page.LoadDocument({}); Page.show(); bool Seen = false;
        QTimer Driver; Driver.setInterval(10);
        connect(&Driver, &QTimer::timeout, &Page, [&] {
            auto* Dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!Dialog || Dialog->objectName() != "pluginAddType") return;
            auto* Types = Dialog->findChild<QComboBox*>("pluginAddFormat");
            if (Types) {
                Seen = Types->count() == 3 && Types->itemText(0) == QStringLiteral("토탈 커맨더 플러그인")
                    && Types->itemData(0).toInt() == -1 && Types->itemData(1).toInt() == 2 && Types->itemData(2).toInt() == 4;
                const auto Capture = qEnvironmentVariable("HC_PLUGIN_UI_CAPTURE");
                if (!Capture.isEmpty()) Dialog->grab().save(QFileInfo(Capture).absolutePath() + "/plugin-add-types.png");
            }
            Dialog->reject(); Driver.stop();
        }); Driver.start(); Page.findChild<QPushButton*>("pluginAdd")->click(); QVERIFY(Seen);
    }
    void directTcTypes_data() {
        QTest::addColumn<QString>("Group");
        for (const auto& Group : {QString("wcx"), QString("wlx"), QString("wfx"), QString("wdx")}) QTest::newRow(qPrintable(Group)) << Group;
    }
    void directTcTypes() {
        QFETCH(QString, Group); QTemporaryDir Root;
        const auto Path = Root.filePath("sample." + Group + "64");
        QFile Binary(Path); QVERIFY(Binary.open(QIODevice::WriteOnly)); QVERIFY(Binary.write(fixture()) > 0); Binary.close();
        QPluginManagementPage Page(nullptr, Root.path()); Page.LoadDocument({{"future", "keep"}});
        QVERIFY(Page.AddTotalCommanderPlugin(Path, false));
        QCOMPARE(Page.Document()["future"].toString(), QString("keep"));
        QCOMPARE(Page.Document()[Group].toArray().size(), 1);
        QCOMPARE(Page.Document()[Group].toArray()[0].toObject()["path"].toString(), Path);
        if (Group == "wcx") Page.findChild<QLineEdit*>("pluginRegisteredExtensions")->setText("abc");
        QVERIFY(Page.AddTotalCommanderPlugin(Path, false)); QCOMPARE(Page.Document()[Group].toArray().size(), 1);
    }
    void zipMetadataInstallsAndPreservesExisting() {
        QTemporaryDir Root; const auto Path = Root.filePath("download.zip"); const auto Binary = fixture(); QVERIFY(!Binary.isEmpty());
        QVERIFY(package(Path, {{"PLUGINST.INF", "[PluginInstall]\r\nTYPE=WCX\r\nFILE=sample.wcx\r\nDEFAULTDIR=Sample\r\nDEFAULTEXTENSION=abc,def\r\n"},
            {"sample.wcx", "32-bit placeholder"}, {"sample.wcx64", Binary}, {"helper.dll", "dependency"}, {"language/ko.txt", "Korean"}, {"sample.ini", "original"}}));
        auto Installed = install(Path, Root.path()); QVERIFY2(Installed.HasValue(), Installed.HasValue() ? "" : Installed.Failure().code_.c_str());
        QCOMPARE(Installed.Value().group_, QString("wcx")); QCOMPARE(Installed.Value().extensions_, (QStringList{"abc", "def"}));
        QCOMPARE(Installed.Value().binary_, Root.filePath("Addons/WCX/Sample/sample.wcx64"));
        QVERIFY(!QFileInfo::exists(Root.filePath("Addons/WCX/Sample/PLUGINST.INF")));
        QFile Out(Installed.Value().binary_); QVERIFY(Out.open(QIODevice::ReadOnly)); QCOMPARE(Out.readAll(), Binary); Out.close();
        QVERIFY(QFileInfo::exists(Root.filePath("Addons/WCX/Sample/language/ko.txt")));
        QFile Config(Root.filePath("Addons/WCX/Sample/sample.ini")); QVERIFY(Config.open(QIODevice::WriteOnly)); Config.write("user settings"); Config.close();
        auto Again = install(Path, Root.path()); QVERIFY(Again.HasValue()); QCOMPARE(Again.Value().directory_, Root.filePath("Addons/WCX/Sample-2"));
        QVERIFY(Config.open(QIODevice::ReadOnly)); QCOMPARE(Config.readAll(), QByteArray("user settings")); Config.close();
        const auto Hidden = QDir(Root.filePath("Addons/WCX")).entryList({".install-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot); QVERIFY(Hidden.isEmpty());
    }
    void zipRegistersIntoSettingsDraft() {
        QTemporaryDir Root, Config; const auto Path = Root.filePath("viewer.zip");
        QVERIFY(package(Path, {{"pluginst.inf", "[plugininstall]\ntype=wlx\nfile=viewer.wlx\nfile64=x64/viewer.wlx64\ndefaultdir=Viewer\n"}, {"x64/viewer.wlx64", fixture("hc_wlx_fixture.wlx64")}}));
        hc::settings::Repository Settings(Config.path()); QVERIFY(Settings.Put(hc::settings::Section::Plugins, QJsonObject{{"future", true}}));
        const auto Before = Settings.Object();
        QPluginManagementPage Page(nullptr, Root.path()); Page.Load(Settings); QVERIFY(Page.AddTotalCommanderPlugin(Path, false));
        QCOMPARE(Settings.Object(), Before);
        const auto Items = Page.Document()["wlx"].toArray(); QCOMPARE(Items.size(), 1);
        QCOMPARE(Items[0].toObject()["path"].toString(), Root.filePath("Addons/WLX/Viewer/x64/viewer.wlx64"));
        QVERIFY(Items[0].toObject()["hostEnabled"].toBool()); QVERIFY(Items[0].toObject()["hostReviewed"].toObject().isEmpty());
        QVERIFY(Page.Save(Settings)); QCOMPARE(Settings.Read(hc::settings::Section::Plugins).toObject()["wlx"].toArray().size(), 1);
    }
    void zipWithoutMetadataAndWrappedPackage() {
        QTemporaryDir Root;
        const auto Plain = Root.filePath("plain.zip"); QVERIFY(package(Plain, {{"viewer.wlx64", fixture("hc_wlx_fixture.wlx64")}, {"readme.txt", "notes"}}));
        auto First = install(Plain, Root.path()); QVERIFY(First.HasValue()); QCOMPARE(First.Value().binary_, Root.filePath("Addons/WLX/plain/viewer.wlx64"));
        const auto Wrapped = Root.filePath("wrapped.zip");
        QVERIFY(package(Wrapped, {{"folder/pluginst.inf", "[plugininstall]\ntype=wdx\nfile=sample.wdx64\ndefaultdir=Content\n"}, {"folder/sample.wdx64", fixture()}}));
        auto Second = install(Wrapped, Root.path()); QVERIFY2(Second.HasValue(), Second.HasValue() ? "" : Second.Failure().code_.c_str());
        QCOMPARE(Second.Value().binary_, Root.filePath("Addons/WDX/Content/sample.wdx64"));
    }
    void unsafePackages_data() {
        QTest::addColumn<QString>("Member"); QTest::addColumn<QByteArray>("Metadata");
        const QByteArray Good("[plugininstall]\ntype=wcx\nfile=sample.wcx64\ndefaultdir=Sample\n");
        QTest::newRow("parent-traversal") << "../outside.txt" << Good;
        QTest::newRow("drive-path") << "C:/outside.txt" << Good;
        QTest::newRow("alternate-stream") << "data.txt:stream" << Good;
        QTest::newRow("device-name") << "CON.txt" << Good;
        QTest::newRow("trailing-dot") << "data." << Good;
        QTest::newRow("case-collision") << "SAMPLE.WCX64" << Good;
        QTest::newRow("unsafe-defaultdir") << "safe.txt" << QByteArray("[plugininstall]\ntype=wcx\nfile=sample.wcx64\ndefaultdir=../outside\n");
        QTest::newRow("unsupported-type") << "safe.txt" << QByteArray("[plugininstall]\ntype=prg\nfile=sample.wcx64\ndefaultdir=Sample\n");
        QTest::newRow("missing-binary") << "safe.txt" << QByteArray("[plugininstall]\ntype=wcx\nfile=absent.wcx64\ndefaultdir=Sample\n");
    }
    void unsafePackages() {
        QFETCH(QString, Member); QFETCH(QByteArray, Metadata); QTemporaryDir Root;
        const auto Path = Root.filePath("bad.zip"); QVERIFY(package(Path, {{"pluginst.inf", Metadata}, {"sample.wcx64", fixture()}, {Member, "payload"}}));
        auto Result = install(Path, Root.path()); QVERIFY(!Result.HasValue());
        QVERIFY(!QFileInfo::exists(Root.filePath("Addons/WCX/Sample"))); QVERIFY(!QFileInfo::exists(Root.filePath("outside.txt")));
    }
    void ambiguousAndCancelledPackages() {
        QTemporaryDir Root; const auto Path = Root.filePath("many.zip");
        QVERIFY(package(Path, {{"one.wcx64", fixture()}, {"two.wlx64", fixture("hc_wlx_fixture.wlx64")}}));
        auto Ambiguous = install(Path, Root.path()); QVERIFY(!Ambiguous.HasValue()); QCOMPARE(Ambiguous.Failure().code_, std::string("plugin-package-binary-ambiguous"));
        std::stop_source Stop; Stop.request_stop(); hc::storage::OperationContext Context; Context.stopToken_ = Stop.get_token();
        auto Cancelled = hc::plugins::InstallTcPluginPackage(Path, Root.path(), Context); QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, hc::storage::ErrorCategory::Cancelled);
        QVERIFY(!QFileInfo::exists(Root.filePath("Addons")));
    }
    void damagedPayloadLeavesNoInstalledFolder() {
        QTemporaryDir Root; const auto Path = Root.filePath("damaged.zip"); const auto Binary = fixture(); QVERIFY(Binary.size() > 1024);
        {
            QZipWriter Zip(Path); Zip.setCompressionPolicy(QZipWriter::NeverCompress);
            Zip.addFile("pluginst.inf", QByteArray("[plugininstall]\ntype=wcx\nfile=sample.wcx64\ndefaultdir=Sample\n"));
            Zip.addFile("sample.wcx64", Binary); Zip.close(); QCOMPARE(Zip.status(), QZipWriter::NoError);
        }
        QFile Zip(Path); QVERIFY(Zip.open(QIODevice::ReadWrite)); auto Bytes = Zip.readAll();
        const auto Offset = Bytes.indexOf(Binary.first(64)); QVERIFY(Offset >= 0); Bytes[Offset + 100] = char(Bytes[Offset + 100] ^ 1);
        QVERIFY(Zip.seek(0)); QCOMPARE(Zip.write(Bytes), Bytes.size()); Zip.close();
        auto Result = install(Path, Root.path()); QVERIFY(!Result.HasValue());
        const QDir Category(Root.filePath("Addons/WCX"));
        QVERIFY(Category.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    }
    void wcxUiGrantRequiresExplicitSelection_data()
    {
        QTest::addColumn<bool>("Allow"); QTest::newRow("preserve-old-grants") << false; QTest::newRow("explicit-ui-grant") << true;
    }
    void wcxUiGrantRequiresExplicitSelection()
    {
        using namespace hc::plugins; QFETCH(bool, Allow); QTemporaryDir Package, Config;
        const auto Binary = Package.filePath("fixture.wcx64"); QVERIFY(QFile::copy(QCoreApplication::applicationDirPath() + "/hc_wcx_fixture.wcx64", Binary));
        auto Inspected = InspectPluginBinary(Binary, PluginFormat::Wcx); QVERIFY(Inspected.HasValue()); Inspected.Value().manifest_.permissions_.erase(Permission::UserInterface);
        auto Manifest = SavePluginManifest(Inspected.Value(), Config.path()); QVERIFY(Manifest.HasValue());
        hc::settings::Repository Settings(Config.path());
        QJsonObject Record{{"path", Binary}, {"hostEnabled", true}, {"hostManifest", Manifest.Value()}, {"hostReviewed", hc::settings::JsonObject(EncodeManifest(Inspected.Value().manifest_))}};
        QVERIFY(Settings.Put(hc::settings::Section::Plugins, QJsonObject{{"wcx", QJsonArray{Record}}}));
        QPluginHostSettings Page(PluginFormat::Wcx); Page.Load(Settings); Page.show(); bool Seen = false;
        QTimer Response; Response.setInterval(10); connect(&Response, &QTimer::timeout, &Page, [&] {
            auto* Box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()); if (!Box || Box->objectName() != "hc.plugin.reviewApproval") return;
            auto* Check = Box->findChild<QCheckBox*>("hc.plugin.allowSettingsUi"); QVERIFY(Check); QVERIFY(!Check->isChecked()); Check->setChecked(Allow); Seen = true; Box->done(QMessageBox::Yes); Response.stop();
        }); Response.start(); Page.ReviewPlugin(0); QTRY_VERIFY_WITH_TIMEOUT(Seen, 10000); QTRY_VERIFY_WITH_TIMEOUT(Page.findChild<QPushButton*>("wcxHostReview")->isEnabled(), 10000); QTRY_VERIFY_WITH_TIMEOUT(Page.VerificationState(0).contains("확인 완료"), 10000);
        QJsonObject Out; Page.SavePlugins(Out); auto Reviewed = ParseManifest(hc::settings::JsonBytes(Out["wcx"].toArray()[0].toObject()["hostReviewed"])); QVERIFY(Reviewed.HasValue()); QCOMPARE(Reviewed.Value().permissions_.contains(Permission::UserInterface), Allow);
    }
    void approvalAndChangedPackage()
    {
        using namespace hc::plugins;
        QTemporaryDir Package, Config; QVERIFY(Package.isValid()); QVERIFY(Config.isValid());
        const auto Binary = Package.filePath("fixture.wlx64");
        const auto Actual = qEnvironmentVariable("HC_PLUGIN_REVIEW_BINARY");
        QVERIFY(QFile::copy(Actual.isEmpty() ? QCoreApplication::applicationDirPath() + "/hc_wlx_fixture.wlx64" : Actual, Binary));
        hc::settings::Repository Settings(Config.path());
        QVERIFY(Settings.Put(hc::settings::Section::Plugins, QJsonObject{{"wlx", QJsonArray{QJsonObject{{"path", Binary}, {"hostEnabled", true}}}}}));
        QPluginHostSettings Page(PluginFormat::Wlx); Page.Load(Settings); Page.resize(800, 250); Page.show();
        int Answers = 0;
        bool Accept = false;
        QTimer Response; Response.setInterval(10);
        connect(&Response, &QTimer::timeout, &Page, [&] {
            auto* Box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (Box && Box->objectName() == "hc.plugin.reviewApproval") {
                ++Answers;
                const auto Output = qEnvironmentVariable("HC_PLUGIN_UI_CAPTURE");
                if (!Output.isEmpty()) Box->grab().save(QFileInfo(Output).absolutePath() + QString("/approval-%1.png").arg(Answers));
                Box->done(Accept ? QMessageBox::Yes : QMessageBox::No);
            }
        }); Response.start();
        Page.ReviewPlugin(0); QTRY_COMPARE_WITH_TIMEOUT(Answers, 1, 10000);
        QJsonObject Rejected; Page.SavePlugins(Rejected); QVERIFY(Rejected["wlx"].toArray()[0].toObject()["hostReviewed"].toObject().isEmpty());
        Accept = true; Page.ReviewPlugin(0); QTRY_COMPARE_WITH_TIMEOUT(Answers, 2, 10000);
        QTRY_VERIFY(Page.VerificationState(0).contains("확인 완료"));
        QJsonObject Approved; Page.SavePlugins(Approved);
        const auto Before = Approved["wlx"].toArray()[0].toObject(); QVERIFY(!Before["hostManifest"].toString().isEmpty());
        QVERIFY(QFileInfo(Before["hostManifest"].toString()).absolutePath().startsWith(Config.path()));
        { QFile File(Binary); QVERIFY(File.open(QIODevice::Append)); QVERIFY(File.write("version-two") > 0); }
        QTRY_VERIFY_WITH_TIMEOUT(Page.VerificationState(0).contains("변경됨"), 10000);
        Accept = false; Page.ReviewPlugin(0); QTRY_COMPARE_WITH_TIMEOUT(Answers, 3, 10000);
        QJsonObject Cancelled; Page.SavePlugins(Cancelled); QCOMPARE(Cancelled["wlx"].toArray()[0].toObject()["hostReviewed"], Before["hostReviewed"]);
        Accept = true; Page.ReviewPlugin(0); QTRY_COMPARE_WITH_TIMEOUT(Answers, 4, 10000);
        QTRY_VERIFY(Page.VerificationState(0).contains("확인 완료"));
        QJsonObject Updated; Page.SavePlugins(Updated); QVERIFY(Updated["wlx"].toArray()[0].toObject()["hostReviewed"] != Before["hostReviewed"]);
        QPluginManagementPage Management; Management.LoadDocument(Updated); QVERIFY(Management.SelectPlugin(Binary));
        QTRY_VERIFY(Management.findChild<QLabel*>("pluginStatus")->text().contains("확인 완료"));
        const auto Output = qEnvironmentVariable("HC_PLUGIN_UI_CAPTURE");
        if (!Output.isEmpty()) { Management.resize(1100, 750); Management.show(); QTest::qWait(100); QVERIFY(Management.grab().save(QFileInfo(Output).absolutePath() + "/approved-package.png")); }
        auto Copies = Updated["wlx"].toArray(); Copies.append(Copies[0]); Updated["wlx"] = Copies;
        Page.LoadPlugins(Updated); Page.SelectPlugin(1);
        QTRY_VERIFY(Page.VerificationState(1).contains("확인 완료"));
    }
    void extensionSubset() {
        auto Rule = PluginApplicationRule::Parse(rule("extensions", QJsonArray{"rar", "lha"})); QVERIFY(Rule.HasValue());
        QVERIFY(!Rule.Value().Matches("sample.7z").Value()); QVERIFY(Rule.Value().Matches("SAMPLE.RAR").Value()); QVERIFY(Rule.Value().Matches("sample.lha").Value());
        auto Empty = PluginApplicationRule::Parse(rule("extensions", QJsonArray{})); QVERIFY(Empty.HasValue()); QVERIFY(!Empty.Value().Matches("sample.rar").Value());
        QVERIFY(PluginApplicationRule::Parse(QJsonValue{}).Value().Matches("anything.bin").Value());
    }
    void viewerChoicesRetainRegisteredExtensions() {
        QTemporaryDir Temp; QVERIFY(Temp.isValid()); hc::settings::Repository Settings(Temp.path());
        const QJsonObject Record{{"path", "C:/plugins/PEViewer.wlx64"}, {"hostEnabled", true},
            {"application", QJsonObject{{"version", 1}, {"enabled", true}, {"mode", "all"}}},
            {"extensions", QJsonArray{"exe", "dll"}}};
        QVERIFY(Settings.SetPlugins("wlx", QJsonArray{Record}));
        const auto Choices = QPluginHostSettings::ViewerChoices(Settings);
        QCOMPARE(Choices.size(), std::size_t(1));
        QCOMPARE(Choices.front().extensions_, (QStringList{"exe", "dll"}));
    }
    void expressionsAndMissingContent() {
        auto Tc = PluginApplicationRule::Parse(rule("tc", "EXT=\"RAR\" | EXT=\"LHA\"")); QVERIFY(Tc.HasValue()); QVERIFY(Tc.Value().NameOnly());
        QVERIFY(Tc.Value().Matches("sample.RaR").Value()); QVERIFY(!Tc.Value().Matches("sample.7z").Value());
        auto Header = PluginApplicationRule::Parse(rule("tc", "[0]=82 & SIZE>2")); QVERIFY(Header.HasValue()); QVERIFY(!Header.Value().NameOnly());
        QVERIFY(!Header.Value().Matches("sample.bin").HasValue()); QVERIFY(Header.Value().Matches("sample.bin", 3, QByteArray("RAR")).Value());
        auto Pattern = PluginApplicationRule::Parse(rule("pattern", "backup-*.(rar|lha)")); QVERIFY(Pattern.HasValue());
        QVERIFY(Pattern.Value().Matches("backup-1.RAR").Value()); QVERIFY(!Pattern.Value().Matches("other.rar").Value());
        QVERIFY(!PluginApplicationRule::Parse(rule("pattern", "*.(rar|)")).HasValue());
        QVERIFY(!PluginApplicationRule::Parse(QJsonObject{{"version", 2}, {"mode", "all"}}).HasValue());
        QVERIFY(!PluginApplicationRule::Parse(QJsonObject{{"version", 1}, {"mode", 7}}).HasValue());
    }
    void builtinsAndUnifiedDocuments() {
        QPluginManagementPage Page(nullptr, "X:/missing-product"); Page.LoadDocument({});
        auto* Tree = Page.findChild<QTreeWidget*>("pluginCatalog"); QCOMPARE(Tree->topLevelItemCount(), 1); QCOMPARE(Tree->topLevelItem(0)->childCount(), 6);
        auto* Roles = Page.findChild<QListWidget*>("pluginRoles"); QCOMPARE(Roles->count(), 5); QCOMPARE(Roles->item(2)->text(), QString("문서"));
        Roles->setCurrentRow(2); QCOMPARE(Tree->topLevelItem(0)->childCount(), 3);
        QStringList Actions; for (int Row = 0; Row < 3; ++Row) Actions.append(Tree->topLevelItem(0)->child(Row)->text(1));
        QVERIFY(Actions.contains("보기")); QVERIFY(Actions.contains("편집")); QVERIFY(Actions.contains("보기 · 편집"));
    }
    void draftRoundtripAndRepair() {
        const QJsonObject Record{{"path", "sample.wcx64"}, {"extensions", QJsonArray{"7z", "rar", "lha"}}, {"hostEnabled", true}, {"futureField", 42}};
        const QJsonObject Original{{"wcx", QJsonArray{Record}}, {"unknownGroup", "retained"}};
        QPluginManagementPage Page; Page.LoadDocument(Original); auto* Item = external(Page); QVERIFY(Item);
        Page.findChild<QTreeWidget*>("pluginCatalog")->setCurrentItem(Item);
        Page.findChild<QComboBox*>("pluginRuleMode")->setCurrentIndex(1);
        auto* Extensions = Page.findChild<QListWidget*>("pluginApplicationExtensions"); QCOMPARE(Extensions->count(), 3); Extensions->item(0)->setCheckState(Qt::Unchecked);
        auto Saved = Page.Document(); auto Updated = Saved["wcx"].toArray()[0].toObject();
        QCOMPARE(Updated["extensions"], Record["extensions"]); QCOMPARE(Updated["futureField"].toInt(), 42); QCOMPARE(Saved["unknownGroup"].toString(), QString("retained"));
        auto Applied = PluginApplicationRule::Parse(Updated["application"]); QVERIFY(Applied.HasValue()); QVERIFY(!Applied.Value().Matches("a.7z").Value()); QVERIFY(Applied.Value().Matches("a.rar").Value());
        Page.findChild<QComboBox*>("pluginRuleMode")->setCurrentIndex(2); auto* Expression = Page.findChild<QPlainTextEdit*>("pluginApplicationExpression"); Expression->setPlainText("EXT=");
        Page.Document(); Expression->setPlainText("EXT=\"LHA\""); QVERIFY(Page.Validate());
        QVERIFY(PluginApplicationRule::Parse(Page.Document()["wcx"].toArray()[0].toObject()["application"]).Value().Matches("a.lha").Value());
        Page.LoadDocument(Original); QCOMPARE(Page.Document()["wcx"].toArray()[0].toObject()["futureField"].toInt(), 42);
    }
    void futureRulePreserved() {
        const QJsonObject Future{{"version", 99}, {"opaque", "retain"}};
        QPluginManagementPage Page; Page.LoadDocument({{"wcx", QJsonArray{QJsonObject{{"path", "x.wcx64"}, {"extensions", QJsonArray{"rar"}}, {"application", Future}}}}});
        Page.findChild<QTreeWidget*>("pluginCatalog")->setCurrentItem(external(Page));
        QVERIFY(!Page.findChild<QComboBox*>("pluginRuleMode")->isEnabled()); QVERIFY(Page.Validate());
        QCOMPARE(Page.Document()["wcx"].toArray()[0].toObject()["application"].toObject(), Future);
    }
    void multipleRegistrationsAndApprovalDraft() {
        QJsonArray Items;
        for (int Index = 0; Index < 3; ++Index) Items.append(QJsonObject{{"path", QString("fixture%1.wcx64").arg(Index)}, {"extensions", QJsonArray{"rar"}}, {"hostEnabled", true}, {"unknown", Index}});
        QPluginManagementPage Page; Page.LoadDocument({{"wcx", Items}}); Page.findChild<QTreeWidget*>("pluginCatalog")->setCurrentItem(external(Page));
        Page.findChild<QPushButton*>("wcxHostRevoke")->click(); const auto Saved = Page.Document()["wcx"].toArray(); QCOMPARE(Saved.size(), 3);
        for (int Index = 0; Index < 3; ++Index) { QCOMPARE(Saved[Index].toObject()["unknown"].toInt(), Index); QVERIFY(Saved[Index].toObject()["hostEnabled"].toBool()); }
    }
    void captureDesign() {
        const auto Output = qEnvironmentVariable("HC_PLUGIN_UI_CAPTURE"); if (Output.isEmpty()) return;
        QPluginManagementPage Page(nullptr, "D:/Utils/HyperCommander"); Page.LoadDocument({}); Page.resize(1160, 760); Page.show(); QVERIFY(QTest::qWaitForWindowExposed(&Page));
        QTest::qWait(100); QVERIFY(Page.grab().save(Output));
        Page.LoadDocument({{"wcx", QJsonArray{QJsonObject{{"path", "example.wcx64"}, {"extensions", QJsonArray{"7z", "rar", "lha"}}, {"hostEnabled", true}, {"application", rule("tc", "EXT=\"RAR\" | EXT=\"LHA\"")}}}}});
        Page.findChild<QTreeWidget*>("pluginCatalog")->setCurrentItem(external(Page)); QTest::qWait(50);
        QVERIFY(Page.grab().save(QFileInfo(Output).absolutePath() + "/page-external-condition.png"));
    }
};
QTEST_MAIN(PluginManagementTests)
#include "tstPluginManagement.moc"
