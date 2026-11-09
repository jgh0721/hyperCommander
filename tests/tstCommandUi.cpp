#include <QtTest>
#include "UniqueLibs/commandLineExecution.hpp"
#include <QTemporaryDir>
#include <QLineEdit>
#include "UniqueLibs/renameSelection.hpp"
#include "UniqueLibs/navigationError.hpp"
#include "UniqueLibs/userMenu.hpp"
#include "UniqueLibs/commandUiWidgets.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/settings/fileColorRules.hpp"
#include "UniqueLibs/itemSelection.hpp"
#include "commonLibs/Qt/elidedLabel.hpp"
#include <QVBoxLayout>
#include <QPushButton>
#include "UniqueLibs/internetFile.hpp"

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
    void commandLinePromptElision()
    {
        QCOMPARE(CommandLinePrompt("C:\\work"), QString("C:\\work>"));
        QCOMPARE(CommandLinePrompt("C:\\work>"), QString("C:\\work>"));
        const auto Path = QStringLiteral("D:\\작업 폴더\\매우 긴 상위 디렉터리\\추가 경로\\현재 작업 폴더");
        const auto Short = CommandLinePrompt(Path);
        QCOMPARE(Short.toUcs4().size(), 36);
        QVERIFY(Short.startsWith("D:\\"));
        QVERIFY(Short.contains("..."));
        QVERIFY(Short.endsWith("현재 작업 폴더>"));
        const auto Unicode = QString::fromUcs4(U"D:\\😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀");
        const auto Elided = CommandLinePrompt(Unicode);
        QCOMPARE(Elided.toUcs4().size(), 36);
        QVERIFY(!Elided.contains(QChar::ReplacementCharacter));
        QCOMPARE(QString::fromUtf8(Elided.toUtf8()), Elided);
    }
    void commandLineContextualKeys()
    {
        const auto Keys = EffectiveShortcuts({}, {});
        QCOMPARE(CommandLineShortcut(QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier), {}), QString("hc.commandLine.execute"));
        QCOMPARE(CommandLineShortcut(QKeyEvent(QEvent::KeyPress, Qt::Key_Enter, Qt::ShiftModifier | Qt::KeypadModifier), {}), QString("hc.commandLine.keepOpen"));
        QVERIFY(CommandLineShortcut(QKeyEvent(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier), {}).isEmpty());
        const QJsonObject Overrides{{"commandLine", QJsonObject{{"Return", QJsonValue::Null}, {"Ctrl+R", "hc.commandLine.keepOpen"}}}};
        const auto Custom = EffectiveShortcuts({}, Overrides);
        QVERIFY(CommandLineShortcut(QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier), Overrides).isEmpty());
        QCOMPARE(CommandLineShortcut(QKeyEvent(QEvent::KeyPress, Qt::Key_R, Qt::ControlModifier), Overrides), QString("hc.commandLine.keepOpen"));
        QCOMPARE(Custom.value(QKeyCombination(Qt::Key_Return)), Keys.value(QKeyCombination(Qt::Key_Return)));
        QCOMPARE(ShortcutCommand(QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier), Keys), QString("hc.navigate.open"));
    }
    void commandLineSavedPanelBindings()
    {
        // SaveSettings materializes every effective panel key. Such a snapshot
        // used to turn off both command-input defaults after opening settings.
        for (const auto* Preset : {"legacy", "prototype"}) {
            const QJsonObject General{{"shortcutPreset", Preset}};
            const auto Keys = EffectiveShortcuts(General, {});
            QJsonObject Saved;
            for (auto It = Keys.begin(); It != Keys.end(); ++It)
                Saved[QKeySequence(It.key()).toString(QKeySequence::PortableText)] = CommandId(It.value());
            for (const auto Key : {Qt::Key_Return, Qt::Key_Enter}) {
                QCOMPARE(CommandLineShortcut(QKeyEvent(QEvent::KeyPress, Key, Qt::NoModifier), Saved), QString("hc.commandLine.execute"));
                QCOMPARE(CommandLineShortcut(QKeyEvent(QEvent::KeyPress, Key, Qt::ShiftModifier), Saved), QString("hc.commandLine.keepOpen"));
            }
            QCOMPARE(EffectiveShortcuts(General, Saved), Keys);
            Saved["Return"] = QJsonValue::Null;
            QCOMPARE(CommandLineShortcut(QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier), Saved), QString("hc.commandLine.execute"));
            QCOMPARE(ShortcutCommand(QKeyEvent(QEvent::KeyPress, Qt::Key_Slash, Qt::ControlModifier), Keys), QString("hc.commandLine.activate"));
        }
        const QJsonObject Legacy{{"Return", "hc.commandLine.keepOpen"}, {"Ctrl+R", "hc.commandLine.execute"}};
        QCOMPARE(CommandLineShortcut(QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier), Legacy), QString("hc.commandLine.keepOpen"));
        QCOMPARE(CommandLineShortcut(QKeyEvent(QEvent::KeyPress, Qt::Key_R, Qt::ControlModifier), Legacy), QString("hc.commandLine.execute"));
        QCOMPARE(ShortcutCommand(QKeyEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier), EffectiveShortcuts({}, Legacy)), QString("hc.navigate.open"));
        auto Scoped = Legacy;
        Scoped["commandLine"] = QJsonObject{{"Return", QJsonValue::Null}, {"Ctrl+C", "hc.commandLine.execute"}};
        QVERIFY(CommandLineShortcut(QKeyEvent(QEvent::KeyPress, Qt::Key_Enter, Qt::KeypadModifier), Scoped).isEmpty());
        QVERIFY(CommandLineShortcut(QKeyEvent(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier), Scoped).isEmpty());
        const QJsonObject Disabled{{"Ctrl+/", QJsonValue::Null}};
        QVERIFY(CommandKeys(EffectiveShortcuts({}, Disabled), "hc.commandLine.activate").isEmpty());
        const QJsonObject Rebound{{"Ctrl+/", "hc.jobs.show"}};
        QCOMPARE(ShortcutCommand(QKeyEvent(QEvent::KeyPress, Qt::Key_Slash, Qt::ControlModifier), EffectiveShortcuts({}, Rebound)), QString("hc.jobs.show"));
    }
    void commandLineLaunchPlans()
    {
        using namespace hc::ui::commandLine;
        const auto Cwd = QDir::tempPath();
        for (const bool Keep : {false, true}) {
            const auto Bare = Plan("cmd", Cwd, Keep);
            QVERIFY(Bare.program_.endsWith("cmd.exe", Qt::CaseInsensitive));
            QVERIFY(Bare.arguments_.isEmpty());
            QCOMPARE(Plan("cmd /C echo done", Cwd, Keep).arguments_, QString("/C echo done"));
            QCOMPARE(Plan("cmd /K echo done", Cwd, Keep).arguments_, QString("/K echo done"));
            QCOMPARE(Plan("cmd /K echo done > result.txt", Cwd, Keep).arguments_, QString("/K echo done > result.txt"));
        }
        QCOMPARE(Plan("echo hello | findstr hello > out.txt", Cwd, false).arguments_,
            QString("/D /S /C \"echo hello | findstr hello > out.txt\""));
        QCOMPARE(Plan("copy con xxx.bat", Cwd, true).arguments_, QString("/D /S /K \"copy con xxx.bat\""));
        QVERIFY(HasShellSyntax("cd sub && dir"));
        QVERIFY(HasShellSyntax("cd %TEMP%"));
        QVERIFY(!HasShellSyntax("cd \"한글 & 공백\""));
        const auto Quoted = QString("\"%1\" /C echo done").arg(qEnvironmentVariable("COMSPEC"));
        QCOMPARE(Plan(Quoted, Cwd, true).arguments_, QString("/C echo done"));
        QVERIFY(!Launch(Plan("cmd", Cwd, false), "\\\\server\\share").isEmpty());
        QVERIFY(!Launch(Plan("cmd", Cwd, false), Cwd + "/hc-no-such-directory").isEmpty());
    }
    void commandLineDetachedRedirection()
    {
        using namespace hc::ui::commandLine;
        QTemporaryDir Root; QVERIFY(Root.isValid());
        const auto Cwd = Root.path() + "/한글 공백";
        QVERIFY(QDir().mkpath(Cwd));
        const auto Command = QStringLiteral("(echo first&echo second) | findstr second > \"결과 파일.txt\"");
        QVERIFY2(Launch(Plan(Command, Cwd, false), Cwd).isEmpty(), "detached launch failed");
        const auto Path = Cwd + "/결과 파일.txt";
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(Path).size() > 0, 5000);
        QFile Result(Path); QVERIFY(Result.open(QIODevice::ReadOnly));
        QCOMPARE(Result.readAll().trimmed(), QByteArray("second"));
        const auto Program = Cwd + "/한글 도구.exe";
        QVERIFY(QFile::copy(qEnvironmentVariable("COMSPEC"), Program));
        const auto Direct = QString("\"%1\" /D /C \"echo quoted>direct.txt\"").arg(Program);
        const auto DirectPlan = Plan(Direct, Cwd, false);
        QCOMPARE(QDir::fromNativeSeparators(DirectPlan.program_), Program);
        QVERIFY(Launch(DirectPlan, Cwd).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(Cwd + "/direct.txt").size() > 0, 5000);
        QVERIFY(Launch(Plan("cd .. && cd > cwd.txt", Cwd, false), Cwd).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(Root.path() + "/cwd.txt").size() > 0, 5000);
    }

    void compareCommandShortcut()
    {
        for (const auto& Preset : {"legacy", "prototype"}) {
            const auto Keys = EffectiveShortcuts({{"shortcutPreset", Preset}}, {});
            QCOMPARE(CommandId(Keys.value(QKeyCombination(Qt::ControlModifier, Qt::Key_O))), QString("hc.file.compare"));
            QString Invoked; const QKeyEvent Event(QEvent::KeyPress, Qt::Key_O, Qt::ControlModifier);
            QVERIFY(InvokeShortcut(Event, Keys, [&](const QString& Id) { Invoked = Id; return true; })); QCOMPARE(Invoked, QString("hc.file.compare"));
            QCOMPARE(CommandId(EffectiveShortcuts({{"shortcutPreset", Preset}}, {{"Ctrl+O", "hc.jobs.show"}}).value(QKeyCombination(Qt::ControlModifier, Qt::Key_O))), QString("hc.jobs.show"));
            QVERIFY(!EffectiveShortcuts({{"shortcutPreset", Preset}}, {{"Ctrl+O", QJsonValue::Null}}).contains(QKeyCombination(Qt::ControlModifier, Qt::Key_O)));
        }
        bool Found = false; for (const auto& Command : hc::defaults::BusCommands()) Found |= Command.id_ == "hc.file.compare";
        QVERIFY(Found);
    }
    void itemSelectionConditionsAndGroups()
    {
        using namespace hc::selection;
        Matcher M; Criteria C; C.pattern_ = "*.jpg; *.png"; C.exclude_ = "*_backup.*";
        QVERIFY(M.Compile(C).isEmpty());
        QVERIFY(M.Matches({"COVER.JPG", 0, 100, false}));
        QVERIFY(!M.Matches({"cover_backup.png", 0, 100, false}));
        QVERIFY(!M.Matches({"folder.png", 0x10, 0, false}));
        QVERIFY(!M.Matches({"..", 0x10, 0, false}));
        C.pattern_ = "*.*"; QVERIFY(M.Compile(C).isEmpty()); QVERIFY(M.Matches({"README", 0, 0, false}));
        C.pattern_ = "["; C.syntax_ = Syntax::Regex; QVERIFY(!M.Compile(C).isEmpty());
        C.pattern_ = "^cover\\.jpg$"; QVERIFY(M.Compile(C).isEmpty()); QVERIFY(M.Matches({"cover.jpg", 0, 0, false}));
        C.caseSensitive_ = true; QVERIFY(M.Compile(C).isEmpty()); QVERIFY(!M.Matches({"COVER.JPG", 0, 0, false}));
        QVERIFY(hc::selection::Groups({}).isEmpty());
        QVERIFY(hc::selection::Groups(hc::settings::FileColorRules::Empty()).isEmpty());
        const QJsonObject Rule{{"id", "custom"}, {"name", "recent-doc"}, {"join", "all"}, {"enabled", true},
            {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"op", "in"}, {"value", "pdf,hwpx"}},
                QJsonObject{{"type", "size"}, {"op", "ge"}, {"value", "2"}, {"unit", "MiB"}}}}};
        auto Disabled = Rule; Disabled["id"] = "disabled"; Disabled["name"] = "disabled-doc"; Disabled["enabled"] = false;
        const auto UserGroups = hc::selection::Groups({{"version", 2}, {"mode", "first"}, {"groups", QJsonArray{Disabled, Rule}}});
        QCOMPARE(UserGroups.size(), 1); QCOMPARE(UserGroups.front().id_, "user:custom"); QCOMPARE(UserGroups.front().name_, "recent-doc");
        C = {}; C.groupId_ = UserGroups.front().id_; C.group_ = UserGroups.front().rule_;
        QVERIFY(M.Compile(C).isEmpty()); QVERIFY(!M.Matches({"report.pdf", 0, 1024, false}));
        QVERIFY(M.Matches({"report.pdf", 0, 3 * 1048576, false}));
        QVERIFY(M.Matches({"한글.HWPX", 0, 3 * 1048576, false}));
        QVERIFY(!M.Matches({"image.png", 0, 3 * 1048576, false}));
        C.group_ = {}; QVERIFY(!M.Compile(C).isEmpty());
        const QJsonObject LegacyRule{{"id", "legacy"}, {"name", "legacy-doc"}, {"join", "all"},
            {"conditions", QJsonArray{QJsonObject{{"type", "extension"}, {"value", "pdf"}},
                QJsonObject{{"type", "sizeMore"}, {"value", "100"}}}}};
        const auto LegacyGroups = hc::selection::Groups({{"version", 1}, {"mode", "first"}, {"groups", QJsonArray{LegacyRule}}});
        QCOMPARE(LegacyGroups.size(), 1); C.groupId_ = LegacyGroups.front().id_; C.group_ = LegacyGroups.front().rule_;
        QVERIFY(M.Compile(C).isEmpty()); QVERIFY(M.Matches({"report.pdf", 0, 101, false}));
        QVERIFY(!M.Matches({"report.pdf", 0, 100, false}));
        const auto Retired = Criteria::Load({{"groupId", "builtin.document"}, {"pattern", "*.pdf; *.hwpx"}});
        QVERIFY(Retired.groupId_.isEmpty()); QVERIFY(M.Compile(Retired).isEmpty());
        QVERIFY(M.Matches({"report.pdf", 0, 0, false})); QVERIFY(!M.Matches({"image.png", 0, 0, false}));
    }
    void itemSelectionAdvancedAndSetOperations()
    {
        using namespace hc::selection;
        Criteria C; C.advanced_ = true; C.pattern_ = "*.jpg"; C.size_ = "2"; C.modified_ = QDate(2026, 10, 13);
        Matcher M; QVERIFY(M.Compile(C).isEmpty());
        hc::settings::FileRuleInput Input{"photo.jpg", 0, 3 * 1048576, false};
        Input.modifiedUtcMs_ = C.modified_.startOfDay().toMSecsSinceEpoch(); QVERIFY(M.Matches(Input));
        Input.modifiedUtcMs_.reset(); QVERIFY(!M.Matches(Input));
        C.all_ = false; QVERIFY(M.Compile(C).isEmpty()); QVERIFY(M.Matches(Input));
        C.exclude_ = "*.jpg"; QVERIFY(M.Compile(C).isEmpty()); QVERIFY(!M.Matches(Input));
        C.size_ = "nan"; QVERIFY(!M.Compile(C).isEmpty()); C.size_ = "-1"; QVERIFY(!M.Compile(C).isEmpty());
        C.size_ = "1e100"; QVERIFY(!M.Compile(C).isEmpty());
        for (const bool Before : {false, true}) for (const bool Match : {false, true}) {
            QCOMPARE(Matcher::Selected(Operation::Add, Before, Match), Before || Match);
            QCOMPARE(Matcher::Selected(Operation::Remove, Before, Match), Before && !Match);
            QCOMPARE(Matcher::Selected(Operation::Replace, Before, Match), Match);
            QCOMPARE(Matcher::Selected(Operation::Intersect, Before, Match), Before && Match);
        }
        const auto Encoded = C.Save(); QCOMPARE(Criteria::Load(Encoded).Save(), Encoded);
    }
    void changingStatusKeepsLayout()
    {
        QWidget Window;auto* Layout=new QVBoxLayout(&Window);auto* Status=new hc::qt::ElidedLabel(&Window);
        Status->SetLineCount(4);Status->setWordWrap(true);Layout->addWidget(Status);
        auto* Footer=new QPushButton("Apply",&Window);Layout->addWidget(Footer);Window.resize(420,220);Window.show();
        QCoreApplication::processEvents();const auto Size=Window.size();const auto Bounds=Footer->geometry();const auto LabelBounds=Status->geometry();
        for(const auto& Text:QStringList{"", "Scanning...", QString(4000,'x'), "one\ntwo\nthree\nfour\nfive"}) {
            Status->SetText(Text);QCoreApplication::processEvents();
            QCOMPARE(Window.size(),Size);QCOMPARE(Footer->geometry(),Bounds);QCOMPARE(Status->geometry(),LabelBounds);QCOMPARE(Status->toolTip(),Text);
        }
    }
    void mediaDefaultShortcuts()
    {
        const auto Keys=EffectiveShortcuts({}, {});
        const auto Command=[&](Qt::Key Key){return CommandId(Keys.value(QKeyCombination(Qt::ControlModifier|Qt::AltModifier,Key)));};
        QCOMPARE(Command(Qt::Key_Up),"hc.audio.preview");QCOMPARE(Command(Qt::Key_Down),"hc.audio.stop");
        QCOMPARE(Command(Qt::Key_Left),"hc.audio.prev");QCOMPARE(Command(Qt::Key_Right),"hc.audio.next");
        QVERIFY(CommandId(Keys.value(QKeyCombination(Qt::ControlModifier,Qt::Key_Return)))!="hc.audio.preview");
    }
    void folderSizeSelection()
    {
        using namespace hc::selection;
        Criteria C; C.advanced_ = true; C.scope_ = Scope::Folders; C.pattern_ = "*"; C.size_ = "0"; C.minimumSize_ = false;
        Matcher M; QVERIFY(M.Compile(C).isEmpty());
        QVERIFY(M.Matches({"empty", 0x10, 0, false}));
        QVERIFY(!M.Matches({"nonempty", 0x10, 1, false}));
        QVERIFY(!M.Matches({"unknown", 0x10, 0, false}, nullptr, nullptr, false));
        C.minimumSize_ = true; C.size_ = "1"; QVERIFY(M.Compile(C).isEmpty());
        QVERIFY(M.Matches({"large", 0x10, 1048576, false}));
        QVERIFY(!M.Matches({"small", 0x10, 1048575, false}));
    }
    void itemSelectionShortcutsAndOverrides()
    {
        for (const auto& Preset : {"legacy", "prototype"}) {
            const auto Keys = EffectiveShortcuts({{"shortcutPreset", Preset}}, {});
            const auto Command = [&](Qt::KeyboardModifiers Mod, Qt::Key Key) { return CommandId(Keys.value(QKeyCombination(Mod, Key))); };
            QCOMPARE(Command(Qt::ControlModifier, Qt::Key_Equal), "hc.selection.select");
            QCOMPARE(Command(Qt::ControlModifier, Qt::Key_Minus), "hc.selection.deselect");
            QCOMPARE(Command(Qt::KeypadModifier, Qt::Key_Plus), "hc.selection.select");
            QCOMPARE(Command(Qt::KeypadModifier, Qt::Key_Minus), "hc.selection.deselect");
            QCOMPARE(Command(Qt::ControlModifier, Qt::Key_A), "hc.archive.create");
        }
        const auto Keys = EffectiveShortcuts({}, {{"Ctrl+=", "hc.file.copy"}, {"Ctrl+-", QJsonValue::Null}});
        QCOMPARE(CommandId(Keys.value(QKeyCombination(Qt::ControlModifier, Qt::Key_Equal))), "hc.file.copy");
        QVERIFY(!Keys.contains(QKeyCombination(Qt::ControlModifier, Qt::Key_Minus)));
    }
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
    void fileClipboardCommandsAndShortcuts()
    {
        const auto Descriptors = hc::defaults::BusCommands();
        for (const auto& Preset : {"legacy", "prototype"}) {
            const QJsonObject General{{"shortcutPreset", Preset}};
            for (const auto& Item : QList<QPair<QString, Qt::Key>>{{"hc.clipboard.cut", Qt::Key_X},
                {"hc.clipboard.copy", Qt::Key_C}, {"hc.clipboard.paste", Qt::Key_V}}) {
                const QKeyCombination Key(Qt::ControlModifier | Qt::AltModifier, Item.second);
                QCOMPARE(CommandId(EffectiveShortcuts(General, {}).value(Key)), Item.first);
                QCOMPARE(QString(FindCommand(Item.first).Name), Item.first);
                QCOMPARE(std::count_if(Descriptors.begin(), Descriptors.end(), [&](const auto& Descriptor) {
                    return Descriptor.id_ == Item.first.toStdString() && Descriptor.parameters_.empty();
                }), 1);
                const auto Text = QKeySequence(Key).toString(QKeySequence::PortableText);
                QVERIFY(!EffectiveShortcuts(General, {{Text, QJsonValue::Null}}).contains(Key));
                QCOMPARE(CommandId(EffectiveShortcuts(General, {{Text, "hc.jobs.show"}}).value(Key)), "hc.jobs.show");
            }
        }
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
    void keypadNavigationShortcuts()
    {
        const QKeyCombination Plain(Qt::ControlModifier, Qt::Key_PageDown);
        const QKeyCombination Keypad(Qt::ControlModifier | Qt::KeypadModifier, Qt::Key_PageDown);
        const QKeyEvent Event(QEvent::KeyPress, Keypad.key(), Keypad.keyboardModifiers());
        auto Keys = EffectiveShortcuts({}, {{"Ctrl+PgDown", "hc.navigate.enterFile"}});
        QString Invoked;
        const auto Invoke = [&](const QString& Id) { Invoked = Id; return true; };
        QVERIFY(InvokeShortcut(Event, Keys, Invoke)); QCOMPARE(Invoked, QString("hc.navigate.enterFile"));
        Keys[Keypad] = QLatin1String("hc.navigate.parent");
        QVERIFY(InvokeShortcut(Event, Keys, Invoke)); QCOMPARE(Invoked, QString("hc.navigate.parent"));
        Keys.remove(Keypad); Keys.remove(Plain); Invoked.clear();
        QVERIFY(!InvokeShortcut(Event, Keys, Invoke)); QVERIFY(Invoked.isEmpty());
        QVERIFY(!InvokeShortcut(Event, EffectiveShortcuts({{"useBuiltInShortcuts", false}}, {}), Invoke));
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
        for (const auto& Preset : hc::defaults::ShortcutPresetIds()) {
            const auto Shortcuts = EffectiveShortcuts({{"shortcutPreset", Preset}}, {});
            QCOMPARE(CommandId(Shortcuts.value(QKeyCombination(Qt::Key_F2))), QString("hc.file.rename"));
        }
        QCOMPARE(hc::defaults::Options()["renameInDialog"].toBool(), false);
        QCOMPARE(hc::defaults::Options(true)["renameInDialog"].toBool(), true);
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
    void defaultFileGroups()
    {
        using hc::settings::FileColorRules;
        const auto Fresh = hc::defaults::FileGroups(), Existing = hc::defaults::FileGroups(true);
        QVERIFY2(FileColorRules::Validate(Fresh).isEmpty(), qPrintable(FileColorRules::Validate(Fresh)));
        QCOMPARE(Fresh["mode"].toString(), QString("merge")); QCOMPARE(Fresh["groups"].toArray().size(), 29);
        QVERIFY(Existing["groups"].toArray().isEmpty());
        QCOMPARE(hc::defaults::Settings()["options"].toObject()["fileColorRules"].toObject(), Fresh);
        QCOMPARE(hc::defaults::Settings(true)["options"].toObject()["fileColorRules"].toObject(), Existing);
        QHash<QString, QString> Owners;
        for (const auto Value : Fresh["groups"].toArray()) {
            const auto G = Value.toObject(); QVERIFY(G["id"].toString().startsWith("default."));
            QCOMPARE(G["autoDark"].toBool(), G["id"] == "default.internet");
            for (const auto C : G["conditions"].toArray()) if (C.toObject()["type"] == "extension")
                for (const auto& Ext : FileColorRules::Extensions(C.toObject()["value"].toString())) {
                    QVERIFY2(!Owners.contains(Ext), qPrintable(Ext + ": " + Owners.value(Ext) + " / " + G["name"].toString()));
                    Owners.insert(Ext, G["name"].toString());
                }
        }
        QCOMPARE(Owners.size(), 443);
        FileColorRules Rules; QVERIFY(Rules.Load(Fresh).isEmpty());
        const auto Style = [&](const QString& Name, quint32 Attributes = 0, quint64 Size = 1, bool Dark = false) {
            hc::settings::FileRuleInput Input{Name, Attributes, Size, Dark};
            Input.fields_.append(hc::columns::CellValue::Missing(hc::columns::ValueState::Empty));
            return Rules.Evaluate(Input);
        };
        QCOMPARE(Rules.RequiredFields(), QStringList{"file.alternateDataStream.zone"});
        hc::settings::FileRuleInput Internet{"download.txt", 0, 1, false};
        Internet.fields_.append(hc::columns::CellValue::Ready(quint64(3)));
        QCOMPARE(Rules.Evaluate(Internet).font_.value("border").toString(), QString("dash"));
        Internet.fields_[0] = hc::columns::CellValue::Ready(quint64(2));
        QVERIFY(!Rules.Evaluate(Internet).font_.contains("border"));
        const auto Disguised = Style("report.pdf.exe");
        QCOMPARE(Disguised.foreground_, QColor("#B8231A")); QCOMPARE(Disguised.background_, QColor("#FBE9E7")); QVERIFY(Disguised.font_.value("bold").toBool());
        QCOMPARE(Style(QString("photo") + QChar(0x202E) + "gpj.exe", 0, 1, true).background_, QColor("#3A1E1B"));
        QCOMPARE(Style(QString("left-") + QChar(0x05D0) + ".txt").foreground_, QColor("#B8231A"));
        QCOMPARE(Style("setup.exe").foreground_, QColor("#18793A")); QCOMPARE(Style("setup.exe", 0, 1, true).foreground_, QColor("#56D364"));
        QCOMPARE(Style("src", 0x10).foreground_, QColor("#C42B1C")); QCOMPARE(Style("src", 0x10 | 0x2).foreground_, QColor("#6C6961"));
        const auto Large = Style("movie.mkv", 0, 2ull << 30);
        QCOMPARE(Large.foreground_, QColor("#00706A")); QCOMPARE(Large.background_, QColor("#FBEAE6"));
        QCOMPARE(Style("CMakeLists.txt").foreground_, QColor("#1D5BC7")); QCOMPARE(Style(".gitignore").foreground_, QColor("#00639A"));
        QCOMPARE(Style("id_ed25519").foreground_, QColor("#8F4A05")); QVERIFY(Style("~$report.docx").font_.value("italic").toBool());
        QVERIFY(Style(QString("한글.txt").normalized(QString::NormalizationForm_D)).font_.value("underline").toBool());
        QVERIFY(!Style("model.stl").matched_);
    }
    void internetBlockRemovalPreservesOtherData()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto Path = Directory.filePath("download.txt");
        QVERIFY(Write(Path, "original body"));
        QVERIFY(Write(Path + ":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n"));
        QVERIFY(Write(Path + ":note", "keep"));
        bool Changed = false;
        QCOMPARE(hc::files::RemoveInternetBlock(Path, Changed), DWORD(ERROR_SUCCESS));
        QVERIFY(Changed);
        QFile Marker(Path + ":Zone.Identifier"); QVERIFY(!Marker.open(QIODevice::ReadOnly));
        QFile Body(Path); QVERIFY(Body.open(QIODevice::ReadOnly)); QCOMPARE(Body.readAll(), QByteArray("original body"));
        QFile Note(Path + ":note"); QVERIFY(Note.open(QIODevice::ReadOnly)); QCOMPARE(Note.readAll(), QByteArray("keep"));
        QCOMPARE(hc::files::RemoveInternetBlock(Path, Changed), DWORD(ERROR_SUCCESS)); QVERIFY(!Changed);
        QVERIFY(hc::files::RemoveInternetBlock(Directory.filePath("missing.txt"), Changed) != ERROR_SUCCESS);
        QVERIFY(!Changed);
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
        QCOMPARE(General["shortcutPreset"].toString(), "product");
        const auto Values = EffectiveShortcuts(General, {});
        const QMap<QString, QString> Requested{{"Ctrl+C", "hc.file.copy"}, {"F2", "hc.file.rename"},
            {"Ctrl+R", "hc.file.rename"}, {"Ctrl+M", "hc.file.move"}, {"Ctrl+Alt+R", "hc.file.batchRename"},
            {"Ctrl+K", "hc.file.mkdir"}, {"Ctrl+A", "hc.archive.create"}, {"Ctrl+X", "hc.archive.extract"},
            {"Ctrl+Alt+B", "hc.file.ads.allowInternetBlock"}};
        for (auto It = Requested.cbegin(); It != Requested.cend(); ++It)
            QCOMPARE(CommandId(Values.value(QKeySequence::fromString(It.key(), QKeySequence::PortableText)[0])), It.value());
        QCOMPARE(CommandId(Values.value(QKeyCombination(Qt::Key_F5))), "hc.navigate.refresh");
        const auto Prototype = EffectiveShortcuts({{"shortcutPreset", "prototype"}}, {});
        QCOMPARE(CommandId(Prototype.value(QKeyCombination(Qt::Key_F6))), "hc.file.move");
        QCOMPARE(CommandId(Prototype.value(QKeyCombination(Qt::Key_F7))), "hc.file.mkdir");
        QCOMPARE(CommandId(Prototype.value(QKeyCombination(Qt::Key_F8))), "hc.file.delete");
        QCOMPARE(CommandKeys(Prototype, "hc.file.batchRename").first().toString(QKeySequence::PortableText), "Ctrl+M");
        const auto UserKeys = EffectiveShortcuts(General, {{"Ctrl+C", QJsonValue::Null}, {"Ctrl+M", "hc.file.batchRename"}});
        QVERIFY(!UserKeys.contains(QKeySequence("Ctrl+C")[0]));
        QCOMPARE(CommandId(UserKeys.value(QKeySequence("Ctrl+M")[0])), QString("hc.file.batchRename"));
        QCOMPARE(CommandId(Values.value(QKeyCombination(Qt::ControlModifier, Qt::Key_F5))), "hc.navigate.forceRefresh");
        const auto RefreshKeys = EffectiveShortcuts(General, {{"Ctrl+R", "hc.file.rename"}, {"F5", "hc.navigate.refresh"}, {"Shift+F5", "hc.navigate.forceRefresh"}});
        QCOMPARE(CommandId(RefreshKeys.value(QKeyCombination(Qt::ControlModifier, Qt::Key_R))), "hc.file.rename");
        QCOMPARE(CommandId(RefreshKeys.value(QKeyCombination(Qt::Key_F5))), "hc.navigate.refresh");
        QCOMPARE(CommandId(RefreshKeys.value(QKeyCombination(Qt::ShiftModifier, Qt::Key_F5))), "hc.navigate.forceRefresh");
        QVERIFY(!FindCommand("hc.navigate.forceRefresh").Name.isEmpty());
        auto OldDefaults = hc::settings::Defaults(); auto OldGeneral = OldDefaults["general"].toObject();
        OldGeneral.remove("shortcutPreset"); OldDefaults["general"] = OldGeneral;
        QVERIFY(Write(QDir(OldDirectory.path()).filePath("settings.json"), QJsonDocument(OldDefaults).toJson()));
        hc::settings::Repository Old(OldDirectory.path());
        QCOMPARE(CommandId(EffectiveShortcuts(Old.Read(hc::settings::Section::General).toObject(), {}).value(QKeyCombination(Qt::Key_F5))), "hc.navigate.refresh");
        const QJsonObject Overrides{{"F5", "cm_RereadSource"}, {"Ctrl+Alt+C", "hc.file.copy"}, {"F8", QJsonValue::Null}};
        const auto Custom = EffectiveShortcuts(General, Overrides);
        QCOMPARE(CommandId(Custom.value(QKeyCombination(Qt::Key_F5))), "hc.navigate.refresh");
        QVERIFY(CommandKeys(Custom, "cm_CopyOtherPanel").contains(QKeySequence("Ctrl+Alt+C")));
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
    void processServiceScopes()
    {
        // 보기 범위 명령은 전역 키 맵에 들어가지 않는다. hc.process.run만 전역에서도 쓸 수 있다.
        const auto Global = DefaultShortcuts(QJsonObject{{"useBuiltInShortcuts", true}});
        for (auto It = Global.begin(); It != Global.end(); ++It) QVERIFY2(!IsViewScopedCommand(CommandId(It.value())), qPrintable(It.value()));
        QVERIFY(IsViewScopedCommand("hc.process.terminate")); QVERIFY(IsViewScopedCommand("hc.service.stop"));
        QVERIFY(IsViewScopedCommand("hc.system.refreshPause")); QVERIFY(!IsViewScopedCommand("hc.process.run"));
        QCOMPARE(HomeScope("hc.service.stop"), InputScope::ServiceView); QCOMPARE(HomeScope("hc.process.copy"), InputScope::ProcessView);
        QCOMPARE(HomeScope("hc.process.run"), InputScope::Global); QCOMPARE(HomeScope("hc.file.copy"), InputScope::Global);
        QVERIFY(ScopeAccepts(InputScope::ProcessView, "hc.process.run")); QVERIFY(!ScopeAccepts(InputScope::ServiceView, "hc.process.terminate"));
        QVERIFY(ScopeAccepts(InputScope::ServiceView, "hc.system.refreshPause")); QVERIFY(!ScopeAccepts(InputScope::Global, "hc.service.start"));

        // 같은 F8이 범위마다 다른 명령을 부른다. 사용자 지정은 범위 안에서만 적용되고 null은 해제다.
        QKeyEvent F8(QEvent::KeyPress, Qt::Key_F8, Qt::NoModifier), ShiftF8(QEvent::KeyPress, Qt::Key_F8, Qt::ShiftModifier), CtrlK(QEvent::KeyPress, Qt::Key_K, Qt::ControlModifier);
        QCOMPARE(ScopeShortcut(F8, {}, InputScope::ProcessView), QString("hc.process.terminate"));
        QCOMPARE(ScopeShortcut(ShiftF8, {}, InputScope::ProcessView), QString("hc.process.terminateTree"));
        QCOMPARE(ScopeShortcut(F8, {}, InputScope::ServiceView), QString("hc.service.stop"));
        const QJsonObject Overrides{{"processView", QJsonObject{{"F8", QJsonValue::Null}, {"Ctrl+K", "hc.process.terminate"}}}};
        QVERIFY(ScopeShortcut(F8, Overrides, InputScope::ProcessView).isEmpty());
        QCOMPARE(ScopeShortcut(CtrlK, Overrides, InputScope::ProcessView), QString("hc.process.terminate"));
        QCOMPARE(ScopeShortcut(F8, Overrides, InputScope::ServiceView), QString("hc.service.stop"));
        // 다른 범위의 명령은 사용자 지정으로도 들어오지 않는다.
        QVERIFY(ScopeShortcut(CtrlK, QJsonObject{{"serviceView", QJsonObject{{"Ctrl+K", "hc.process.terminate"}}}}, InputScope::ServiceView).isEmpty());

        // 패널 전환·탭·찾기 키는 보기 범위에서 다시 정의할 수 없다.
        QVERIFY(IsProtectedScopeKey(QKeyCombination(Qt::Key_Tab))); QVERIFY(IsProtectedScopeKey(QKeyCombination(Qt::AltModifier, Qt::Key_F1)));
        QVERIFY(!IsProtectedScopeKey(QKeyCombination(Qt::Key_F8)));

        // 시스템 보기에서 살아 있는 전역 명령과 막히는 파일 명령
        for (const auto* Id : {"hc.tab.new", "hc.navigate.refresh", "hc.search.focus", "hc.panel.switch", "hc.navigate.back", "hc.process.run", "hc.panel.locationMenuLeft"})
            QVERIFY2(IsSystemViewGlobalCommand(Id), Id);
        for (const auto* Id : {"hc.file.copy", "hc.file.delete", "hc.selection.all", "hc.clipboard.paste", "hc.navigate.parent"})
            QVERIFY2(!IsSystemViewGlobalCommand(Id), Id);

        // 보기별 기능 키 막대
        const auto Processes = FunctionCommandsFor({}, FunctionBarKind::ProcessView), Services = FunctionCommandsFor({}, FunctionBarKind::ServiceView);
        QCOMPARE(Processes.size(), 9); QCOMPARE(Services.size(), 9);
        QVERIFY(Processes.contains("hc.process.terminate")); QVERIFY(Services.contains("hc.service.stop"));
        QCOMPARE(FunctionCommandsFor({}, FunctionBarKind::File), FunctionCommands({}));
        auto Custom = Processes; Custom[0] = "hc.process.copy";
        const QJsonObject General{{"functionBars", QJsonObject{{"processView", QJsonObject{{"commands", QJsonArray::fromStringList(Custom)}, {"captions", QJsonArray{"복사하기"}}}}}}};
        QCOMPARE(FunctionCommandsFor(General, FunctionBarKind::ProcessView).value(0), QString("hc.process.copy"));
        QCOMPARE(FunctionCaptionFor(General, FunctionBarKind::ProcessView, 0, "hc.process.copy", "x"), QString("복사하기"));
        QCOMPARE(FunctionCommandsFor(General, FunctionBarKind::ServiceView), Services);
    }
    void systemMenuResource()
    {
        QStringList Commands;
        for (const auto& Menu : QJsonDocument::fromJson(hc::defaults::MenuBarDefaults()).object()["menus"].toArray())
            if (Menu.toObject()["id"] == "tools")
                for (const auto& Item : Menu.toObject()["items"].toArray())
                    if (Item.toObject()["id"] == "system")
                        for (const auto& Child : Item.toObject()["items"].toArray()) if (Child.toObject().contains("command")) Commands.append(Child.toObject()["command"].toString());
        QVERIFY(Commands.contains("hc.system.processes.open")); QVERIFY(Commands.contains("hc.system.services.open"));
        for (const auto& Id : Commands) QVERIFY2(!FindCommand(Id).Name.isEmpty(), qPrintable(Id));
    }
    void defaultResource()
    {
        const auto Definition = MenuDefinition::Parse(hc::defaults::MenuBarDefaults(), {}, {"plugins", "automation"});
        QVERIFY2(Definition.error_.isEmpty(), qPrintable(Definition.error_)); QCOMPARE(Definition.menus_.size(), 9);
    }
    void stockSelectionMenuUpgradePreservesUserFile()
    {
        QTemporaryDir Directory; QMenuBar Bar; QAction Select("Select", &Bar), Toggle("Toggle", &Bar);
        QMenu Plugins("Plugins", &Bar), Scripts("Automation", &Bar);
        auto Root = QJsonDocument::fromJson(hc::defaults::MenuBarDefaults()).object(); auto Menus = Root["menus"].toArray();
        for (qsizetype I = 0; I < Menus.size(); ++I) if (Menus[I].toObject()["id"] == "select") {
            auto Old = Menus[I].toObject();
            Old["items"] = QJsonArray{QJsonObject{{"id", "invert"}, {"command", "hc.selection.invert"}, {"label", QStringLiteral("선택 반전 · 다음 항목")}}};
            Menus[I] = Old;
        }
        Root["menus"] = Menus; const auto Bytes = QJsonDocument(Root).toJson();
        const auto Path = Directory.filePath("menu-bar.json"); QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly));
        QCOMPARE(File.write(Bytes), Bytes.size()); File.close();
        UserMenu Menu(&Bar, Path, hc::defaults::MenuBarDefaults(), {{"hc.selection.select", &Select}, {"hc.selection.invert", &Toggle}}, {{"plugins", &Plugins}, {"automation", &Scripts}});
        QVERIFY(Menu.Reload()); QVERIFY(Bar.findChild<QAction*>("hc.userMenu.select-items"));
        QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), Bytes);
    }
};
QTEST_MAIN(CommandUiTests)
#include "tstCommandUi.moc"
