#include "UniqueLibs/document/documentRecoveryStore.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <limits>

using namespace hc::document;
namespace
{
    bool Write(const QString& Path, const QByteArray& Bytes)
    { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
    QByteArray Read(const QString& Path)
    { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
    QJsonObject Json(const QString& Path)
    { const auto Document = QJsonDocument::fromJson(Read(Path)); return Document.isObject() ? Document.object() : QJsonObject{}; }
    bool Put(const QString& Path, const QJsonObject& Object)
    { return Write(Path, QJsonDocument(Object).toJson(QJsonDocument::Compact)); }
    RecoverySnapshot External()
    {
        RecoverySnapshot Snapshot;
        Snapshot.document_.id_ = std::string(32, 'a');
        Snapshot.document_.contentMode_ = DocumentContentMode::ExternalModel;
        Snapshot.document_.generation_ = 7; Snapshot.document_.revision_ = 8;
        Snapshot.document_.savedRevision_ = 2; Snapshot.document_.dirty_ = true;
        Snapshot.document_.model_ = {9007199254740993ULL, 9, "edited"};
        Snapshot.document_.savedModel_ = {9007199254740993ULL, 2, "saved"};
        Snapshot.document_.source_.localPath_ = "D:/cache/materialized.bin";
        Snapshot.document_.source_.sourceToken_ = std::string(32, 'f');
        Snapshot.document_.source_.inputSha256_ = std::string(64, 'c');
        Snapshot.document_.source_.originalWritable_ = true;
        Snapshot.document_.source_.entry_.emplace();
        auto& Entry = *Snapshot.document_.source_.entry_;
        Entry.id_.providerId_ = {"archive"}; Entry.id_.key_ = "entry";
        Entry.id_.scope_ = hc::storage::IdentityScope::Persistent;
        Entry.id_.revision_ = hc::storage::ResourceRevision{"captured-revision"};
        Entry.locator_.kind_ = hc::storage::LocatorKind::Archive;
        Entry.locator_.root_ = "D:/origin/archive.zip";
        Entry.locator_.nesting_.push_back({"zip", "document.hwp"});
        Snapshot.viewId_ = "rhwp"; Snapshot.bundleSha256_ = std::string(64, 'b');
        Snapshot.format_ = "hwp"; Snapshot.engineSchema_ = RecoveryEngineSchema;
        Snapshot.bytes_ = std::string("snapshot\0bytes", 14);
        return Snapshot;
    }
}
class HwpRecoveryTests final : public QObject
{
    Q_OBJECT
private slots:
    void externalRoundTrip_data()
    {
        QTest::addColumn<QString>("Format"); QTest::addColumn<bool>("Dirty");
        QTest::newRow("hwp-dirty") << QString("hwp") << true;
        QTest::newRow("hwpx-dirty") << QString("hwpx") << true;
        QTest::newRow("undo-to-saved-baseline") << QString("hwp") << false;
    }
    void externalRoundTrip()
    {
        QFETCH(QString, Format); QFETCH(bool, Dirty);
        QTemporaryDir Root; DocumentRecoveryStore Store(Root.path());
        auto Snapshot = External(); Snapshot.format_ = Format.toStdString();
        if (!Dirty) { Snapshot.document_.dirty_ = false; Snapshot.document_.model_.savepoint_ = "saved"; }
        const auto Stored = Store.Store(Snapshot); QVERIFY(Stored.HasValue());
        const auto Path = QString::fromStdString(Stored.Value().snapshotPath_);
        const auto Record = Json(Path);
        QCOMPARE(Record["schemaVersion"], QJsonValue(3));
        QCOMPARE(Record["contentMode"], QJsonValue("externalModel"));
        QCOMPARE(Record["model"].toObject()["epoch"].toString(), QString("9007199254740993"));
        const auto Loaded = Store.Read(Path.toStdString()); QVERIFY(Loaded.HasValue());
        const auto& Recovered = Loaded.Value();
        QCOMPARE(Recovered.bytes_, Snapshot.bytes_); QCOMPARE(Recovered.format_, Snapshot.format_);
        QCOMPARE(Recovered.bundleSha256_, Snapshot.bundleSha256_);
        QCOMPARE(Recovered.engineSchema_, RecoveryEngineSchema);
        QVERIFY(Recovered.document_.contentMode_ == DocumentContentMode::ExternalModel);
        QVERIFY(Recovered.document_.model_ == Snapshot.document_.model_);
        QVERIFY(Recovered.document_.savedModel_ == Snapshot.document_.savedModel_);
        QCOMPARE(Recovered.document_.dirty_, Dirty);
        QCOMPARE(Recovered.document_.generation_, Snapshot.document_.generation_);
        QCOMPARE(Recovered.document_.revision_, Snapshot.document_.revision_);
        QCOMPARE(Recovered.document_.savedRevision_, Snapshot.document_.savedRevision_);
        QVERIFY(!Recovered.document_.source_.originalWritable_);
        QVERIFY(Recovered.document_.source_.sourceToken_.empty());
        QVERIFY(!Recovered.document_.source_.lease_);
        QCOMPARE(Recovered.document_.source_.inputSha256_, Snapshot.document_.source_.inputSha256_);
        QVERIFY(Recovered.document_.source_.entry_);
        QCOMPARE(Recovered.document_.source_.entry_->id_.key_, std::string("entry"));
        QCOMPARE(Recovered.document_.source_.entry_->locator_.nesting_.size(), std::size_t(1));
        const auto Listed = Store.List(); QVERIFY(Listed.HasValue()); QCOMPARE(Listed.Value().size(), std::size_t(1));
        QCOMPARE(Listed.Value().front().document_.dirty_, Dirty);
    }
    void missingExternalMetadataNeverBecomesClean()
    {
        QTemporaryDir Root; DocumentRecoveryStore Store(Root.path());
        RecoverySnapshot Bare; Bare.document_.id_ = std::string(32, 'b');
        Bare.document_.contentMode_ = DocumentContentMode::ExternalModel; Bare.bytes_ = "bytes";
        QVERIFY(!Store.Store(Bare).HasValue());
        QVERIFY(!QFile::exists(Root.filePath("index.json")));
        auto Snapshot = External(); Snapshot.document_.dirty_ = false;
        QVERIFY(!Store.Store(Snapshot).HasValue()); // A byte hash is not a saved model baseline.
        Snapshot = External(); Snapshot.document_.savedModel_ = {};
        QVERIFY(!Store.Store(Snapshot).HasValue());
        Snapshot = External(); Snapshot.document_.contentMode_ = static_cast<DocumentContentMode>(99);
        QVERIFY(!Store.Store(Snapshot).HasValue());
        Snapshot = External(); Snapshot.document_.source_.localPath_ = std::string(1, char(0xff));
        QVERIFY(!Store.Store(Snapshot).HasValue());
    }
    void malformedSchema3_data()
    {
        QTest::addColumn<QString>("Field");
        for (const auto* Field : {"bundle", "format", "engine", "mode", "dirty", "generation", "revision",
            "zeroRevision", "savedRevision", "model", "savedModel", "modelSequence", "savepoint", "utc", "sourceGrant", "sourceHash", "extra", "bytes"})
            QTest::newRow(Field) << QString::fromLatin1(Field);
    }
    void malformedSchema3()
    {
        QFETCH(QString, Field);
        QTemporaryDir Root; DocumentRecoveryStore Store(Root.path());
        const auto Stored = Store.Store(External()); QVERIFY(Stored.HasValue());
        const auto Path = QString::fromStdString(Stored.Value().snapshotPath_);
        auto Record = Json(Path);
        if (Field == "bundle") Record["bundleSha256"] = QString(64, 'z');
        if (Field == "format") Record["format"] = "pdf";
        if (Field == "engine") Record["engineSchema"] = 2;
        if (Field == "mode") Record["contentMode"] = "inlineBytes";
        if (Field == "dirty") Record["dirty"] = false;
        if (Field == "generation") Record["generation"] = "07";
        if (Field == "revision") Record["revision"] = 8;
        if (Field == "zeroRevision") Record["revision"] = "0";
        if (Field == "savedRevision") Record["savedRevision"] = "99";
        if (Field == "model") Record.remove("model");
        if (Field == "savedModel") Record["savedModel"] = QJsonObject{};
        if (Field == "modelSequence") { auto Model = Record["model"].toObject(); Model["changeSequence"] = "-1"; Record["model"] = Model; }
        if (Field == "savepoint") { auto Model = Record["model"].toObject(); Model["savepoint"] = "invalid whitespace"; Record["model"] = Model; }
        if (Field == "utc") Record["utcMs"] = "0";
        if (Field == "sourceGrant") { auto Source = Record["source"].toObject(); Source["originalWritable"] = true; Record["source"] = Source; }
        if (Field == "sourceHash") { auto Source = Record["source"].toObject(); Source["inputSha256"] = "wrong"; Record["source"] = Source; }
        if (Field == "extra") Record["edit"] = true;
        if (Field == "bytes") Record["bytes"] = "!!!!";
        QVERIFY(Put(Path, Record)); QVERIFY(!Store.Read(Path.toStdString()).HasValue());
    }
    void schemaDowngradeDoesNotRestoreByteOnlyExternalModel()
    {
        QTemporaryDir Root; DocumentRecoveryStore Store(Root.path());
        const auto Stored = Store.Store(External()); QVERIFY(Stored.HasValue());
        const auto Path = QString::fromStdString(Stored.Value().snapshotPath_);
        auto Record = Json(Path); Record["schemaVersion"] = 2;
        QVERIFY(Put(Path, Record)); QVERIFY(!Store.Read(Path.toStdString()).HasValue());
        for (const auto* Field : {"contentMode", "model", "savedModel", "bundleSha256", "format", "engineSchema", "dirty"}) Record.remove(Field);
        QVERIFY(Put(Path, Record)); QVERIFY(!Store.Read(Path.toStdString()).HasValue());
    }
    void legacyCompatibility_data()
    {
        QTest::addColumn<int>("Schema"); QTest::addColumn<QString>("View");
        for (const auto Schema : {1, 2}) for (const auto* View : {"text", "pdf", "image"}) {
            const auto Name = QByteArray::number(Schema) + "-" + View;
            QTest::newRow(Name.constData()) << Schema << QString::fromLatin1(View);
        }
    }
    void legacyCompatibility()
    {
        QFETCH(int, Schema); QFETCH(QString, View);
        QTemporaryDir Root; DocumentRecoveryStore Store(Root.path());
        RecoverySnapshot Snapshot; Snapshot.document_.id_ = std::string(32, 'c');
        Snapshot.document_.revision_ = 3; Snapshot.document_.savedRevision_ = 1;
        Snapshot.viewId_ = View.toStdString(); Snapshot.bytes_ = "legacy bytes";
        const auto Stored = Store.Store(Snapshot); QVERIFY(Stored.HasValue());
        const auto Path = QString::fromStdString(Stored.Value().snapshotPath_);
        auto Record = Json(Path); QCOMPARE(Record["schemaVersion"], QJsonValue(2));
        if (Schema == 1) { Record["schemaVersion"] = 1; Record.remove("generation"); Record.remove("savedRevision"); QVERIFY(Put(Path, Record)); }
        const auto Loaded = Store.Read(Path.toStdString()); QVERIFY(Loaded.HasValue());
        QCOMPARE(Loaded.Value().viewId_, Snapshot.viewId_); QCOMPARE(Loaded.Value().bytes_, Snapshot.bytes_);
        QVERIFY(Loaded.Value().document_.contentMode_ == DocumentContentMode::InlineBytes);
        QVERIFY(!Loaded.Value().document_.source_.originalWritable_);
    }
    void staleCheckpointCannotReplaceOrEvictLatest_data()
    {
        QTest::addColumn<QString>("Kind");
        for (const auto* Kind : {"old-revision", "equal-stamp", "old-generation"})
            QTest::newRow(Kind) << QString::fromLatin1(Kind);
    }
    void staleCheckpointCannotReplaceOrEvictLatest()
    {
        QFETCH(QString, Kind);
        QTemporaryDir Root; DocumentRecoveryStore Store(Root.path());
        auto Snapshot = External();
        for (std::uint64_t Revision = 6; Revision <= 8; ++Revision) {
            Snapshot.document_.revision_ = Revision; Snapshot.bytes_ = std::to_string(Revision);
            QVERIFY(Store.Store(Snapshot).HasValue());
        }
        const auto Before = Read(Root.filePath("index.json"));
        const auto Alias = Root.filePath(QString::fromStdString(Snapshot.document_.id_) + ".json");
        const auto AliasBefore = Read(Alias); const auto Files = QDir(Root.path()).entryList({"*.snapshot.json"});
        if (Kind == "old-revision") Snapshot.document_.revision_ = 5;
        if (Kind == "old-generation") { Snapshot.document_.generation_ = 6; Snapshot.document_.revision_ = 99; }
        Snapshot.bytes_ = "late completion";
        const auto Stale = Store.Store(Snapshot); QVERIFY(!Stale.HasValue());
        QCOMPARE(Stale.Failure().code_, std::string("document-recovery-stale"));
        QCOMPARE(Read(Root.filePath("index.json")), Before); QCOMPARE(Read(Alias), AliasBefore);
        QCOMPARE(QDir(Root.path()).entryList({"*.snapshot.json"}), Files);
        const auto Latest = Store.List(); QVERIFY(Latest.HasValue()); QCOMPARE(Latest.Value().size(), std::size_t(1));
        QCOMPARE(Latest.Value().front().bytes_, std::string("8"));
    }
    void legacyIndexUsesRecordGenerationToRejectStaleCompletion()
    {
        QTemporaryDir Root; DocumentRecoveryStore Store(Root.path()); auto Snapshot = External();
        QVERIFY(Store.Store(Snapshot).HasValue());
        // Remove the alias so only the legacy index record can supply generation.
        QVERIFY(QFile::remove(Root.filePath(QString::fromStdString(Snapshot.document_.id_) + ".json")));
        auto Index = Json(Root.filePath("index.json")); auto Entries = Index["entries"].toArray();
        auto Item = Entries[0].toObject(); Item.remove("generation"); Entries[0] = Item;
        Index["entries"] = Entries; QVERIFY(Put(Root.filePath("index.json"), Index));
        Snapshot.document_.generation_ = 6; Snapshot.document_.revision_ = 99;
        const auto Stale = Store.Store(Snapshot); QVERIFY(!Stale.HasValue());
        QCOMPARE(Stale.Failure().code_, std::string("document-recovery-stale"));
        QCOMPARE(Store.List().Value().front().document_.generation_, std::uint64_t(7));
    }
    void distinctEpochsArePreservedAsDirtyWithoutAuthority()
    {
        QTemporaryDir Root; DocumentRecoveryStore Store(Root.path()); auto Snapshot = External();
        --Snapshot.document_.savedModel_.epoch_;
        const auto Stored = Store.Store(Snapshot); QVERIFY(Stored.HasValue());
        const auto Loaded = Store.Read(Stored.Value().snapshotPath_); QVERIFY(Loaded.HasValue());
        QVERIFY(Loaded.Value().document_.dirty_);
        QVERIFY(Loaded.Value().document_.savedModel_ == Snapshot.document_.savedModel_);
        QVERIFY(Loaded.Value().document_.source_.sourceToken_.empty());
        QVERIFY(!Loaded.Value().document_.source_.originalWritable_);
    }
    void corruptLatestFallsBackWithoutLosingDirtyBaseline()
    {
        QTemporaryDir Root; DocumentRecoveryStore Store(Root.path()); auto Snapshot = External();
        const auto First = Store.Store(Snapshot); QVERIFY(First.HasValue());
        ++Snapshot.document_.revision_; Snapshot.bytes_ = "newer";
        const auto Second = Store.Store(Snapshot); QVERIFY(Second.HasValue());
        QVERIFY(Write(QString::fromStdString(Second.Value().snapshotPath_), "incomplete"));
        QVERIFY(Write(Root.filePath(QString::fromStdString(Snapshot.document_.id_) + ".json"), "incomplete"));
        const auto Listed = Store.List(); QVERIFY(Listed.HasValue()); QCOMPARE(Listed.Value().size(), std::size_t(1));
        QCOMPARE(Listed.Value().front().bytes_, First.Value().bytes_);
        QVERIFY(Listed.Value().front().document_.dirty_);
        QVERIFY(Listed.Value().front().document_.savedModel_ == First.Value().document_.savedModel_);
    }
    void indexOrderAndOldCleanupCannotHideLatest()
    {
        QTemporaryDir Root; DocumentRecoveryStore Store(Root.path()); auto Snapshot = External();
        QVERIFY(Store.Store(Snapshot).HasValue());
        ++Snapshot.document_.revision_; Snapshot.bytes_ = "latest";
        const auto Latest = Store.Store(Snapshot); QVERIFY(Latest.HasValue());
        auto Index = Json(Root.filePath("index.json")); const auto Entries = Index["entries"].toArray();
        QJsonArray Reversed; for (auto At = Entries.size(); At > 0; --At) Reversed.append(Entries[At - 1]);
        Index["entries"] = Reversed; QVERIFY(Put(Root.filePath("index.json"), Index));
        QCOMPARE(Store.List().Value().front().bytes_, std::string("latest"));
        QVERIFY(Store.Remove(Snapshot.document_.id_, 8).HasValue());
        QVERIFY(QFile::exists(QString::fromStdString(Latest.Value().snapshotPath_)));
        QCOMPARE(Store.List().Value().front().bytes_, std::string("latest"));
        QVERIFY(Store.Remove(Snapshot.document_.id_, (std::numeric_limits<std::uint64_t>::max)()).HasValue());
        QVERIFY(Store.List().Value().empty());
    }
};
QTEST_GUILESS_MAIN(HwpRecoveryTests)
#include "tstHwpRecovery.moc"
