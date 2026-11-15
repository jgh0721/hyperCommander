#include "stdafx.h"
#include "registryAutoruns.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "registryLinks.hpp"
#include "UniqueLibs/nativeStorageRevision.hpp"
/// 공통 라이브러리
#include "commonLibs/system/registryValue.hpp"
#include "commonLibs/system/imageInfo.hpp"
#include "commonLibs/system/registryWatch.hpp"
#include "commonLibs/system/processInfo.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFutureWatcher>
#include <QPointer>
#include <QTimer>
#include <QtConcurrent>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QtEndian>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryAutoruns, "hc.registry.autoruns")
namespace hc::registry {
namespace {
const QString RunPrefix = QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\");
const QString Blocked = RunPrefix + "Shell Extensions\\Blocked";
QString ValueText(const RegistryValue& Value) { const auto Text = RegistryValueCodec::DecodeText(Value); return Text.lossless ? Text.text : QString(); }
std::optional<RegistryValue> Optional(const RegistryResult<RegistryValue>& Value) { return Value ? std::optional<RegistryValue>(Value.value) : std::nullopt; }
void Inspect(RegistryAutorunEntry& Entry)
{
    if (Entry.image.isEmpty()) { Entry.warning += QObject::tr(" 실행 파일 경로를 해석할 수 없습니다."); Entry.risk = true; return; }
    HANDLE File = CreateFileW(reinterpret_cast<LPCWSTR>(Entry.image.utf16()), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (File == INVALID_HANDLE_VALUE) { Entry.error = GetLastError(); Entry.warning += QObject::tr(" 파일 확인 실패 (%1)").arg(Entry.error); Entry.risk = true; return; }
    BY_HANDLE_FILE_INFORMATION Info{}; const bool Got = GetFileInformationByHandle(File, &Info); CloseHandle(File);
    if (!Got) { Entry.warning = QObject::tr("파일 식별 정보를 읽을 수 없습니다."); Entry.risk = true; return; }
    const auto Revision = hc::storage::native::HandleRevision(Info);
    const auto Id = Entry.image.toCaseFolded() + '\n' + QString::fromStdString(Revision.token_);
    static QMutex Mutex; static QHash<QString, hc::system::SignatureInfo> Cache;
    hc::system::SignatureInfo Signature; bool Cached = false;
    { QMutexLocker Lock(&Mutex); const auto Found = Cache.constFind(Id); if (Found != Cache.cend()) { Signature = *Found; Cached = true; } }
    if (!Cached) Signature = hc::system::VerifySignature(Entry.image.toStdWString());
    // The signature API opens by path; reject replacement during verification.
    File = CreateFileW(reinterpret_cast<LPCWSTR>(Entry.image.utf16()), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    BY_HANDLE_FILE_INFORMATION After{};
    const bool Stable = File != INVALID_HANDLE_VALUE && GetFileInformationByHandle(File, &After)
        && hc::storage::native::HandleRevision(After).token_ == Revision.token_;
    if (File != INVALID_HANDLE_VALUE) CloseHandle(File);
    if (!Stable) { Entry.warning += QObject::tr(" 서명 확인 중 파일이 바뀌었습니다."); Entry.risk = true; return; }
    if (!Cached) { QMutexLocker Lock(&Mutex); if (Cache.size() >= 4096) Cache.clear(); Cache.insert(Id, Signature); }
    Entry.publisher = QString::fromStdWString(Signature.signer);
    Entry.microsoft = Signature.state == hc::system::SignatureState::Signed && (Entry.publisher.compare("Microsoft Windows", Qt::CaseInsensitive) == 0 || Entry.publisher.compare("Microsoft Corporation", Qt::CaseInsensitive) == 0 || Entry.publisher.compare("Microsoft Windows Publisher", Qt::CaseInsensitive) == 0);
    if (Signature.state == hc::system::SignatureState::Invalid) { Entry.warning += QObject::tr(" 서명이 유효하지 않습니다."); Entry.risk = true; }
    if (Entry.publisher.isEmpty()) Entry.publisher = Signature.state == hc::system::SignatureState::Unsigned ? QObject::tr("서명 없음") : QObject::tr("서명 미확인");
}
}
StartupApproval StartupApprovedCodec::Decode(const std::optional<RegistryValue>& Value)
{
    if (!Value) return {StartupApproval::State::Enabled, 0};
    if (Value->type != REG_BINARY || Value->bytes.size() != 12) return {};
    const auto State = qFromLittleEndian<quint32>(Value->bytes.constData());
    const auto Time = qFromLittleEndian<quint64>(Value->bytes.constData() + 4);
    if (State == 2 && Time == 0) return {StartupApproval::State::Enabled, 0};
    if (State == 3) return {StartupApproval::State::Disabled, Time};
    return {}; // Preserve unverified states instead of normalizing unknown bytes.
}
RegistryValue StartupApprovedCodec::Encode(const QString& Name, bool Enabled, quint64 Timestamp)
{
    RegistryValue Value{Name, REG_BINARY, QByteArray(12, char(0))}; qToLittleEndian<quint32>(Enabled ? 2 : 3, Value.bytes.data());
    qToLittleEndian<quint64>(Enabled ? 0 : Timestamp, Value.bytes.data() + 4); return Value;
}
std::optional<RegistryLocation> StartupApprovedCodec::LocationFor(const RegistryLocation& Run)
{
    if (Run.key.compare(RunPrefix + "Run", Qt::CaseInsensitive) != 0) return {};
    if (Run.hive != Hive::CurrentUser && Run.hive != Hive::LocalMachine) return {};
    // P0: actual Task Manager transitions verified HKCU64, HKLM64 and HKLM32.
    if (Run.hive == Hive::CurrentUser && Run.view == View::Wow32) return {};
    auto Result = Run; Result.view = View::Native64;
    Result.key = RunPrefix + "Explorer\\StartupApproved\\" + (Run.view == View::Wow32 ? "Run32" : "Run"); return Result;
}
RegistryAutorunScan RegistryAutoruns::Scan(IRegistryBackend& Backend, const QProcessEnvironment& Environment, const std::atomic_bool* Cancel, bool InspectFiles)
{
    RegistryAutorunScan Result; const auto Stopped = [&] { return Cancel && Cancel->load(); };
    const auto Add = [&](RegistryAutorunEntry Entry) {
        if (Stopped() || Result.entries.size() >= 50000) return;
        Entry.id = RegistryTreeModel::ValueId(Entry.location, Entry.value.name);
        if (Result.entries.contains(Entry.id)) return;
        if (Entry.name.isEmpty()) Entry.name = Entry.value.name.isEmpty() ? Entry.location.key.section('\\', -1) : Entry.value.name;
        if (InspectFiles) Inspect(Entry);
        if (Entry.status.isEmpty()) Entry.status = Entry.enabled ? QObject::tr("사용") : QObject::tr("사용 안 함");
        RegistryResultRow Row; Row.id = Entry.id; Row.groups = {Entry.category}; Row.location = Entry.location; Row.value = Entry.value;
        Row.cells = {Entry.name, Entry.status, Entry.publisher, Entry.image.isEmpty() ? ValueText(Entry.value) : Entry.image};
        Row.payload = {{"risk", Entry.risk}, {"microsoft", Entry.microsoft}, {"warning", Entry.warning}, {"view", int(Entry.location.view)}};
        Row.searchText = Entry.location.Path() + '\n' + Entry.warning; Result.rows.append(Row); Result.entries.insert(Entry.id, std::move(Entry));
    };
    const auto Values = [&](const RegistryLocation& Key) {
        const auto Snapshot = Backend.Enumerate(Key, Cancel);
        if (!Snapshot && Snapshot.error != ERROR_FILE_NOT_FOUND && Snapshot.error != ERROR_PATH_NOT_FOUND && Result.diagnostics.size() < 100) Result.diagnostics.append(Key.Path() + QString(" (%1)").arg(Snapshot.error));
        return Snapshot;
    };
    for (const auto HiveId : {Hive::CurrentUser, Hive::LocalMachine}) for (const auto Bits : {View::Native64, View::Wow32}) {
        for (const auto& Kind : {QString("Run"), QString("RunOnce")}) {
            if (Stopped()) return Result;
            const RegistryLocation Key{HiveId, RunPrefix + Kind, Bits}; const auto Snapshot = Values(Key); if (!Snapshot) continue;
            for (const auto& Value : Snapshot.value.values) {
                RegistryAutorunEntry Entry; Entry.location = Key; Entry.value = Value;
                Entry.category = QObject::tr("로그온 (Run)"); Entry.image = RegistryLinkResolver::FilePath(ValueText(Value), Environment);
                Entry.switchLocation = StartupApprovedCodec::LocationFor(Key);
                if (Entry.switchLocation) {
                    const auto Approval = Backend.Read(*Entry.switchLocation, Value.name);
                    if (!Approval && Approval.error != ERROR_FILE_NOT_FOUND && Approval.error != ERROR_PATH_NOT_FOUND) { Entry.status = QObject::tr("상태 읽기 실패"); Entry.error = Approval.error; }
                    else { Entry.switchValue = Optional(Approval); const auto State = StartupApprovedCodec::Decode(Entry.switchValue);
                        Entry.canToggle = State.state != StartupApproval::State::Unknown; Entry.enabled = State.state != StartupApproval::State::Disabled;
                        if (!Entry.canToggle) Entry.status = QObject::tr("알 수 없는 승인 형식"); }
                } else Entry.warning = QObject::tr("이 항목은 승인 값으로 끄지 않습니다. 원래 값 편집/삭제와 백업을 사용하세요.");
                Add(Entry);
            }
        }
    }
    for (const auto Bits : {View::Native64, View::Wow32}) {
        const RegistryLocation Key{Hive::LocalMachine, "Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", Bits};
        for (const auto& Name : {QString("Shell"), QString("Userinit")}) { const auto Value = Backend.Read(Key, Name); if (!Value) continue;
            RegistryAutorunEntry Entry; Entry.location = Key; Entry.value = Value.value; Entry.category = QObject::tr("Winlogon"); Entry.risk = true;
            const auto Data = ValueText(Value.value); Entry.image = RegistryLinkResolver::FilePath(Data, Environment);
            Entry.warning = QObject::tr("로그온 구성입니다. 값 편집/삭제 전 백업과 영향을 확인하세요."); Add(Entry); }
        const RegistryLocation Ifeo{Hive::LocalMachine, "Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options", Bits};
        const auto Children = Values(Ifeo); if (Children) for (const auto& Child : Children.value.keys) {
            if (Stopped()) return Result; const auto Key = Ifeo.Child(Child.name); const auto Value = Backend.Read(Key, "Debugger"); if (!Value) continue;
            RegistryAutorunEntry Entry; Entry.location = Key; Entry.value = Value.value; Entry.category = QObject::tr("실행 가로채기 (Image File Execution Options)");
            Entry.name = Child.name + " → Debugger"; Entry.risk = true; Entry.image = RegistryLinkResolver::FilePath(ValueText(Value.value), Environment);
            Entry.warning = QObject::tr("다른 프로그램 실행을 가로챕니다. Microsoft 서명도 숨기지 않습니다."); Add(Entry);
        }
    }
    const RegistryLocation Session{Hive::LocalMachine, "SYSTEM\\CurrentControlSet\\Control\\Session Manager"};
    const auto Boot = Backend.Read(Session, "BootExecute"); if (Boot) {
        RegistryAutorunEntry Entry; Entry.location = Session; Entry.value = Boot.value; Entry.category = QObject::tr("부팅 실행 (BootExecute)");
        Entry.name = "BootExecute"; Entry.warning = QObject::tr("부팅 구성입니다. Space로 끄지 않으며 원문과 백업을 확인합니다.");
        const auto Text = ValueText(Boot.value); Entry.risk = Text.trimmed().compare("autocheck autochk *", Qt::CaseInsensitive) != 0;
        Entry.image = RegistryLinkResolver::FilePath(Text.contains("autochk", Qt::CaseInsensitive) ? "autochk.exe" : Text, Environment); Add(Entry);
    }
    const QRegularExpression IdPattern("\\{[0-9a-fA-F]{8}-(?:[0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}\\}");
    for (const auto Bits : {View::Native64, View::Wow32}) {
        const RegistryLocation Root{Hive::ClassesRoot, {}, Bits}; const auto Types = Values(Root); if (!Types) continue;
        for (const auto& Type : Types.value.keys) {
            if (Stopped()) return Result;
            for (const auto& Tail : {QString("shellex\\ContextMenuHandlers"), QString("Background\\shellex\\ContextMenuHandlers")}) {
                const auto Parent = Root.Child(Type.name).Child(Tail); const auto Handlers = Backend.Enumerate(Parent, Cancel); if (!Handlers) continue;
                for (const auto& Handler : Handlers.value.keys) {
                    const auto Key = Parent.Child(Handler.name); const auto Value = Backend.Read(Key, {}); if (!Value) continue;
                    RegistryAutorunEntry Entry; Entry.location = Key; Entry.value = Value.value; Entry.name = Handler.name;
                    Entry.category = QObject::tr("셸 확장 · 바로 가기 메뉴"); Entry.clsid = IdPattern.match(ValueText(Value.value)).captured();
                    if (Entry.clsid.isEmpty()) Entry.clsid = IdPattern.match(Handler.name).captured();
                    if (Entry.clsid.isEmpty()) { Entry.risk = true; Entry.warning = QObject::tr("CLSID를 확인할 수 없습니다."); Add(Entry); continue; }
                    const auto Server = Backend.Read(Root.Child("CLSID\\" + Entry.clsid + "\\InprocServer32"), {});
                    if (Server) Entry.image = RegistryLinkResolver::FilePath(ValueText(Server.value), Environment);
                    else { Entry.risk = true; Entry.warning = QObject::tr("CLSID 서버 등록 없음"); }
                    Entry.switchLocation = RegistryLocation{Hive::LocalMachine, Blocked, Bits};
                    const auto Machine = Backend.Read(*Entry.switchLocation, Entry.clsid); const auto User = Backend.Read({Hive::CurrentUser, Blocked, Bits}, Entry.clsid);
                    const auto Readable = [](const auto& Value) { return Value || Value.error == ERROR_FILE_NOT_FOUND || Value.error == ERROR_PATH_NOT_FOUND; };
                    const auto UserRoot = RegistryLinkResolver::PhysicalClasses(Backend, Hive::CurrentUser, Bits);
                    const auto MachineRoot = RegistryLinkResolver::PhysicalClasses(Backend, Hive::LocalMachine, Bits);
                    const RegistryLocation UserSource = UserRoot ? UserRoot.value.Child(Key.key) : RegistryLocation{Hive::CurrentUser, "Software\\Classes\\" + Key.key, Bits};
                    const RegistryLocation MachineSource = MachineRoot ? MachineRoot.value.Child(Key.key) : RegistryLocation{Hive::LocalMachine, "Software\\Classes\\" + Key.key, Bits};
                    const auto UserValue = Backend.Read(UserSource, Value.value.name); const auto MachineValue = Backend.Read(MachineSource, Value.value.name);
                    if (UserValue && UserValue.value == Value.value) Entry.location = UserSource;
                    else if ((UserValue.error == ERROR_FILE_NOT_FOUND || UserValue.error == ERROR_PATH_NOT_FOUND) && MachineValue && MachineValue.value == Value.value) Entry.location = MachineSource;
                    Entry.switchValue = Optional(Machine); Entry.userBlocked = bool(User); Entry.enabled = !Machine && !User;
                    Entry.canToggle = Entry.location.hive != Hive::ClassesRoot && Readable(Machine) && Readable(User) && (!Entry.userBlocked || bool(Machine));
                    if (Entry.userBlocked) Entry.warning += QObject::tr(" 사용자 차단도 있습니다. 컴퓨터 차단을 해제해도 사용 상태가 되지 않습니다.");
                    if (!Readable(Machine) || !Readable(User)) { Entry.status = QObject::tr("차단 상태 읽기 실패"); Entry.risk = true; }
                    Add(Entry);
                }
            }
        }
    }
    if (Result.entries.size() >= 50000) Result.diagnostics.append(QObject::tr("항목 수 제한에 도달했습니다."));
    return Result;
}
RegistryMutationResult RegistryAutoruns::Toggle(RegistryMutation& Mutation, const RegistryAutorunEntry& Entry, quint64 Timestamp)
{
    if (!Entry.canToggle || !Entry.switchLocation) { RegistryMutationResult Result; Result.applied.error = ERROR_NOT_SUPPORTED; return Result; }
    // Keep the original autorun command byte-for-byte; only the approval/block is changed.
    RegistryStep Guard; Guard.kind = StepKind::ObserveValue; Guard.location = Entry.location; Guard.name = Entry.value.name; Guard.before = Guard.after = Entry.value;
    if (Entry.clsid.isEmpty()) return Mutation.SetCheckedValue(*Entry.switchLocation, Entry.value.name, Entry.switchValue,
        StartupApprovedCodec::Encode(Entry.value.name, !Entry.enabled, Timestamp), {Guard});
    const auto After = Entry.switchValue ? std::optional<RegistryValue>() : std::optional<RegistryValue>(RegistryValue{Entry.clsid, REG_SZ, QByteArray(2, char(0))});
    return Mutation.SetCheckedValue(*Entry.switchLocation, Entry.clsid, Entry.switchValue, After, {Guard});
}
struct RegistryAutoruns::Watches { std::vector<std::unique_ptr<RegistryWatcher>> items; QTimer* debounce = nullptr; };
RegistryAutoruns::RegistryAutoruns(QObject* Parent) : QObject(Parent), watches_(std::make_unique<Watches>())
{
    model_ = new RegistryResultModel({tr("항목"), tr("상태"), tr("게시자"), tr("실행 파일 / 데이터")}, this);
    watches_->debounce = new QTimer(this); watches_->debounce->setSingleShot(true); watches_->debounce->setInterval(750);
    connect(watches_->debounce, &QTimer::timeout, this, [this] { if (active_) Reload(); });
}
RegistryAutoruns::~RegistryAutoruns() { SetActive(false); }
void RegistryAutoruns::SetActive(bool Active)
{
    if (active_ == Active) return;
    active_ = Active;
    if (Active) { startWatches(); Reload(); }
    else { watches_->debounce->stop(); watches_->items.clear(); if (cancel_) cancel_->store(true); ++generation_; loading_ = false; }
}
void RegistryAutoruns::startWatches()
{
    watches_->items.clear();
    // Bounded partition watches cover missing leaf keys as well as existing ones.
    QList<RegistryLocation> Roots;
    for (const auto Bits : {View::Native64, View::Wow32}) {
        for (const auto HiveId : {Hive::CurrentUser, Hive::LocalMachine}) {
            Roots.append({HiveId, "Software\\Microsoft\\Windows\\CurrentVersion", Bits});
            Roots.append({HiveId, "Software\\Classes", Bits});
        }
        Roots.append({Hive::Users, QString::fromStdWString(hc::system::CurrentUserSid()) + "_Classes", Bits});
        Roots.append({Hive::LocalMachine, "Software\\Microsoft\\Windows NT\\CurrentVersion", Bits});
    }
    Roots.append({Hive::LocalMachine, "SYSTEM\\CurrentControlSet\\Control\\Session Manager"});
    for (const auto& Root : Roots) {
        auto Watch = std::make_unique<RegistryWatcher>(); const QPointer<RegistryAutoruns> Guard(this);
        if (Watch->Start(Root, true, [Guard](quint32) {
            if (Guard) QMetaObject::invokeMethod(Guard, [Guard] { if (Guard && Guard->active_) Guard->watches_->debounce->start(); }, Qt::QueuedConnection);
        })) watches_->items.push_back(std::move(Watch));
    }
}
std::optional<RegistryAutorunEntry> RegistryAutoruns::Entry(const QString& Id) const
{ const auto Found = data_.entries.constFind(Id); return Found == data_.entries.cend() ? std::nullopt : std::optional<RegistryAutorunEntry>(*Found); }
void RegistryAutoruns::Reload()
{
    if (cancel_) cancel_->store(true); cancel_ = std::make_shared<std::atomic_bool>(false); const auto Cancel = cancel_;
    const auto Generation = ++generation_; loading_ = true; const auto Environment = QProcessEnvironment::systemEnvironment();
    auto* Watcher = new QFutureWatcher<RegistryAutorunScan>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Generation] {
        auto Result = Watcher->result(); Watcher->deleteLater(); if (Generation != generation_) return;
        emit Changing(); data_ = std::move(Result); loading_ = false; model_->SetRows(data_.rows); emit Changed();
    });
    Watcher->setFuture(QtConcurrent::run([Cancel, Environment] { WinRegistryBackend Backend; return Scan(Backend, Environment, Cancel.get()); }));
}
} // namespace hc::registry
