#include "stdafx.h"
#include "UniqueLibs/passwords/passwordUi.hpp"

/// Product UI
#include "UniqueLibs/qtArchivePassword.hpp"
#include <fmstyle/WatercolorStyle.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/WindowChrome.h>

/// Qt
#include <QtTest>
#include <QtWidgets>
#include <QtConcurrent>

using namespace hc::passwords;
using namespace hc::storage;
namespace
{
    QByteArray read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
    bool put(const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    void fillPrompt(const QString& Value, int Scope = 0)
    {
        QTimer::singleShot(0, qApp, [Value, Scope] {
            auto* Dialog = qobject_cast<QDialog*>(qApp->activeModalWidget());
            if (!Dialog) return;
            if (auto* Edit = Dialog->findChild<QLineEdit*>("hc.archive.passwordValue")) Edit->setText(Value);
            if (auto* Radio = Dialog->findChild<QRadioButton*>("hc.archive.passwordScope" + QString::number(Scope))) Radio->setChecked(true);
            if (auto* Buttons = Dialog->findChild<QDialogButtonBox*>()) Buttons->button(QDialogButtonBox::Ok)->click();
        });
    }
}
class PasswordTests final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qApp->setStyle(new fm::style::WatercolorStyle);
        fm::style::ThemeManager::instance().setDesign(fm::style::Design::Watercolor);
        qApp->setFont(QFont("Malgun Gothic", 10));
        fm::style::ThemeManager::instance().setScheme(fm::style::ThemeManager::Scheme::Light);
    }
    void init() { ForgetSessionPasswords(); }
    void accountProtection()
    {
        QTemporaryDir Dir; Store Value(Dir.filePath("passwords")); QVERIFY(Value.Load());
        QVERIFY(Value.Add("한글 이름", "한글 암호 !")); QVERIFY(Value.Save());
        const auto Bytes = read(Value.Path()); QVERIFY(!Bytes.contains("한글")); QVERIFY(!Bytes.contains("!"));
        Store Reloaded(Value.Path()); QVERIFY(Reloaded.Load()); QVERIFY(!Reloaded.Locked()); QVERIFY(!Reloaded.HasMaster());
        QCOMPARE(Reloaded.Value(0), QString("한글 암호 !")); QCOMPARE(Reloaded.Entries()[0].name_, QString("한글 이름"));
        QVERIFY(!Value.Add("", "secret")); QCOMPARE(Value.Entries().size(), 1);
        QVERIFY(!Value.Edit(0, "test", "", false)); QCOMPARE(Value.Value(0), QString("한글 암호 !"));
    }
    void optionalMasterLifecycle()
    {
        QTemporaryDir Dir; Store Value(Dir.filePath("passwords")); QVERIFY(Value.Load());
        QVERIFY(Value.Add("backup", "archive-secret")); QVERIFY(Value.SetMaster("마스터 secret")); QVERIFY(Value.Save());
        Store Locked(Value.Path()); QVERIFY(Locked.Load()); QVERIFY(Locked.Locked()); QVERIFY(Locked.HasMaster()); QVERIFY(Locked.Entries().isEmpty());
        QVERIFY(!Locked.Unlock("wrong")); QVERIFY(!Locked.Save()); QVERIFY(Locked.Unlock("마스터 secret")); QCOMPARE(Locked.Value(0), QString("archive-secret"));
        QVERIFY(Locked.SetMaster("new master")); QVERIFY(Locked.Save()); Locked.Lock();
        QVERIFY(!Locked.Unlock("마스터 secret")); QVERIFY(Locked.Unlock("new master"));
        QVERIFY(Locked.SetMaster({})); QVERIFY(Locked.Save()); Store Plain(Locked.Path()); QVERIFY(Plain.Load()); QVERIFY(!Plain.HasMaster()); QCOMPARE(Plain.Value(0), QString("archive-secret"));
    }
    void unicodeCapacity()
    {
        QTemporaryDir Dir; Store Value(Dir.filePath("passwords")); QVERIFY(Value.Load()); const QString Secret(4096, QChar(0xAC00));
        for (int Row = 0; Row < 100; ++Row) QVERIFY(Value.Add(QString::number(Row), Secret));
        QVERIFY(!Value.Add("overflow", "secret")); QVERIFY(Value.Save());
        Store Reloaded(Value.Path()); QVERIFY(Reloaded.Load()); QCOMPARE(Reloaded.Entries().size(), 100); QCOMPARE(Reloaded.Value(99), Secret);
    }
    void corruptionAndConflicts()
    {
        QTemporaryDir Dir; Store First(Dir.filePath("passwords")); QVERIFY(First.Load()); QVERIFY(First.Add("one", "secret")); QVERIFY(First.Save());
        Store Other(First.Path()); QVERIFY(Other.Load()); QVERIFY(First.Add("two", "other")); QVERIFY(First.Save());
        const auto Before = read(First.Path()); QVERIFY(Other.Add("conflict", "value")); QVERIFY(!Other.Save()); QCOMPARE(read(First.Path()), Before);
        auto Object = QJsonDocument::fromJson(Before).object(); auto Cipher = QByteArray::fromBase64(Object["data"].toString().toLatin1()); Cipher[Cipher.size() / 2] ^= 1;
        Object["data"] = QString::fromLatin1(Cipher.toBase64()); QVERIFY(put(First.Path(), QJsonDocument(Object).toJson()));
        Store Corrupt(First.Path()); QVERIFY(!Corrupt.Load()); QVERIFY(Corrupt.Locked()); QVERIFY(!Corrupt.Save());
    }
    void masterAuthentication()
    {
        QTemporaryDir Dir; Store Value(Dir.filePath("passwords")); QVERIFY(Value.Load()); QVERIFY(Value.Add("private", "value"));
        QVERIFY(Value.SetMaster("master")); QVERIFY(Value.Save()); auto Object = QJsonDocument::fromJson(read(Value.Path())).object();
        // Reprotect modified ciphertext with the same account: the GCM tag must still reject it.
        auto Cipher = Store::Reveal(QByteArray::fromBase64(Object["data"].toString().toLatin1())); QVERIFY(!Cipher.isEmpty()); Cipher[0] ^= 1;
        Object["data"] = QString::fromLatin1(Store::Protect(Cipher).toBase64()); QVERIFY(put(Value.Path(), QJsonDocument(Object).toJson()));
        Store Corrupt(Value.Path()); QVERIFY(Corrupt.Load()); QVERIFY(!Corrupt.Unlock("master")); QVERIFY(Corrupt.Entries().isEmpty());
    }
    void settingsDraftAndDialog()
    {
        QTemporaryDir Dir; Store Value(Dir.filePath("passwords")); QVERIFY(Value.Load()); QVERIFY(Value.Add("개인 백업", "secret")); QVERIFY(Value.Save()); RuntimeStore() = Value;
        {
            SettingsPage Page;
            auto* Automatic = Page.findChild<QCheckBox*>("hc.passwords.automatic"); QVERIFY(Automatic); Automatic->setChecked(false);
            QVERIFY(RuntimeStore().Automatic()); // Cancel destroys the draft.
        }
        SettingsPage Page; QVERIFY(Page.findChild<QCheckBox*>("hc.passwords.automatic")->isChecked());
        Page.findChild<QCheckBox*>("hc.passwords.automatic")->setChecked(false); QVERIFY(Page.Apply()); QVERIFY(!RuntimeStore().Automatic());
        ArchivePasswordDialog Dialog("D:/Backup/test.7z", 1); auto* Saved = Dialog.findChild<QComboBox*>("hc.archive.savedPassword");
        QCOMPARE(Saved->count(), 2); Saved->setCurrentIndex(1); QCOMPARE(Dialog.Value(), QString("secret"));
        QCOMPARE(Dialog.findChild<QLineEdit*>("hc.archive.passwordValue")->echoMode(), QLineEdit::Password); QCOMPARE(Dialog.Scope(), 0);
    }
    void automaticAndCreation()
    {
        QTemporaryDir Dir; Store Value(Dir.filePath("passwords")); QVERIFY(Value.Load()); QVERIFY(Value.Add("first", "bad")); QVERIFY(Value.Add("second", "secret")); QVERIFY(Value.Save()); RuntimeStore() = Value;
        const auto Archive = Dir.filePath("existing.7z"); QVERIFY(put(Archive, "archive"));
        auto First = AcquireArchivePassword(Archive.toStdString(), 1, {}); QVERIFY(First.HasValue()); QCOMPARE(First.Value().Value(), std::string("bad"));
        auto Second = AcquireArchivePassword(Archive.toStdString(), 2, {}); QVERIFY(Second.HasValue()); QCOMPARE(Second.Value().Value(), std::string("secret"));
        fillPrompt("manual"); auto Manual = AcquireArchivePassword(Archive.toStdString(), 3, {}); QVERIFY(Manual.HasValue()); QCOMPARE(Manual.Value().Value(), std::string("manual"));
        QTimer::singleShot(0, qApp, [] { if (auto* Dialog = qobject_cast<QDialog*>(qApp->activeModalWidget())) Dialog->reject(); });
        auto Create = AcquireArchivePassword(Dir.filePath("new.zip").toStdString(), 1, {}); QVERIFY(!Create.HasValue()); QCOMPARE(Create.Failure().category_, ErrorCategory::Cancelled);
    }
    void settingsMasterControls()
    {
        QTemporaryDir Dir; Store Value(Dir.filePath("passwords")); QVERIFY(Value.Load()); QVERIFY(Value.Add("backup", "secret")); QVERIFY(Value.Save()); RuntimeStore() = Value;
        SettingsPage Page;
        QTimer::singleShot(0, qApp, [] {
            auto* Dialog = qobject_cast<QDialog*>(qApp->activeModalWidget()); if (!Dialog) return;
            Dialog->findChild<QLineEdit*>("hc.passwords.newMaster")->setText("master");
            Dialog->findChild<QLineEdit*>("hc.passwords.confirmMaster")->setText("master");
            Dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        Page.findChild<QCheckBox*>("hc.passwords.master")->click(); QVERIFY(Page.Apply()); QVERIFY(RuntimeStore().HasMaster());
        SettingsPage Other; QCOMPARE(Other.findChild<QTableWidget*>("hc.passwords.table")->rowCount(), 1);
        Page.findChild<QPushButton*>("hc.passwords.unlock")->click(); QVERIFY(RuntimeStore().Locked()); QCOMPARE(Page.findChild<QTableWidget*>("hc.passwords.table")->rowCount(), 0);
        QCOMPARE(Other.findChild<QTableWidget*>("hc.passwords.table")->rowCount(), 0);
        QTimer::singleShot(0, qApp, [] {
            auto* Dialog = qobject_cast<QDialog*>(qApp->activeModalWidget()); if (!Dialog) return;
            Dialog->findChild<QLineEdit*>("hc.passwords.masterInput")->setText("master");
            Dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        Page.findChild<QPushButton*>("hc.passwords.unlock")->click(); QVERIFY(!RuntimeStore().Locked()); QCOMPARE(Page.findChild<QTableWidget*>("hc.passwords.table")->rowCount(), 1);
        QTimer::singleShot(0, qApp, [] {
            auto* Dialog = qobject_cast<QDialog*>(qApp->activeModalWidget()); if (!Dialog) return;
            Dialog->findChild<QLineEdit*>("hc.passwords.oldMaster")->setText("master");
            Dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        Page.findChild<QCheckBox*>("hc.passwords.master")->click(); QVERIFY(Page.Apply()); QVERIFY(!RuntimeStore().HasMaster());
        QCOMPARE(RuntimeStore().Value(0), QString("secret"));
    }
    void scopesAndCancellation()
    {
        QTemporaryDir Dir; Store Value(Dir.filePath("passwords")); QVERIFY(Value.Load()); Value.SetAutomatic(false); QVERIFY(Value.Save()); RuntimeStore() = Value;
        const auto Archive = Dir.filePath("existing.egg"); QVERIFY(put(Archive, "archive"));
        std::stop_source Source; OperationContext Context; Context.stopToken_ = Source.get_token();
        fillPrompt("task secret"); auto Prompt = AcquireArchivePassword(Archive.toStdString(), 1, Context); QVERIFY(Prompt.HasValue());
        auto Cached = AcquireArchivePassword(Archive.toStdString(), 1, Context); QVERIFY(Cached.HasValue()); QCOMPARE(Cached.Value().Value(), std::string("task secret"));
        Source.request_stop(); auto Cancelled = AcquireArchivePassword(Archive.toStdString(), 1, Context); QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled);
        std::stop_source Other; Context.stopToken_ = Other.get_token();
        QTimer::singleShot(20, qApp, [Other]() mutable { Other.request_stop(); });
        auto Waiting = AcquireArchivePassword(Archive.toStdString(), 1, Context); QVERIFY(!Waiting.HasValue()); QCOMPARE(Waiting.Failure().category_, ErrorCategory::Cancelled);
        auto Limited = AcquireArchivePassword(Archive.toStdString(), 4, {}); QVERIFY(!Limited.HasValue()); QCOMPARE(Limited.Failure().category_, ErrorCategory::AccessDenied);
        fillPrompt("session secret", 1); auto Session = AcquireArchivePassword(Archive.toStdString(), 1, {}); QVERIFY(Session.HasValue());
        auto Reused = AcquireArchivePassword(Archive.toStdString(), 1, {}); QVERIFY(Reused.HasValue()); QCOMPARE(Reused.Value().Value(), std::string("session secret"));
    }
    void lockedDirectInputAndSaving()
    {
        QTemporaryDir Dir; Store Value(Dir.filePath("passwords")); QVERIFY(Value.Load()); QVERIFY(Value.Add("private", "saved")); QVERIFY(Value.SetMaster("master")); QVERIFY(Value.Save()); Value.Lock(); RuntimeStore() = Value;
        const auto Archive = Dir.filePath("existing.zip"); QVERIFY(put(Archive, "archive"));
        fillPrompt("direct"); auto Direct = AcquireArchivePassword(Archive.toStdString(), 1, {}); QVERIFY(Direct.HasValue()); QCOMPARE(Direct.Value().Value(), std::string("direct")); QVERIFY(RuntimeStore().Locked());
        QVERIFY(RuntimeStore().Unlock("master")); RuntimeStore().SetAutomatic(false);
        fillPrompt("saved from prompt", 2); auto Saved = AcquireArchivePassword(Archive.toStdString(), 1, {}); QVERIFY(Saved.HasValue());
        Store Reloaded(Value.Path()); QVERIFY(Reloaded.Load()); QVERIFY(Reloaded.Unlock("master")); QCOMPARE(Reloaded.Entries().size(), 2); QCOMPARE(Reloaded.Value(1), QString("saved from prompt"));
    }
    void cancelNestedUnlock()
    {
        QTemporaryDir Dir; Store Value(Dir.filePath("passwords")); QVERIFY(Value.Load()); QVERIFY(Value.Add("private", "secret")); QVERIFY(Value.SetMaster("master")); QVERIFY(Value.Save()); Value.Lock(); RuntimeStore() = Value;
        const auto Archive = Dir.filePath("encrypted.7z"); QVERIFY(put(Archive, "archive")); std::stop_source Source; OperationContext Context; Context.stopToken_ = Source.get_token();
        bool NestedOpened = false;
        QTimer::singleShot(0, qApp, [&] {
            auto* Dialog = qobject_cast<QDialog*>(qApp->activeModalWidget()); if (!Dialog) { Source.request_stop(); return; }
            QTimer::singleShot(20, qApp, [&] { NestedOpened = qApp->activeModalWidget() && qApp->activeModalWidget()->objectName() == "hc.passwords.unlock"; Source.request_stop(); });
            for (auto* Button : Dialog->findChildren<QPushButton*>()) if (Button->text().startsWith("저장소 잠금 해제")) { Button->click(); break; }
        });
        auto Cancelled = AcquireArchivePassword(Archive.toStdString(), 1, Context); QVERIFY(NestedOpened); QVERIFY(!Cancelled.HasValue()); QCOMPARE(Cancelled.Failure().category_, ErrorCategory::Cancelled);
        QVERIFY(!qApp->activeModalWidget()); QVERIFY(RuntimeStore().Locked());
    }
    void renderScreens()
    {
        const auto Out = qEnvironmentVariable("HC_PASSWORD_CAPTURES"); if (Out.isEmpty()) return;
        QVERIFY(QDir().mkpath(Out)); QTemporaryDir Dir; Store Value(Dir.filePath("passwords")); QVERIFY(Value.Load());
        QVERIFY(Value.Add("개인 백업", "sample-password")); QVERIFY(Value.Add("공유 자료", "sample-other")); QVERIFY(Value.Add("이전 백업", "old", false)); RuntimeStore() = Value;
        auto& Theme = fm::style::ThemeManager::instance();
        for (bool Dark : {false, true}) {
            Theme.setScheme(Dark ? fm::style::ThemeManager::Scheme::Dark : fm::style::ThemeManager::Scheme::Light);
            QDialog Dialog(nullptr, Qt::FramelessWindowHint); Dialog.setWindowTitle("환경설정"); Dialog.resize(1012, 718);
            auto* Rows = new QVBoxLayout(&Dialog); Rows->setContentsMargins(0, 0, 0, 0); Rows->addWidget(new fm::ui::WindowChrome(&Dialog, &Dialog, {false, true}));
            auto* Body = new QHBoxLayout; auto* Sidebar = new QListWidget(&Dialog); Sidebar->setFixedWidth(210);
            Sidebar->addItems({"일반 · 모양", "테마 색상", "파일 패널", "파일 작업", "비밀번호 관리", "키보드", "관리자 권한", "플러그인 관리", "탐색", "메뉴바", "실험실", "레거시"}); Sidebar->setCurrentRow(4); Body->addWidget(Sidebar);
            Body->addWidget(new SettingsPage(&Dialog), 1); Rows->addLayout(Body, 1);
            auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, &Dialog); Rows->addWidget(Buttons);
            Dialog.show(); QTest::qWait(100); QVERIFY(Dialog.grab().save(QDir(Out).filePath(Dark ? "settings-dark.png" : "settings-light.png"))); Dialog.hide();
            ArchivePasswordDialog Prompt("D:/Backup/가족사진_2026.7z", 1); Prompt.show(); QTest::qWait(100); QVERIFY(Prompt.grab().save(QDir(Out).filePath(Dark ? "prompt-dark.png" : "prompt-light.png")));
        }
    }
};
QTEST_MAIN(PasswordTests)
#include "tstPasswords.moc"
