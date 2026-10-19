#include <QtTest>
#include <QTemporaryDir>
#include <QLineEdit>
#include "UniqueLibs/renameSelection.hpp"
#include "UniqueLibs/navigationError.hpp"
#include "UniqueLibs/userMenu.hpp"
#include "UniqueLibs/commandUiWidgets.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/settings/fileColorRules.hpp"

using namespace hc::ui;
namespace {
QByteArray MenuBytes(const QString& Command = "hc.file.copy", const QString& Label = "복사")
{
    const QJsonArray Items{QJsonObject{{"id", "copy"}, {"command", Command}, {"label", Label}},
        QJsonObject{{"type", "separator"}}, QJsonObject{{"id", "tools"}, {"section", "automation"}}};
    const QJsonArray Menus{QJsonObject{{"id", "file"}, {"label", "파일(&F)"}, {"items", Items}}};
    return QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"menus", Menus}}).toJson();
}
bool Write(const QString& Path, const QByteArray& Bytes)
{
    QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size();
}
}
class CommandUiTests : public QObject {
    Q_OBJECT
private slots:
    void navigationErrorDescriptions()
    {
        using namespace hc::storage;
        for (const auto& Error : {Error{ErrorCategory::AccessDenied, "win32:5"},
            Error{ErrorCategory::NotFound, "win32:3"},
            Error{ErrorCategory::Unavailable, "provider-session-closed"}}) {
            const auto Text = DescribeNavigationError(Error, "D:/folder");
            QVERIFY(!Text.summary_.isEmpty()); QVERIFY(!Text.recovery_.isEmpty());
            QVERIFY(!Text.summary_.contains(QString::fromStdString(Error.code_)));
            QVERIFY(!Text.recovery_.contains(QString::fromStdString(Error.code_)));
            QVERIFY(Text.details_.contains(QString::fromStdString(Error.code_)));
            QVERIFY(Text.details_.contains("D:\\folder"));
        }
        for (const auto& Path : {"\\\\tsclient", "\\\\TSCLIENT\\D", "//tsclient/D/folder"}) {
            const auto Text = DescribeNavigationError({ErrorCategory::Unavailable, "win32:1707"}, Path);
            QVERIFY(Text.summary_.contains(QStringLiteral("원격 데스크톱")));
            QVERIFY(Text.recovery_.contains(QStringLiteral("드라이브")));
            QVERIFY(Text.details_.contains("win32:1707"));
            QVERIFY(Text.details_.contains(QStringLiteral("Windows 오류:")));
            QVERIFY(!Text.summary_.contains("1707")); QVERIFY(!Text.recovery_.contains("1707"));
        }
        const auto Missing = DescribeNavigationError({ErrorCategory::Unavailable, "rdp-drives-unavailable"}, "\\\\tsclient");
        QVERIFY(Missing.summary_.contains(QStringLiteral("원격 데스크톱")));
        QVERIFY(!Missing.recovery_.contains("rdp-drives-unavailable"));
    }
    void driveRootCommandsAndShortcuts()
    {
        const auto Descriptors = hc::defaults::BusCommands();
        for (const auto& Preset : {"legacy", "prototype"}) {
            const QJsonObject General{{"shortcutPreset", Preset}};
            const auto Keys = EffectiveShortcuts(General, {});
            for (int Offset = 0; Offset < 26; ++Offset) {
                const auto Letter = QChar::fromLatin1(char('A' + Offset));
                const auto Id = QString("hc.navigate.gotoDrive%1").arg(Letter);
                const QKeyCombination Key(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key(Qt::Key_A + Offset));
                QCOMPARE(QString(FindCommand(Id).Name), Id);
                QCOMPARE(CommandId(Id.toLower()), Id);
                QCOMPARE(CommandId(Keys.value(Key)), Id);
                QCOMPARE(std::count_if(Descriptors.begin(), Descriptors.end(), [&](const auto& Descriptor) {
                    return Descriptor.id_ == Id.toStdString() && Descriptor.parameters_.empty();
                }), 1);
                int Bindings = 0;
                for (const auto& Command : hc::defaults::Commands())
                    if (Command.Default == Key) ++Bindings;
                QCOMPARE(Bindings, 1);
                QString Invoked;
                const QKeyEvent Event(QEvent::KeyPress, Key.key(), Key.keyboardModifiers());
                QVERIFY(InvokeShortcut(Event, Keys, [&](const QString& Command) { Invoked = Command; return true; }));
                QCOMPARE(Invoked, Id);
                const auto KeyText = QKeySequence(Key).toString(QKeySequence::PortableText);
                QCOMPARE(CommandId(EffectiveShortcuts(General, {{KeyText, "hc.jobs.show"}}).value(Key)), QString("hc.jobs.show"));
                QVERIFY(!EffectiveShortcuts(General, {{KeyText, QJsonValue::Null}}).contains(Key));
            }
        }
        QVERIFY(EffectiveShortcuts({{"useBuiltInShortcuts", false}}, {}).isEmpty());
    }
    void shellMenuDefaults()
    {
        QVERIFY(!FindCommand("hc.file.shellMenu").Name.isEmpty());
        for (const auto& Preset : {QString("legacy"), QString("prototype")}) {
            const QJsonObject General{{"shortcutPreset", Preset}};
            const auto Keys = EffectiveShortcuts(General, {});
            const QKeyCombination Return(Qt::ShiftModifier, Qt::Key_Return);
            const QKeyCombination Enter(Qt::ShiftModifier | Qt::KeypadModifier, Qt::Key_Enter);
            QCOMPARE(CommandId(Keys.value(Return)), QString("hc.file.shellMenu"));
            QCOMPARE(CommandId(Keys.value(Enter)), QString("hc.file.shellMenu"));
            const auto Override = EffectiveShortcuts(General, {{"Shift+Return", "hc.file.copy"}});
            QCOMPARE(CommandId(Override.value(Return)), QString("hc.file.copy"));
            QVERIFY(!EffectiveShortcuts(General, {{"Shift+Return", QJsonValue::Null}}).contains(Return));
        }
    }
    void renameSelectionAndDefaults()
    {
        for (const auto& Preset : {QString("legacy"), QString("prototype")}) {
            const auto Shortcuts = EffectiveShortcuts({{"shortcutPreset", Preset}}, {});
            QCOMPARE(CommandId(Shortcuts.value(QKeyCombination(Qt::Key_F2))), QString("hc.file.rename"));
        }
        QCOMPARE(hc::defaults::Options()["renameInDialog"].toBool(), true);
        QCOMPARE(hc::defaults::Options()["renameSelectExtension"].toBool(), false);
        QLineEdit Input;
        const QList<QPair<QString, QString>> Names{{"report.tar.gz", "report.tar"}, {"이름.자료.txt", "이름.자료"}, {"README", "README"}, {".gitignore", ".gitignore"}};
        for (const auto& [Name, Stem] : Names) {
            Input.setText(Name); SelectRenameText(&Input, true); QCOMPARE(Input.selectedText(), Name);
            SelectRenameText(&Input, false);
            QCOMPARE(Input.selectedText(), Stem);
        }
    }
    void productCatalogAndPalettes()
    {
        const auto Aliases = hc::defaults::CommandAliases(); QSet<QString> Ids;
        for (const auto& Command : hc::defaults::Commands()) {
            const auto Id = CommandId(Command.Name); QVERIFY(Id.startsWith("hc."));
            QVERIFY(!FindCommand(Id).Name.isEmpty()); Ids.insert(Id);
            QVERIFY(QString(Command.Name).startsWith("hc."));
            QCOMPARE(QString(Command.Name), Id);
        }
        for (auto It = Aliases.cbegin(); It != Aliases.cend(); ++It) QVERIFY(Ids.contains(It.value()));
        QVERIFY(Ids.contains("hc.app.exit")); QVERIFY(Ids.contains("hc.iso.create"));
        const auto Extra = EffectiveShortcuts({}, {{"Alt+X", "hc.app.exit"}});
        QCOMPARE(CommandId(Extra.value(QKeyCombination(Qt::AltModifier, Qt::Key_X))), QString("hc.app.exit"));
        QSet<QString> BusIds;
        for (const auto& Command : hc::defaults::BusCommands()) {
            const auto Id = QString::fromStdString(Command.id_); QVERIFY(!BusIds.contains(Id)); BusIds.insert(Id);
        }
        QVERIFY(BusIds.contains("hc.file.copy")); QVERIFY(BusIds.contains("hc.viewer.close"));
        QVERIFY(hc::settings::FileColorRules::Validate(hc::defaults::FileGroups()).isEmpty());
        const QJsonObject Examples{{"version", 2}, {"mode", "first"}, {"groups", hc::defaults::ExampleFileGroups()}};
        QVERIFY2(hc::settings::FileColorRules::Validate(Examples).isEmpty(), qPrintable(hc::settings::FileColorRules::Validate(Examples)));
        hc::settings::FileColorRules Rules; Rules.Load(Examples);
        const auto Light = Rules.Evaluate({"notes.txt", 0, 12, false});
        const auto Dark = Rules.Evaluate({"notes.txt", 0, 12, true});
        QVERIFY(Light.matched_ && Dark.matched_); QVERIFY(Light.foreground_.isValid() && Dark.foreground_.isValid());
        QVERIFY(Light.foreground_ != Dark.foreground_);
    }
    void shortFunctionCaptionsKeepCommandIdentity()
    {
        const auto Ids = FunctionCommands({});
        const QStringList Expected{"이름 변경", "보기", "편집", "복사", "이동", "새 폴더", "삭제", "새 파일", "다중 이름 변경"};
        for (int Slot = 0; Slot < Ids.size(); ++Slot) QCOMPARE(FunctionCaption({}, Slot, Ids[Slot], "full label"), Expected[Slot]);
        QCOMPARE(FunctionCaption({}, 0, "hc.navigate.refresh", "새로 고침"), QString("새로 고침"));
        const QJsonObject Custom{{"functionCaptions", QJsonArray{"내 문구", "", 42}}};
        QCOMPARE(FunctionCaption(Custom, 0, Ids[0], "full"), QString("내 문구"));
        QCOMPARE(FunctionCaption(Custom, 1, Ids[1], "full"), QString("보기"));
        QCOMPARE(FunctionCaption(Custom, 2, Ids[2], "full"), QString("편집"));
        QAction Command("내장 뷰어로 열기"); Command.setProperty("hc.baseLabel", "내장 뷰어로 열기");
        fm::ui::FunctionKeyBar Bar; auto* Button = Bar.addKey({}, "보기"); BindFunctionButton(Button, &Command);
        Command.setProperty("hc.keyLabel", "Ctrl+Alt+V"); Command.setToolTip("내장 뷰어로 열기 (Ctrl+Alt+V)");
        QCOMPARE(Button->text(), QString("보기")); QCOMPARE(Button->keys(), QString("Ctrl+Alt+V"));
        QVERIFY(Button->accessibleName().contains("내장 뷰어로 열기")); QVERIFY(Button->accessibleName().contains("Ctrl+Alt+V"));
        SetFunctionCaption(Button, &Command, "내 보기"); QCOMPARE(Button->text(), QString("내 보기"));
        QVERIFY(Button->accessibleName().contains("내장 뷰어로 열기"));
        int Runs = 0; connect(&Command, &QAction::triggered, &Bar, [&] { ++Runs; }); Button->click(); QCOMPARE(Runs, 1);
        Command.setEnabled(false); QVERIFY(!Button->isEnabled()); QCOMPARE(Button->text(), QString("내 보기"));
    }
    void presetsAndAliases()
    {
        QTemporaryDir NewDirectory, OldDirectory;
        hc::settings::Repository New(NewDirectory.path());
        QVERIFY(!New.Editing()); QVERIFY(!New.Exists());
        const auto General = New.Read(hc::settings::Section::General).toObject();
        QCOMPARE(General["shortcutPreset"].toString(), "prototype");
        const auto Values = EffectiveShortcuts(General, {});
        QCOMPARE(CommandId(Values.value(QKeyCombination(Qt::Key_F5))), "hc.file.copy");
        QCOMPARE(CommandId(Values.value(QKeyCombination(Qt::Key_F6))), "hc.file.move");
        QCOMPARE(CommandId(Values.value(QKeyCombination(Qt::Key_F7))), "hc.file.mkdir");
        QCOMPARE(CommandId(Values.value(QKeyCombination(Qt::Key_F8))), "hc.file.delete");
        QCOMPARE(CommandKeys(Values, "hc.file.batchRename").first().toString(QKeySequence::PortableText), "Ctrl+M");
        auto OldDefaults = hc::settings::Defaults(); auto OldGeneral = OldDefaults["general"].toObject();
        OldGeneral.remove("shortcutPreset"); OldDefaults["general"] = OldGeneral;
        QVERIFY(Write(QDir(OldDirectory.path()).filePath("settings.json"), QJsonDocument(OldDefaults).toJson()));
        hc::settings::Repository Old(OldDirectory.path());
        QCOMPARE(CommandId(EffectiveShortcuts(Old.Read(hc::settings::Section::General).toObject(), {}).value(QKeyCombination(Qt::Key_F5))), "hc.navigate.refresh");
        const QJsonObject Overrides{{"F5", "cm_RereadSource"}, {"Ctrl+Alt+C", "hc.file.copy"}, {"F8", QJsonValue::Null}};
        const auto Custom = EffectiveShortcuts(General, Overrides);
        QCOMPARE(CommandId(Custom.value(QKeyCombination(Qt::Key_F5))), "hc.navigate.refresh");
        QCOMPARE(CommandKeys(Custom, "cm_CopyOtherPanel").first().toString(QKeySequence::PortableText), "Ctrl+Alt+C");
        QVERIFY(!Custom.contains(QKeyCombination(Qt::Key_F8)));
        QCOMPARE(FindCommand("HC.FILE.COPY").Name, QLatin1String("hc.file.copy"));
        QCOMPARE(CommandId("cm_MultiRenameFiles"), "hc.file.batchRename");
    }
    void storedCommandIdsMigrateAndPersist()
    {
        QTemporaryDir Directory;
        const auto Path = QDir(Directory.path()).filePath("settings.json");
        auto Document = hc::settings::Defaults();
        auto General = Document["general"].toObject();
        auto Functions = QJsonArray::fromStringList(hc::defaults::FunctionCommands());
        Functions[0] = "cm_RenameSingleFile"; Functions[1] = "CM_LIST";
        General["functionCommands"] = Functions; General["functionCaptions"] = QJsonArray{"내 이름 변경"};
        General["shortcutPreset"] = "legacy"; Document["general"] = General;
        QJsonObject Shortcuts{{"F5", "cm_RereadSource"}, {"F8", QJsonValue::Null}, {"Alt+X", "extension.custom"}};
        const auto Aliases = hc::defaults::CommandAliases();
        for (auto It = Aliases.cbegin(); It != Aliases.cend(); ++It) Shortcuts["stored." + It.key()] = It.key();
        Document["shortcuts"] = Shortcuts;
        const auto Original = QJsonDocument(Document).toJson();
        QVERIFY(Write(Path, Original));
        hc::settings::Repository Settings(Directory.path());
        const auto Migrated = Settings.Read(hc::settings::Section::Shortcuts).toObject();
        for (auto It = Aliases.cbegin(); It != Aliases.cend(); ++It) {
            QCOMPARE(Migrated["stored." + It.key()].toString(), It.value());
            QCOMPARE(QString(FindCommand(It.key()).Name), It.value());
        }
        QCOMPARE(Migrated["F5"].toString(), QString("hc.navigate.refresh"));
        QVERIFY(Migrated["F8"].isNull()); QCOMPARE(Migrated["Alt+X"].toString(), QString("extension.custom"));
        const auto NewGeneral = Settings.Read(hc::settings::Section::General).toObject();
        QCOMPARE(NewGeneral["functionCommands"].toArray()[0].toString(), QString("hc.file.rename"));
        QCOMPARE(NewGeneral["functionCommands"].toArray()[1].toString(), QString("hc.viewer.open"));
        QCOMPARE(NewGeneral["functionCaptions"], General["functionCaptions"]);
        QCOMPARE(NewGeneral["shortcutPreset"].toString(), QString("legacy"));
        QFile File(Path); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), Original); File.close();
        QVERIFY(Settings.Commit());
        // The legacy settings.json is migrated into HyperCommander.json and kept unchanged.
        QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), Original); File.close();
        File.setFileName(Settings.FileName()); QVERIFY(File.open(QIODevice::ReadOnly));
        const auto Saved = QJsonDocument::fromJson(File.readAll()).object(); File.close();
        QCOMPARE(Saved["shortcuts"].toObject(), Migrated);
        QVERIFY(Settings.Put(hc::settings::Section::Shortcuts, QJsonObject{{"F5", "cm_RereadSource"}, {"F8", QJsonValue::Null}}));
        QCOMPARE(Settings.Read(hc::settings::Section::Shortcuts).toObject()["F5"].toString(), QString("hc.navigate.refresh"));
        Settings.Reload();
        QCOMPARE(Settings.Read(hc::settings::Section::Shortcuts).toObject()["F5"].toString(), QString("hc.navigate.refresh"));
        const auto Keys = EffectiveShortcuts(NewGeneral, Migrated);
        QCOMPARE(QString(Keys.value(QKeyCombination(Qt::Key_F5))), QString("hc.navigate.refresh"));
        QVERIFY(!Keys.contains(QKeyCombination(Qt::Key_F8)));
        for (auto It = Keys.cbegin(); It != Keys.cend(); ++It) QVERIFY(QString(It.value()).startsWith("hc."));
    }
    void archiveDefaultsAndOverrides()
    {
        QCOMPARE(CommandId("cm_PackFiles"), QString("hc.archive.create")); QCOMPARE(CommandId("cm_UnpackFiles"), QString("hc.archive.extract"));
        QCOMPARE(CommandId("cm_EnterFile"), QString("hc.navigate.enterFile"));
        for (const auto& Preset : {"legacy", "prototype"}) {
            const auto Keys = EffectiveShortcuts({{"shortcutPreset", Preset}}, {});
            QCOMPARE(CommandId(Keys.value(QKeyCombination(Qt::ControlModifier,Qt::Key_A))), QString("hc.archive.create"));
            QCOMPARE(CommandId(Keys.value(QKeyCombination(Qt::ControlModifier,Qt::Key_X))), QString("hc.archive.extract"));
            QCOMPARE(CommandId(Keys.value(QKeyCombination(Qt::ControlModifier,Qt::Key_PageDown))), QString("hc.navigate.enterFile"));
        }
        const auto Custom = EffectiveShortcuts({}, {{"Ctrl+A", "hc.file.copy"}, {"Ctrl+X", QJsonValue::Null}, {"Alt+X", "hc.archive.extract"}});
        QCOMPARE(CommandId(Custom.value(QKeyCombination(Qt::ControlModifier, Qt::Key_A))), QString("hc.file.copy"));
        QVERIFY(!Custom.contains(QKeyCombination(Qt::ControlModifier, Qt::Key_X)));
        QCOMPARE(CommandId(Custom.value(QKeyCombination(Qt::AltModifier, Qt::Key_X))), QString("hc.archive.extract"));
    }
    void validation_data()
    {
        QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<QString>("error");
        QTest::newRow("malformed") << QByteArray("{") << QString("json-byte");
        auto Root = QJsonDocument::fromJson(MenuBytes()).object(); auto Menus = Root["menus"].toArray();
        auto File = Menus[0].toObject(); auto Items = File["items"].toArray(); Items.append(Items[0]);
        File["items"] = Items; Menus[0] = File; Root["menus"] = Menus;
        QTest::newRow("duplicate") << QJsonDocument(Root).toJson() << QString("duplicate-id");
        Root = QJsonDocument::fromJson(MenuBytes()).object(); Menus = Root["menus"].toArray();
        auto Other = Menus[0].toObject(); Other["id"] = "other"; Other["items"] = QJsonArray{}; Menus.append(Other); Root["menus"] = Menus;
        QTest::newRow("mnemonic") << QJsonDocument(Root).toJson() << QString("duplicate-mnemonic");
        QTest::newRow("size") << QByteArray(1024 * 1024 + 1, ' ') << QString("document-too-large");
        Root = QJsonDocument::fromJson(MenuBytes()).object(); Root["schemaVersion"] = 2;
        QTest::newRow("schema") << QJsonDocument(Root).toJson() << QString("invalid-root");
        Root = QJsonDocument::fromJson(MenuBytes()).object(); Menus = Root["menus"].toArray(); File = Menus[0].toObject();
        Items = QJsonArray{}; for (int Index = 0; Index < 512; ++Index) Items.append(QJsonObject{{"type", "separator"}});
        File["items"] = Items; Menus[0] = File; Root["menus"] = Menus;
        QTest::newRow("count") << QJsonDocument(Root).toJson() << QString("item-limit");
        QJsonObject Deep{{"id", "deep5"}, {"label", "deep"}, {"items", QJsonArray{}}};
        for (int Index = 4; Index >= 0; --Index) Deep = QJsonObject{{"id", QString("deep%1").arg(Index)}, {"label", "deep"}, {"items", QJsonArray{Deep}}};
        QTest::newRow("depth") << QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"menus", QJsonArray{Deep}}}).toJson() << QString("depth-limit");
    }
    void validation()
    {
        QFETCH(QByteArray, bytes); QFETCH(QString, error);
        const auto Result = MenuDefinition::Parse(bytes, {"hc.file.copy"}, {"automation"});
        QVERIFY2(Result.error_.contains(error), qPrintable(Result.error_)); QVERIFY(Result.menus_.isEmpty());
    }
    void reloadAndButtonDispatch()
    {
        QTemporaryDir Directory; const auto Path = QDir(Directory.path()).filePath("menu-bar.json");
        QMenuBar Bar; QAction Copy("복사", &Bar); QMenu Scripts("자동화", &Bar);
        int Executions = 0; connect(&Copy, &QAction::triggered, &Bar, [&] { ++Executions; });
        UserMenu Menu(&Bar, Path, MenuBytes(), {{"hc.file.copy", &Copy}}, {{"automation", &Scripts}});
        QVERIFY(Menu.Reload()); QVERIFY(QFileInfo::exists(Path));
        QVERIFY(Write(Path, MenuBytes("cm_CopyOtherPanel", "사용자 복사")));
        for (int Index = 0; Index < 3; ++Index) QVERIFY(Menu.Reload());
        auto* Proxy = Bar.actions()[0]->menu()->actions()[0];
        QCOMPARE(Proxy->text(), "사용자 복사"); Proxy->trigger(); QCOMPARE(Executions, 1);
        QVERIFY(Bar.actions()[0]->menu()->actions().contains(Scripts.menuAction()));
        fm::ui::FunctionKeyBar Buttons; auto* Button = Buttons.addKey({}, "복사"); BindFunctionButton(Button, &Copy);
        Copy.setProperty("hc.keyLabel", "Ctrl+Alt+C"); Copy.setText("복사\tCtrl+Alt+C");
        QCOMPARE(Button->keys(), "Ctrl+Alt+C"); QCOMPARE(Proxy->text(), "사용자 복사\tCtrl+Alt+C");
        Button->click(); QCOMPARE(Executions, 2);
        Copy.setProperty("hc.keyLabel", ""); Copy.setText("복사"); QCOMPARE(Button->keys(), "");
        Button->click(); QCOMPARE(Executions, 3);
        Copy.setEnabled(false); QVERIFY(!Proxy->isEnabled()); QVERIFY(!Button->isEnabled());
        const auto Before = Bar.actions()[0];
        QVERIFY(Write(Path, "{invalid")); QVERIFY(!Menu.Reload()); QCOMPARE(Bar.actions()[0], Before);
        QFile Bad(Path); QVERIFY(Bad.open(QIODevice::ReadOnly)); QCOMPARE(Bad.readAll(), "{invalid"); Bad.close();
        QVERIFY(Write(Path, MenuBytes("hc.plugin.missing"))); QVERIFY(Menu.Reload());
        QVERIFY(!Bar.actions()[0]->menu()->actions()[0]->isEnabled());
    }
    void targetDestroyed()
    {
        QTemporaryDir Directory; QMenuBar Bar; QMenu Scripts("자동화", &Bar);
        auto* Command = new QAction("복사", &Bar);
        UserMenu Menu(&Bar, QDir(Directory.path()).filePath("menu-bar.json"), MenuBytes(), {{"hc.file.copy", Command}}, {{"automation", &Scripts}});
        QVERIFY(Menu.Reload()); auto* Proxy = Bar.actions()[0]->menu()->actions()[0];
        fm::ui::FunctionKeyBar Buttons; auto* Button = Buttons.addKey({}, "복사"); BindFunctionButton(Button, Command);
        delete Command; QVERIFY(!Proxy->isEnabled()); QVERIFY(!Button->isEnabled());
    }
    void keyboardUsesSameCommand()
    {
        class List : public QWidget {
        public:
            TyMapShortcutToCMDStr keys_;
            std::function<bool(const QString&)> invoke_;
        protected:
            void keyPressEvent(QKeyEvent* Event) override {
                if (InvokeShortcut(*Event, keys_, invoke_)) Event->accept(); else QWidget::keyPressEvent(Event);
            }
        };
        QTemporaryDir Directory; QMenuBar Bar; QAction Copy("복사", &Bar); QMenu Scripts("자동화", &Bar);
        int Executions = 0; connect(&Copy, &QAction::triggered, &Bar, [&] { ++Executions; });
        UserMenu Menu(&Bar, QDir(Directory.path()).filePath("menu-bar.json"), MenuBytes(), {{"hc.file.copy", &Copy}}, {{"automation", &Scripts}});
        QVERIFY(Menu.Reload());
        List Files;
        Files.keys_ = EffectiveShortcuts(QJsonObject{{"shortcutPreset", "prototype"}}, QJsonObject{{"F5", QJsonValue::Null}, {"Ctrl+Alt+C", "hc.file.copy"}});
        Files.invoke_ = [&](const QString& Id) { if (Id != "hc.file.copy") return false; Copy.trigger(); return true; };
        QTest::keyClick(&Files, Qt::Key_F5); QCOMPARE(Executions, 0);
        QTest::keyClick(&Files, Qt::Key_C, Qt::ControlModifier | Qt::AltModifier); QCOMPARE(Executions, 1);
        Bar.actions()[0]->menu()->actions()[0]->trigger(); QCOMPARE(Executions, 2);
        fm::ui::FunctionKeyBar Buttons; auto* Button = Buttons.addKey({}, "복사"); BindFunctionButton(Button, &Copy);
        Button->click(); QCOMPARE(Executions, 3);
        QLineEdit Input; QTest::keyClicks(&Input, "copy"); QCOMPARE(Input.text(), "copy"); QCOMPARE(Executions, 3);
    }
    void defaultResource()
    {
        QFile File(":/HyperCommander/menu-bar.json"); QVERIFY(File.open(QIODevice::ReadOnly));
        const auto Definition = MenuDefinition::Parse(File.readAll(), {}, {"plugins", "automation"});
        QVERIFY2(Definition.error_.isEmpty(), qPrintable(Definition.error_)); QCOMPARE(Definition.menus_.size(), 9);
    }
};
QTEST_MAIN(CommandUiTests)
#include "tstCommandUi.moc"
