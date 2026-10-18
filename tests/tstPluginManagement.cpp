#include "UniqueLibs/qtPluginManagementPage.hpp"
#include "pluginHost/pluginApplicationRule.hpp"
#include "UniqueLibs/qtPluginHostSettings.hpp"
#include "pluginHost/manifestFile.hpp"
#include <QtTest>
#include <QtWidgets>

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
}
class PluginManagementTests : public QObject {
    Q_OBJECT
private slots:
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
