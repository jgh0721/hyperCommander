#include "UniqueLibs/fileAttributes.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QCoreApplication>
#include <QScopeGuard>
#include <Aclapi.h>

using namespace hc::storage;
using namespace hc::attributes;
using namespace hc::operations;
static auto Skip = [](const EntrySnapshot&, bool) { return Decision::Skip; };
class DeniedWriter final : public IWritableStorage
{
public:
    ProviderId Id() const override { return {"local"}; }
    Result<SupportDecision> Supports(Mutation, Locator, OperationContext) override { return SupportDecision{}; }
    Result<std::optional<EntrySnapshot>> Inspect(Locator, OperationContext) override { return Error{ErrorCategory::Unsupported, "unused"}; }
    Result<std::unique_ptr<IWriteSession>> BeginWrite(Locator, DestinationExpectation, OperationContext) override { return Error{ErrorCategory::Unsupported, "unused"}; }
    Result<Unit> MakeDirectory(Locator, OperationContext) override { return Error{ErrorCategory::Unsupported, "unused"}; }
    Result<Unit> Remove(EntrySnapshot, OperationContext) override { return Error{ErrorCategory::Unsupported, "unused"}; }
    Result<Unit> Rename(EntrySnapshot, Locator, OperationContext) override { return Error{ErrorCategory::Unsupported, "unused"}; }
    Result<Unit> PatchMetadata(EntrySnapshot, MetadataPatch, OperationContext) override { return Error{ErrorCategory::AccessDenied, "fixture-access-denied"}; }
};
class AttributeTests : public QObject
{
    Q_OBJECT
    QTemporaryDir root_;
    std::shared_ptr<IStorageProvider> reader_ = MakeNativeReadProvider(LocatorKind::Local);
    std::shared_ptr<IWritableStorage> writer_ = MakeNativeWritableStorage(LocatorKind::Local, reader_);
    Locator at(const QString& Name) { return {LocatorKind::Local, QDir::toNativeSeparators(root_.filePath(Name)).toStdString()}; }
    EntrySnapshot create(const QString& Name) {
        QFile File(root_.filePath(Name)); if (!File.open(QIODevice::WriteOnly)) qFatal("fixture open failed"); File.write("preserved-content"); File.close();
        return reader_->Resolve(at(Name), {}).get().Value();
    }
    hc::permissions::FileElevationLauncher launcher(bool Cancel = false, std::function<void()> Before = {}) {
        return [Cancel, Before](const QString&, const QString& Arguments) -> Result<HANDLE> {
            if (Cancel) return Error{ErrorCategory::Cancelled, "uac-cancelled"};
            if (Before) Before();
            const auto Exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
            auto Command = ('"' + Exe + (Arguments.startsWith("--elevated-list") ? "\" --directory-fixture " : "\" --metadata-fixture ") + Arguments.section(' ', 1)).toStdWString();
            STARTUPINFOW Startup{}; Startup.cb = sizeof(Startup); PROCESS_INFORMATION Process{};
            if (!CreateProcessW(reinterpret_cast<LPCWSTR>(Exe.utf16()), Command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Startup, &Process))
                return Error{ErrorCategory::Unavailable, "fixture-start"};
            CloseHandle(Process.hThread); return Process.hProcess;
        };
    }
private slots:
    void patchPreservesContentAndOtherTimes() {
        auto Entry = create("preserve.txt"); const auto Name = root_.filePath("preserve.txt").toStdWString();
        auto Cleanup = qScopeGuard([&] { SetFileAttributesW(Name.c_str(), FILE_ATTRIBUTE_NORMAL); });
        MetadataPatch Patch{FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY, FILE_ATTRIBUTE_ARCHIVE, Entry.modifiedUtcMs_ - 60000};
        auto Result = writer_->PatchMetadata(Entry, Patch, {}); QVERIFY2(Result.HasValue(), Result.HasValue() ? "" : Result.Failure().code_.c_str());
        auto Fresh = reader_->Resolve(Entry.locator_, {}).get(); QVERIFY(Fresh.HasValue());
        QCOMPARE(Fresh.Value().attributes_ & 0x27u, 3u); QCOMPARE(Fresh.Value().createdUtcMs_, Entry.createdUtcMs_);
        QCOMPARE(Fresh.Value().modifiedUtcMs_, *Patch.modifiedUtcMs_);
        QFile File(root_.filePath("preserve.txt")); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), QByteArray("preserved-content"));
    }
    void rejectsInvalidAndStale() {
        auto Entry = create("stale.txt");
        QVERIFY(!writer_->PatchMetadata(Entry, {1, 1, {}}, {}).HasValue());
        QVERIFY(!writer_->PatchMetadata(Entry, {0x400, 0, {}}, {}).HasValue());
        QVERIFY(writer_->PatchMetadata(Entry, {2, 0, {}}, {}).HasValue());
        const auto Changed = writer_->PatchMetadata(Entry, {1, 0, {}}, {});
        QVERIFY(!Changed.HasValue()); QCOMPARE(Changed.Failure().category_, ErrorCategory::StaleRevision);
        SetFileAttributesW(root_.filePath("stale.txt").toStdWString().c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    void replacementIsNotMutated() {
        auto Entry = create("replace.txt"); QVERIFY(QFile::rename(root_.filePath("replace.txt"), root_.filePath("original.txt")));
        create("replace.txt"); auto Result = writer_->PatchMetadata(Entry, {2, 0, {}}, {});
        QVERIFY(!Result.HasValue()); QCOMPARE(Result.Failure().category_, ErrorCategory::StaleRevision);
    }
    void recursiveSelectionAndDeduplication() {
        QVERIFY(QDir(root_.path()).mkdir("tree")); auto Child = create("tree/child.txt");
        auto Folder = reader_->Resolve(at("tree"), {}).get().Value();
        const auto Shallow = Collect({Folder}, reader_, false, {}, Skip); QCOMPARE(Shallow.entries_.size(), std::size_t(1));
        const auto Deep = Collect({Child, Folder}, reader_, true, {}, Skip); QVERIFY(!Deep.blocked_); QCOMPARE(Deep.entries_.size(), std::size_t(2));
        QCOMPARE(Deep.entries_.front().name_, std::string("child.txt"));
        const auto ReverseSelection = Collect({Folder, Child}, reader_, true, {}, Skip);
        QCOMPARE(ReverseSelection.entries_.size(), std::size_t(2)); QCOMPARE(ReverseSelection.entries_.front().name_, std::string("child.txt"));
        auto Changed = Apply(Deep, {2, 0, {}}, writer_, {}, {}, Skip); QCOMPARE(Changed.changed_, std::size_t(2));
        SetFileAttributesW(root_.filePath("tree/child.txt").toStdWString().c_str(), FILE_ATTRIBUTE_NORMAL);
        SetFileAttributesW(root_.filePath("tree").toStdWString().c_str(), FILE_ATTRIBUTE_DIRECTORY);
    }
    void noOpAndCancellation() {
        Plan P; P.entries_ = {create("noop.txt"), create("cancel.txt")};
        auto Result = Apply(P, {}, writer_, {}, {}, Skip); QCOMPARE(Result.unchanged_, std::size_t(2));
        std::stop_source Stop; Stop.request_stop(); OperationContext Context; Context.stopToken_ = Stop.get_token();
        Result = Apply(P, {2, 0, {}}, writer_, {}, Context, Skip); QCOMPARE(Result.pending_, std::size_t(2)); QCOMPARE(Result.changed_, std::size_t(0));
    }
    void helperProtocolChangesMetadata() {
        auto Entry = create("helper.txt");
        auto Result = ElevatedPatch(Entry, {2, 0, {}}, hc::permissions::MakeFileElevationService(launcher()), {});
        QVERIFY2(Result.HasValue(), Result.HasValue() ? "" : Result.Failure().code_.c_str());
        QVERIFY(reader_->Resolve(Entry.locator_, {}).get().Value().attributes_ & 2);
        SetFileAttributesW(root_.filePath("helper.txt").toStdWString().c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    void helperUacCancellation() {
        auto Entry = create("uac.txt");
        auto Result = ElevatedPatch(Entry, {2, 0, {}}, hc::permissions::MakeFileElevationService(launcher(true)), {});
        QVERIFY(!Result.HasValue()); QCOMPARE(Result.Failure().category_, ErrorCategory::Cancelled);
        QVERIFY(!(reader_->Resolve(Entry.locator_, {}).get().Value().attributes_ & 2));
    }
    void helperRejectsReplacement() {
        auto Entry = create("helper-replace.txt");
        auto Result = ElevatedPatch(Entry, {2, 0, {}}, hc::permissions::MakeFileElevationService(launcher(false, [&] {
            QFile::rename(root_.filePath("helper-replace.txt"), root_.filePath("helper-old.txt")); create("helper-replace.txt");
        })), {});
        QVERIFY(!Result.HasValue()); QVERIFY(!(reader_->Resolve(Entry.locator_, {}).get().Value().attributes_ & 2));
    }
    void deniedItemsElevateSkipAndStopIndependently() {
        Plan P; P.entries_ = {create("denied-one.txt"), create("denied-two.txt"), create("denied-three.txt")};
        int Questions = 0;
        auto Result = Apply(P, {2, 0, {}}, std::make_shared<DeniedWriter>(), hc::permissions::MakeFileElevationService(launcher()), {},
            [&](const EntrySnapshot&, bool Enumerating) { if (Enumerating) qFatal("unexpected enumerate prompt"); return ++Questions == 1 ? Decision::Elevate : Questions == 2 ? Decision::Skip : Decision::Stop; });
        QCOMPARE(Result.changed_, std::size_t(1)); QCOMPARE(Result.skipped_, std::size_t(1)); QCOMPARE(Result.pending_, std::size_t(1));
        QVERIFY(!(reader_->Resolve(P.entries_[1].locator_, {}).get().Value().attributes_ & 2));
        SetFileAttributesW(root_.filePath("denied-one.txt").toStdWString().c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    void deniedUacStopsBatchWithoutReplayingSuccess() {
        Plan P; P.entries_ = {create("denied-cancel.txt"), create("denied-pending.txt")};
        auto Result = Apply(P, {2, 0, {}}, std::make_shared<DeniedWriter>(), hc::permissions::MakeFileElevationService(launcher(true)), {},
            [](const auto&, bool) { return Decision::Elevate; });
        QCOMPARE(Result.failed_, std::size_t(1)); QCOMPARE(Result.pending_, std::size_t(1)); QCOMPARE(Result.changed_, std::size_t(0));
    }
    void elevatedDirectorySnapshotCanBePatched() {
        QVERIFY(QDir(root_.path()).mkdir("elevated-tree")); create("elevated-tree/child.txt");
        auto Opened = hc::permissions::OpenElevatedDirectorySession(root_.filePath("elevated-tree"), {}, launcher());
        QVERIFY2(Opened.HasValue(), Opened.HasValue() ? "" : Opened.Failure().code_.c_str());
        auto Cursor = Opened.Value()->Enumerate(root_.filePath("elevated-tree"), {}, reader_); QVERIFY(Cursor.HasValue());
        auto Page = Cursor.Value()->Next(256, {}).get(); QVERIFY(Page.HasValue()); QCOMPARE(Page.Value().entries_.size(), std::size_t(1));
        QVERIFY(Strong(Page.Value().entries_[0]));
        auto Result = ElevatedPatch(Page.Value().entries_[0], {2, 0, {}}, hc::permissions::MakeFileElevationService(launcher()), {});
        QVERIFY2(Result.HasValue(), Result.HasValue() ? "" : Result.Failure().code_.c_str());
        SetFileAttributesW(root_.filePath("elevated-tree/child.txt").toStdWString().c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    void nativeAccessDeniedIsOfferedForElevation()
    {
        auto Entry = create("acl-denied.txt"); const auto Name = root_.filePath("acl-denied.txt").toStdWString();
        HANDLE Handle = CreateFileW(Name.c_str(), READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(Handle != INVALID_HANDLE_VALUE);
        PACL Original = nullptr; PSECURITY_DESCRIPTOR Descriptor = nullptr;
        QCOMPARE(GetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &Original, nullptr, &Descriptor), DWORD(ERROR_SUCCESS));
        auto Restore = qScopeGuard([&] { SetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, Original, nullptr); LocalFree(Descriptor); CloseHandle(Handle); });
        BYTE Sid[SECURITY_MAX_SID_SIZE]; DWORD SidSize = sizeof(Sid); QVERIFY(CreateWellKnownSid(WinWorldSid, nullptr, Sid, &SidSize));
        EXPLICIT_ACCESSW Deny{}; Deny.grfAccessPermissions = FILE_WRITE_ATTRIBUTES; Deny.grfAccessMode = DENY_ACCESS;
        Deny.Trustee.TrusteeForm = TRUSTEE_IS_SID; Deny.Trustee.ptstrName = reinterpret_cast<LPWSTR>(Sid);
        PACL Blocked = nullptr; QCOMPARE(SetEntriesInAclW(1, &Deny, Original, &Blocked), DWORD(ERROR_SUCCESS));
        auto FreeAcl = qScopeGuard([&] { LocalFree(Blocked); });
        QCOMPARE(SetSecurityInfo(Handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, Blocked, nullptr), DWORD(ERROR_SUCCESS));
        Plan P; P.entries_ = {Entry}; int Questions = 0;
        auto Result = Apply(P, {2, 0, {}}, writer_, hc::permissions::MakeFileElevationService(launcher()), {},
            [&](const auto&, bool Enumerating) { ++Questions; if (Enumerating) qFatal("wrong approval type"); return Decision::Skip; });
        QCOMPARE(Questions, 1); QCOMPARE(Result.skipped_, std::size_t(1)); QCOMPARE(Result.changed_, std::size_t(0));
        QVERIFY(!(GetFileAttributesW(Name.c_str()) & FILE_ATTRIBUTE_HIDDEN));
    }
};
int main(int Argc, char** Argv)
{
    QCoreApplication App(Argc, Argv);
    if (Argc == 3 && QString::fromLocal8Bit(Argv[1]) == "--directory-fixture")
        return int(hc::permissions::ExecuteDirectoryElevation(QJsonDocument::fromJson(QByteArray::fromBase64(Argv[2], QByteArray::Base64UrlEncoding)).object()));
    if (Argc == 3 && QString::fromLocal8Bit(Argv[1]) == "--metadata-fixture") {
        // Test-owned process only. Production HyperCommanderAdmin requires an
        // elevated token before entering this same authenticated protocol.
        return int(hc::permissions::ExecuteFileElevation(QJsonDocument::fromJson(QByteArray::fromBase64(Argv[2], QByteArray::Base64UrlEncoding)).object()));
    }
    AttributeTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstFileAttributes.moc"
