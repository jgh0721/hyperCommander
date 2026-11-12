#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QLockFile>
#include <QPushButton>
#include <QScopeGuard>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QUuid>
#include "text/QTextView.h"
#include "commonLibs/document/epub/epubArchive.hpp"
#include "UniqueLibs/ProductDefaults_Edit.hpp"
#include "UniqueLibs/document/archiveDraftDirectory.hpp"
#include "UniqueLibs/document/documentRecoveryMaintenance.hpp"
#include "UniqueLibs/document/documentRecoveryStore.hpp"
#include "UniqueLibs/document/documentSettingsDialog.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/recoveryPromptDialog.hpp"
#include "UniqueLibs/settings/unifiedConfig.hpp"
#include "UniqueLibs/temporaryCleanup.hpp"
#include "UniqueLibs/document/rst/sphinxPreviewController.hpp"
#include <QSignalSpy>

using namespace hc::document;
namespace
{
    bool Put(const QString& Path, const QByteArray& Bytes)
    { QDir().mkpath(QFileInfo(Path).absolutePath()); QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    QByteArray Bytes(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
    bool Age(const QString& Path, int Days)
    {
        QFile File(Path); if (!File.open(QIODevice::ReadWrite)) return false;
        return File.setFileTime(QDateTime::currentDateTime().addDays(-Days), QFileDevice::FileModificationTime);
    }
    QString Id() { return QUuid::createUuid().toString(QUuid::Id128); }
    QByteArray Sha(const QByteArray& Data) { return QCryptographicHash::hash(Data, QCryptographicHash::Sha256).toHex(); }
    RecoverySnapshot Snapshot(const QString& Document, const std::string& Bytes, const QString& Owner = {}, const QString& Source = {}, bool Untitled = false)
    {
        RecoverySnapshot Record; Record.document_.id_ = Document.toStdString(); Record.document_.revision_ = 1; Record.bytes_ = Bytes;
        Record.document_.source_.localPath_ = Source.toStdString();
        TextRecoveryState Text; Text.untitled_ = Untitled; Text.untitledNumber_ = Untitled ? 3 : 0; Record.text_ = Text;
        Record.ownerWindow_ = Owner.toStdString(); return Record;
    }
    QJsonObject Item(const RecoverySnapshot& Stored, const QString& Path)
    {
        return {{"documentId", QString::fromStdString(Stored.document_.id_)}, {"path", Path},
            {"snapshot", QFileInfo(QString::fromStdString(Stored.snapshotPath_)).fileName()}, {"sha256", QString::fromStdString(Stored.checksum_)},
            {"generation", QString::number(Stored.document_.generation_)}, {"revision", QString::number(Stored.document_.revision_)}};
    }
    QStringList Ids(DocumentRecoveryStore& Store)
    {
        QStringList Result; auto Listed = Store.List();
        if (Listed.HasValue()) for (const auto& Record : Listed.Value()) Result.append(QString::fromStdString(Record.document_.id_));
        Result.sort(); return Result;
    }
    QByteArray Zip(const QList<QPair<QString, QByteArray>>& Entries)
    {
        epub::detail::StoredZipWriter Writer({});
        for (const auto& [Name, Data] : Entries) Writer.Add(Name, Data);
        return Writer.Finish();
    }
}

class DocumentRecoveryTests final : public QObject
{
    Q_OBJECT
private slots:
    void zipWriteBack()
    {
        QTemporaryDir Temp; const auto Archive = Temp.filePath(QString::fromUtf8("자료.zip"));
        QVERIFY(Put(Archive, Zip({{QString::fromUtf8("docs/보고서.md"), "base"}, {"other.txt", "keep"}})));
        const auto Root = Temp.filePath("recovery/archive-drafts");
        const auto Draft = [&](const QByteArray& Baseline, int Nesting = 1, const QString& Path = {}) {
            ArchiveDraftDirectory Directory(Root); Directory.setAutoRemove(false);
            Put(Directory.filePath(QString::fromUtf8("content/보고서.md")), "edited");
            WriteArchiveDraftManifest(Directory.path(), {{"schemaVersion", 2}, {"purpose", "archive-preview-edit"}, {"name", QString::fromUtf8("보고서.md")},
                {"archivePath", Path.isEmpty() ? Archive : Path}, {"entryPath", QString::fromUtf8("/docs/보고서.md")}, {"nesting", Nesting},
                {"baselineSha256", QString::fromLatin1(Baseline)}, {"pending", true}});
            return Directory.path();
        };
        // The archive changed since the edit began: nothing is written.
        const auto Stale = Draft(Sha("other"));
        QCOMPARE(recovery::ApplyArchiveDraft(Stale), QString("archive-entry-changed")); QVERIFY(QFileInfo::exists(Stale));
        QVERIFY(recovery::RemoveArchiveDraft(Stale));
        // A nested or non-ZIP source is left to HyperCommander.
        QCOMPARE(recovery::ArchiveApplyBlocker(recovery::ReadArchiveManifest(Draft(Sha("base"), 2))), QString("archive-nested"));
        QVERIFY(Put(Temp.filePath("plain.7z"), "7z\xBC\xAF\x27\x1C"));
        QCOMPARE(recovery::ArchiveApplyBlocker(recovery::ReadArchiveManifest(Draft(Sha("base"), 1, Temp.filePath("plain.7z")))), QString("archive-format"));
        for (const auto& Folder : QDir(Root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) QDir(Folder.absoluteFilePath()).removeRecursively();
        // The recovery list shows a live copy as in use and an ended one as pending, writable.
        {
            ArchiveDraftDirectory Live(Root); Put(Live.filePath("content/a.txt"), "live");
            WriteArchiveDraftManifest(Live.path(), {{"name", "a.txt"}, {"archivePath", Archive}, {"entryPath", "/a.txt"}, {"nesting", 1}, {"pending", true}});
            const auto Items = recovery::ListItems(Temp.filePath("recovery"), {}, QDateTime::currentDateTime());
            QCOMPARE(Items.size(), std::size_t(1)); QCOMPARE(Items.front().state_, recovery::State::InUse);
        }
        const auto Ready = Draft(Sha("base"));
        const auto Items = recovery::ListItems(Temp.filePath("recovery"), {}, QDateTime::currentDateTime());
        QCOMPARE(Items.size(), std::size_t(1)); QCOMPARE(Items.front().state_, recovery::State::Pending);
        QVERIFY(Items.front().applyBlocker_.isEmpty()); QVERIFY(Items.front().location_.contains(QString::fromUtf8("자료.zip")));
        QCOMPARE(recovery::ApplyArchiveDraft(Ready), QString{}); QVERIFY(!QFileInfo::exists(Ready));
        const epub::detail::Archive Updated(Bytes(Archive), {}, epub::InputLimit, epub::detail::ZipMode::General);
        QCOMPARE(Updated.Read(QString::fromUtf8("docs/보고서.md")), QByteArray("edited"));
        QCOMPARE(Updated.Read("other.txt"), QByteArray("keep"));
    }
    void retentionAndCapacity()
    {
        QTemporaryDir Temp; const auto Root = Temp.filePath("recovery"); DocumentRecoveryStore Store(Root);
        const auto Old = Store.Store(Snapshot(Id(), "old")).Value(), Recent = Store.Store(Snapshot(Id(), "recent")).Value();
        const auto Pinned = Store.Store(Snapshot(Id(), "pinned")).Value();
        QVERIFY(Age(QString::fromStdString(Old.snapshotPath_), 40)); QVERIFY(Age(QString::fromStdString(Pinned.snapshotPath_), 40));
        // A HotExit session waits for the next start however old it is.
        const auto Window = Id();
        QVERIFY(Store.StoreSession(Window, {{"profileId", "p"}, {"cleanShutdown", true}, {"documents", QJsonArray{Item(Pinned, "C:/a.txt")}}}).HasValue());
        recovery::Policy Settings; Settings.retentionDays_ = 30;
        auto Report = recovery::Purge(Root, Settings, QDateTime::currentDateTime(), recovery::PurgeScope::Expired);
        QCOMPARE(Report.removed_, 1);
        QStringList Expected{QString::fromStdString(Recent.document_.id_), QString::fromStdString(Pinned.document_.id_)}; Expected.sort();
        QCOMPARE(Ids(Store), Expected);
        // Over the size cap the oldest unpinned copy goes, but never one written in the last day.
        Settings.maxBytes_ = 1;
        recovery::Purge(Root, Settings, QDateTime::currentDateTime(), recovery::PurgeScope::Startup);
        QCOMPARE(Ids(Store), Expected);
        QVERIFY(Age(QString::fromStdString(Recent.snapshotPath_), 2));
        recovery::Purge(Root, Settings, QDateTime::currentDateTime(), recovery::PurgeScope::Startup);
        QCOMPARE(Ids(Store), QStringList{QString::fromStdString(Pinned.document_.id_)});
        // Unlimited keeps everything.
        Settings.retentionDays_ = 0; Settings.maxBytes_ = 0; QVERIFY(Age(QString::fromStdString(Pinned.snapshotPath_), 4000));
        QVERIFY(Store.RemoveSession(Window));
        QCOMPARE(recovery::Purge(Root, Settings, QDateTime::currentDateTime(), recovery::PurgeScope::Startup).removed_, 0);
    }
    void leftoversAndFences()
    {
        QTemporaryDir Temp; const auto Root = Temp.filePath("recovery"); DocumentRecoveryStore Store(Root);
        const auto Kept = Store.Store(Snapshot(Id(), "kept")).Value();
        const auto Gone = Store.Store(Snapshot(Id(), "gone")).Value();
        QVERIFY(Store.RemoveThrough(Gone.document_.id_, 1, 1).HasValue());
        const auto Fence = Root + "/" + QString::fromStdString(Gone.document_.id_) + ".retired"; QVERIFY(QFileInfo::exists(Fence));
        const auto Interrupted = Root + "/index.json.AbCdEf", OldOriginal = Root + "/untitled-" + Id() + ".txt", NewOriginal = Root + "/capture-" + Id() + ".png";
        QVERIFY(Put(Interrupted, "x")); QVERIFY(Put(OldOriginal, "")); QVERIFY(Put(NewOriginal, ""));
        QVERIFY(Age(Fence, 1)); QVERIFY(Age(Interrupted, 1)); QVERIFY(Age(OldOriginal, 40));
        // An abandoned window session without documents and a lock of an ended window go too.
        const auto Empty = Id(); QVERIFY(Store.StoreSession(Empty, {{"profileId", "p"}, {"documents", QJsonArray{}}}).HasValue());
        recovery::Purge(Root, {}, QDateTime::currentDateTime(), recovery::PurgeScope::Startup);
        QVERIFY(!QFileInfo::exists(Fence)); QVERIFY(!QFileInfo::exists(Interrupted)); QVERIFY(!QFileInfo::exists(OldOriginal));
        QVERIFY(QFileInfo::exists(NewOriginal)); QVERIFY(!Store.Sessions().contains(Empty));
        QCOMPARE(Ids(Store), QStringList{QString::fromStdString(Kept.document_.id_)});
    }
    void abnormalExitSessions()
    {
        QTemporaryDir Temp; const auto Root = Temp.filePath("recovery"); DocumentRecoveryStore Store(Root);
        const auto Window = Id(), Other = Id(), Live = Id();
        const auto Edited = Store.Store(Snapshot(Id(), "edit", Window, "C:/docs/a.txt")).Value();
        const auto Untitled = Store.Store(Snapshot(Id(), "new", Window, {}, true)).Value();
        QVERIFY(Store.StoreSession(Window, {{"profileId", "p"}, {"cleanShutdown", false}, {"documents", QJsonArray{
            Item(Edited, "C:/docs/a.txt"), Item(Untitled, {}), QJsonObject{{"documentId", Id()}, {"path", "C:/docs/clean.txt"}}}}}).HasValue());
        QVERIFY(Store.StoreSession(Other, {{"profileId", "q"}, {"cleanShutdown", false}, {"documents", QJsonArray{Item(Edited, "C:/docs/a.txt")}}}).HasValue());
        QVERIFY(Store.StoreSession(Live, {{"profileId", "p"}, {"cleanShutdown", false}, {"documents", QJsonArray{Item(Edited, "C:/docs/a.txt")}}}).HasValue());
        QLockFile Running(Store.SessionLockPath(Live)); QVERIFY(Running.tryLock());
        const auto Groups = recovery::CrashGroups(Root, "p");
        QCOMPARE(Groups.size(), std::size_t(1)); QCOMPARE(Groups.front().sessionId_, Window); QCOMPARE(Groups.front().clean_, 1);
        QCOMPARE(Groups.front().entries_.size(), std::size_t(2));
        QCOMPARE(Groups.front().entries_[0].name_, QString("a.txt")); QCOMPARE(Groups.front().entries_[1].type_, recovery::PromptEntry::Untitled);
        QCOMPARE(Groups.front().entries_[1].name_, QString::fromUtf8("새 문서 3"));
        // 삭제 removes the checked copy; the other stays in the list and the session ends.
        QVERIFY(recovery::ReleaseSession(Root, Window, {QString::fromStdString(Edited.document_.id_)}));
        QVERIFY(!Store.Sessions().contains(Window)); QCOMPARE(Ids(Store), QStringList{QString::fromStdString(Untitled.document_.id_)});
        QVERIFY(!recovery::ReleaseSession(Root, Live));
        // Removing a copy from the list also drops it from a session that would reopen it.
        const auto HotExit = Id(); QVERIFY(Store.StoreSession(HotExit, {{"profileId", "p"}, {"cleanShutdown", true}, {"documents", QJsonArray{Item(Untitled, {})}}}).HasValue());
        auto Items = recovery::ListItems(Root, {}, QDateTime::currentDateTime());
        QCOMPARE(Items.size(), std::size_t(1)); QCOMPARE(Items.front().state_, recovery::State::HotExit);
        QCOMPARE(recovery::RemoveCopy(Root, Items.front()), QString{});
        QVERIFY(Ids(Store).isEmpty()); QVERIFY(Store.ReadSession(HotExit).Value()["documents"].toArray().isEmpty());
    }
    void migrationFromLegacyFolders()
    {
        QTemporaryDir Temp; const auto Config = Temp.filePath("config"), Legacy = Temp.filePath("local/HyperEditor/recovery/editor");
        DocumentRecoveryStore Old(Legacy);
        const auto Mine = Id(), Theirs = Id();
        const auto Owned = Old.Store(Snapshot(Id(), "mine", Mine)).Value(), Foreign = Old.Store(Snapshot(Id(), "theirs", Theirs)).Value();
        const auto Orphan = Old.Store(Snapshot(Id(), "orphan")).Value();
        QVERIFY(Old.StoreSession(Mine, {{"profileId", "p"}, {"cleanShutdown", true}, {"documents", QJsonArray{Item(Owned, "C:/a.txt")}}}).HasValue());
        QVERIFY(Old.StoreSession(Theirs, {{"profileId", "q"}, {"cleanShutdown", true}, {"documents", QJsonArray{Item(Foreign, "C:/b.txt")}}}).HasValue());
        QVERIFY(Put(Config + "/epub-drafts/" + Id() + ".hcedraft", "draft"));
        QVERIFY(Put(Temp.filePath("tmp/hc-preview-edit-AbCdEf/content/r.md"), "edited"));
        QVERIFY(WriteArchiveDraftManifest(Temp.filePath("tmp/hc-preview-edit-AbCdEf"), {{"name", "r.md"}, {"pending", true}}));
        QStringList All{QString::fromStdString(Owned.document_.id_), QString::fromStdString(Foreign.document_.id_), QString::fromStdString(Orphan.document_.id_)}; All.sort();
        // Into a folder that disappears afterwards: the shared folders still have everything.
        {
            QTemporaryDir Vanishing;
            QCOMPARE(recovery::MigrateLegacy(Vanishing.path(), "p", {Legacy}, Temp.filePath("tmp")).moved_, 3);   // no EPUB drafts there
        }
        QCOMPARE(Ids(Old), All); QCOMPARE(QDir(Legacy + "/sessions").entryList({"*.json"}).size(), 2);
        QVERIFY(QFileInfo::exists(Temp.filePath("tmp/hc-preview-edit-AbCdEf/content/r.md")));
        QFile::rename(Temp.filePath("tmp/hc-preview-edit-AbCdEf/recovery.migrated.json"), Temp.filePath("tmp/hc-preview-edit-AbCdEf/recovery.json"));
        const auto Report = recovery::MigrateLegacy(Config, "p", {Legacy}, Temp.filePath("tmp"));
        QCOMPARE(Report.moved_, 4);
        const auto Root = hc::defaults::edit::RecoveryDirectory(Config); DocumentRecoveryStore Moved(Root);
        QStringList Expected{QString::fromStdString(Owned.document_.id_), QString::fromStdString(Orphan.document_.id_)}; Expected.sort();
        QCOMPARE(Ids(Moved), Expected); QCOMPARE(Moved.Sessions(), QStringList{Mine});
        QCOMPARE(Ids(Old), All); QVERIFY(QFileInfo::exists(Legacy + "/migrated.json"));
        QCOMPARE(QDir(Root + "/epub-drafts").entryList({"*.hcedraft"}).size(), 1); QVERIFY(!QFileInfo::exists(Config + "/epub-drafts"));
        QCOMPARE(QDir(Root + "/archive-drafts").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
        QVERIFY(!recovery::MigrationNotice(Root).isEmpty()); QCOMPARE(recovery::MigrationNotice(Root)["count"].toInt(), 4);
        QVERIFY(recovery::DismissMigrationNotice(Root)); QVERIFY(recovery::MigrationNotice(Root).isEmpty());
        // A second run copies nothing again.
        QCOMPARE(recovery::MigrateLegacy(Config, "p", {Legacy}, Temp.filePath("tmp")).moved_, 0);
        // After the grace period the old folder gives up only what was copied; another installation's copies stay.
        QVERIFY(Old.RetireImported(QDateTime::currentDateTimeUtc().addDays(1)).HasValue());
        QCOMPARE(Ids(Old), QStringList{QString::fromStdString(Foreign.document_.id_)}); QCOMPARE(Old.Sessions(), QStringList{Theirs});
        QCOMPARE(Ids(Moved), Expected);
    }
    void restartPromptChoices()
    {
        recovery::PromptGroup Group; Group.sessionId_ = Id(); Group.saved_ = QDateTime::currentDateTime(); Group.clean_ = 2;
        Group.entries_ = {{recovery::PromptEntry::Edit, "a", "a.txt", "C:\\docs"}, {recovery::PromptEntry::Untitled, "b", QString::fromUtf8("새 문서 1"), {}}};
        RecoveryPromptDialog Dialog({Group}, 30);
        auto* Restore = Dialog.findChild<QPushButton*>("recoveryPrompt.restore"); QVERIFY(Restore);
        QCOMPARE(Dialog.Selected(), (QStringList{"a", "b"})); QCOMPARE(Restore->text(), QString::fromUtf8("복구"));
        Dialog.findChild<QCheckBox*>("recoveryPrompt.check/b")->setChecked(false);
        QCOMPARE(Dialog.Selected(), QStringList{"a"}); QCOMPARE(Restore->text(), QString::fromUtf8("선택한 1개 복구"));
        QVERIFY(Dialog.findChild<QLabel*>("recoveryPrompt.hint")->text().contains(QString::fromUtf8("2개")));
        Dialog.findChild<QPushButton*>("recoveryPrompt.delete")->click();
        auto* Confirm = Dialog.findChild<QPushButton*>("recoveryPrompt.confirmDelete"); QVERIFY(Confirm && Confirm->isVisibleTo(&Dialog));
        QVERIFY(!Restore->isVisibleTo(&Dialog));
        Dialog.findChild<QPushButton*>("recoveryPrompt.back")->click(); QVERIFY(Restore->isVisibleTo(&Dialog));
        Dialog.findChild<QPushButton*>("recoveryPrompt.delete")->click(); Confirm->click();
        QCOMPARE(Dialog.Chosen(), RecoveryPromptDialog::Delete);
        RecoveryPromptDialog Closed({Group}, 0); Closed.reject(); QCOMPARE(Closed.Chosen(), RecoveryPromptDialog::Keep);
    }
    void partialRestoreKeepsTheRest()
    {
        QTemporaryDir Root; const auto Previous = qApp->property("documentViewsConfigDir");
        qApp->setProperty("documentViewsConfigDir", Root.path());
        const auto Reset = qScopeGuard([&] { qApp->setProperty("documentViewsConfigDir", Previous); });
        // HotExit off: the session is published for periodic recovery and still offered after an abnormal exit.
        QVERIFY(Put(hc::settings::UnifiedConfig::FileName(Root.path()), R"({"schemaVersion":2,"documentViews":{"textView":{"hotExitPolicyVersion":1,"hotExitEnabled":false,"recoveryEnabled":true}}})"));
        const auto RecoveryRoot = Root.filePath("recovery"); QString WindowId, First, Second;
        {
            auto Widget = std::make_unique<DocumentWorkspaceWidget>(RecoveryRoot);
            First = Widget->Handle({{"action", "new"}})["documentId"].toString(); Second = Widget->Handle({{"action", "new"}})["documentId"].toString();
            QTRY_VERIFY(Widget->IsIdle());
            QVERIFY(!Widget->Handle({{"action", "replaceText"}, {"documentId", First}, {"text", "first"}}).contains("error"));
            QVERIFY(!Widget->Handle({{"action", "replaceText"}, {"documentId", Second}, {"text", "second"}}).contains("error"));
            WindowId = Widget->Snapshot()["workspaceId"].toString();
            DocumentRecoveryStore Store(RecoveryRoot);
            const auto BothPreserved = [&] {
                const auto Session = Store.ReadSession(WindowId); if (!Session.HasValue()) return false;
                int Preserved = 0; for (const auto& Value : Session.Value()["documents"].toArray()) Preserved += !Value.toObject()["snapshot"].toString().isEmpty();
                return Preserved == 2;
            };
            QTRY_VERIFY_WITH_TIMEOUT(BothPreserved(), 10000);
            QVERIFY(!Store.ReadSession(WindowId).Value()["cleanShutdown"].toBool());
            // The window ends without closing: the session stays as an abnormal exit.
        }
        DocumentRecoveryStore Store(RecoveryRoot);
        QVERIFY(Store.ReadSession(WindowId).HasValue());
        DocumentWorkspaceWidget Restored(RecoveryRoot); QVERIFY(Restored.RestoreHotExit(WindowId, QStringList{First}));
        QTRY_COMPARE(Restored.Snapshot()["documents"].toArray().size(), 1);
        QTRY_VERIFY(Restored.IsIdle()); QCOMPARE(Restored.findChild<QTextView*>()->Content(), QString("first"));
        // Once the new window's session is durable the old one ends; the unchosen copy waits in the list.
        QTRY_VERIFY_WITH_TIMEOUT(!Store.ReadSession(WindowId).HasValue(), 10000);
        const auto SecondKept = [&] {
            for (const auto& Entry : recovery::ListItems(RecoveryRoot, {}, QDateTime::currentDateTime()))
                if (Entry.key_ == Second) return Entry.state_ == recovery::State::Kept;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(SecondKept(), 10000);
    }
    void settingsRecoveryPage()
    {
        QTemporaryDir Temp; const auto Previous = qApp->property("documentViewsConfigDir");
        qApp->setProperty("documentViewsConfigDir", Temp.path());
        const auto Reset = qScopeGuard([&] { qApp->setProperty("documentViewsConfigDir", Previous); });
        const auto Path = hc::settings::UnifiedConfig::FileName(Temp.path());
        QVERIFY(Put(Path, R"({"schemaVersion":2})"));
        DocumentRecoveryStore Store(hc::defaults::edit::RecoveryDirectory(Temp.path()));
        const auto Old = Store.Store(Snapshot(Id(), "old")).Value(); QVERIFY(Age(QString::fromStdString(Old.snapshotPath_), 10));
        DocumentSettingsDialog Dialog(Path, {});
        auto* List = Dialog.findChild<QListWidget*>("settingsCategories"); QVERIFY(List);
        QCOMPARE(List->item(1)->text(), QString::fromUtf8("세션 · 복구"));
        Dialog.ShowPage("recovery"); QCOMPARE(List->currentRow(), 1);
        QVERIFY(!Dialog.findChild<QCheckBox*>("epubRecoveryEnabled"));
        auto* Days = Dialog.findChild<QComboBox*>("documentViews/recovery/retentionDays"); QVERIFY(Days); QCOMPARE(Days->currentData().toInt(), 30);
        auto* Cap = Dialog.findChild<QComboBox*>("documentViews/recovery/maxMiB"); QVERIFY(Cap); QCOMPARE(Cap->currentData().toInt(), 1024);
        QVERIFY(Cap->findData(0) >= 0 && Days->findData(0) >= 0);
        auto* Action = Dialog.findChild<QComboBox*>("documentViews/recovery/startupAction"); QVERIFY(Action); QCOMPARE(Action->currentData().toString(), QString("ask"));
        auto* Usage = Dialog.findChild<QTableWidget*>("recoveryUsage"); QVERIFY(Usage);
        QTRY_COMPARE(Usage->item(0, 1) ? Usage->item(0, 1)->text() : QString{}, QString::fromUtf8("1개"));
        auto* Purge = Dialog.findChild<QPushButton*>("recoveryPurge"); QVERIFY(Purge); QVERIFY(!Purge->isEnabled());
        // A shorter period on screen shows the copy as expired before 적용.
        Days->setCurrentIndex(Days->findData(7)); QTRY_VERIFY(Purge->isEnabled());
        QVERIFY(Dialog.Apply());
        QJsonObject Saved; QString Error; QVERIFY(hc::settings::UnifiedConfig::Load(Path, Saved, Error));
        QCOMPARE(recovery::PolicyFrom(Saved).retentionDays_, 7);
        Dialog.findChild<QDialogButtonBox*>("settingsButtons")->button(QDialogButtonBox::RestoreDefaults)->click();
        QCOMPARE(Days->currentData().toInt(), 30);
    }
    void scratchSessionsAndSweep()
    {
        QTemporaryDir Temp; const auto Root = Temp.filePath("scratch");
        QString LivePath;
        {
            hc::scratch::Session Live(Root); QVERIFY(Live.IsValid()); LivePath = Live.Path();
            QVERIFY(QFileInfo(Live.Template("epubcheck")).absolutePath() == LivePath);
            QVERIFY(QFileInfo(Live.Folder("rst-shadow")).isDir());
            // Another process's folder without a lock yet (just created) and a stale one an hour old.
            const auto Fresh = Root + "/s-1-0123456789ab", Stale = Root + "/s-2-0123456789ac", Foreign = Root + "/s-not-ours";
            for (const auto& Path : {Fresh, Stale, Foreign}) QVERIFY(QDir().mkpath(Path));
            QVERIFY(Put(Stale + "/x.tmp", "x"));
            // A folder whose owner still runs keeps its lock.
            const auto Held = Root + "/s-3-0123456789ad"; QVERIFY(QDir().mkpath(Held));
            QLockFile Owner(Held + "/owner.lock"); QVERIFY(Owner.tryLock());
            QCOMPARE(hc::scratch::SweepAbandoned(Root), 0);
            // Folders without a lock are left alone for an hour: age them by sweeping with the clock moved on is
            // not possible here, so check the rule on a released lock instead.
            // An ended owner's lock file stays behind as QLockFile wrote it, with a PID that cannot exist on Windows.
            // Its host lines are kept as written: on Windows QLockFile compares %COMPUTERNAME%, not the DNS name.
            QFile Written(Held + "/owner.lock"); QVERIFY(Written.open(QIODevice::ReadOnly)); auto Lines = Written.readAll().split('\n'); Written.close();
            Owner.unlock(); Lines[0] = "999999"; QVERIFY(Put(Held + "/owner.lock", Lines.join('\n')));
            QCOMPARE(hc::scratch::SweepAbandoned(Root), 1);
            QVERIFY(!QFileInfo::exists(Held)); QVERIFY(QFileInfo::exists(LivePath)); QVERIFY(QFileInfo::exists(Foreign));
            QVERIFY(QFileInfo::exists(Fresh)); QVERIFY(QFileInfo::exists(Stale));
        }
        QVERIFY(!QFileInfo::exists(LivePath));   // a normal exit removes the session folder
    }
    void legacyTemporaryFolders()
    {
        QTemporaryDir Temp, Local, Shadow;
        const auto Dir = [&](const QString& Name) { QVERIFY(QDir().mkpath(Temp.filePath(Name))); };
        for (const auto& Name : {"HyperCommander-epub-preview-AbCdEf", "hc-index-AbCdEf", "hc-document-search-AbCdEf", "HyperEditor-AbCdEf",
                 "HyperViewer-AbCdEf", "HyperEditor-TooLong7", "mrst-virtual-0-AbCdEf", "keep-me", "HyperCommander/TransientIndexes/session-AbCdEf"}) Dir(Name);
        Dir(QStringLiteral("mrst-virtual-%1-AbCdEf").arg(QCoreApplication::applicationPid()));
        QVERIFY(Put(Temp.filePath("HyperEditor-AbCdEf/candidate.epub"), "copy")); QVERIFY(Put(Temp.filePath("hc-outcome-index-AbCdEf"), "x"));
        QVERIFY(Put(Shadow.filePath("preview-AbCdEf.shadow"), "unsaved")); QVERIFY(Put(Shadow.filePath("notes.txt"), "keep"));
        QVERIFY(QDir().mkpath(Local.filePath("HyperEpubWeb/cache/qtpipelinecache-x")));
        QVERIFY(Put(Local.filePath("hc-rst-preview/cache/data.bin"), "keep"));
        // Nothing younger than a day goes.
        QCOMPARE(hc::scratch::SweepLegacy(Temp.path(), Shadow.path(), Local.path(), QDateTime::currentDateTime()), 1);   // only the empty helper folder
        const auto Removed = hc::scratch::SweepLegacy(Temp.path(), Shadow.path(), Local.path(), QDateTime::currentDateTime().addDays(2));
        QCOMPARE(Removed, 9);
        for (const auto& Name : {"HyperCommander-epub-preview-AbCdEf", "hc-index-AbCdEf", "hc-document-search-AbCdEf", "HyperEditor-AbCdEf",
                 "HyperViewer-AbCdEf", "mrst-virtual-0-AbCdEf", "hc-outcome-index-AbCdEf", "HyperCommander/TransientIndexes"})
            QVERIFY2(!QFileInfo::exists(Temp.filePath(Name)), Name);
        for (const auto& Name : {"HyperEditor-TooLong7", "keep-me"}) QVERIFY2(QFileInfo::exists(Temp.filePath(Name)), Name);
        QVERIFY(QFileInfo::exists(Temp.filePath(QStringLiteral("mrst-virtual-%1-AbCdEf").arg(QCoreApplication::applicationPid()))));
        QVERIFY(!QFileInfo::exists(Shadow.filePath("preview-AbCdEf.shadow"))); QVERIFY(QFileInfo::exists(Shadow.filePath("notes.txt")));
        QVERIFY(!QFileInfo::exists(Local.filePath("HyperEpubWeb"))); QVERIFY(QFileInfo::exists(Local.filePath("hc-rst-preview/cache/data.bin")));
    }
    void previewBuildWaitsForOtherProcess()
    {
        QTemporaryDir Temp;
        QVERIFY(Put(Temp.filePath("conf.py"), "project = 't'\n")); QVERIFY(Put(Temp.filePath("index.rst"), "Title\n=====\n"));
        QVERIFY(Put(Temp.filePath("builder.py"), "# not run\n"));
        hc::rst::PreviewBuildRequest Request;
        Request.project.projectId = L"lock-test";
        Request.project.rootPath = Request.project.sourcePath = Temp.path().toStdWString();
        Request.project.confPath = Temp.filePath("conf.py").toStdWString();
        Request.project.buildPath = Temp.filePath("_build").toStdWString();
        // Any executable stands in for Python: the build fails, which is enough to see the lock taken and released.
        Request.pythonExe = QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/whoami.exe");
        Request.builderScript = Temp.filePath("builder.py"); Request.sourceFile = Temp.filePath("index.rst");
        const auto Lock = QDir(hc::rst::SphinxPreviewController::outputDirFor(Request.project)).filePath(".mrr-build.lock");
        QVERIFY(QDir().mkpath(QFileInfo(Lock).absolutePath()));
        QLockFile Other(Lock); QVERIFY(Other.tryLock());   // another window building the same project
        hc::rst::SphinxPreviewController Controller;
        QSignalSpy Started(&Controller, &hc::rst::SphinxPreviewController::buildStarted), Finished(&Controller, &hc::rst::SphinxPreviewController::buildFinished);
        QStringList Messages; connect(&Controller, &hc::rst::SphinxPreviewController::logMessage, this, [&](const QString& Text) { Messages.append(Text); });
        Controller.buildNow(Request);
        QTest::qWait(1200);
        QCOMPARE(Started.count(), 0); QVERIFY(Messages.join('\n').contains(QString::fromUtf8("다른 창에서")));
        Other.unlock();
        QTRY_COMPARE_WITH_TIMEOUT(Started.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(Finished.count() >= 1, 15000);
        QLockFile After(Lock); QVERIFY(After.tryLock());   // released when the build ended
    }
};

int main(int Argc, char** Argv)
{
    QApplication App(Argc, Argv); QTemporaryDir ViewConfig; App.setProperty("documentViewsConfigDir", ViewConfig.path());
    DocumentRecoveryTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstDocumentRecovery.moc"
