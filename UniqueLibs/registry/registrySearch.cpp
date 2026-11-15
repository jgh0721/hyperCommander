#include "stdafx.h"
#include "registrySearch.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "registryLinks.hpp"
/// 공통 라이브러리
#include "commonLibs/system/registryValue.hpp"
#include "commonLibs/columns/fieldCondition.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QTimer>
#include <QScopeGuard>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistrySearch, "hc.registry.search")
namespace hc::registry {
RegistrySearchCursor::RegistrySearchCursor(RegistrySearchOptions Options) : options_(std::move(Options)), pending_(options_.roots)
{
    if (options_.text.isEmpty() && !options_.since) error_ = QObject::tr("찾을 내용을 입력하거나 변경 시각을 고르세요.");
    if (options_.text.size() > 4096) error_ = QObject::tr("찾을 내용은 4096자 이내로 입력하세요.");
    if (!options_.keys && !options_.names && !options_.data) error_ = QObject::tr("키·값 이름·데이터 중 찾을 대상을 고르세요.");
    if (options_.roots.isEmpty()) error_ = QObject::tr("찾을 범위가 없습니다.");
    if (options_.regex) {
        pattern_ = hc::columns::FieldRegex(options_.whole ? "\\A(?:" + options_.text + ")\\z" : options_.text);
        if (!pattern_.isValid()) error_ = QObject::tr("정규식 오류: %1").arg(pattern_.errorString());
    }
}
bool RegistrySearchCursor::hit(const QString& Text)
{
    if (options_.text.isEmpty()) return true;
    if (!options_.regex) return options_.whole ? Text.compare(options_.text, Qt::CaseInsensitive) == 0 : Text.contains(options_.text, Qt::CaseInsensitive);
    const auto Match = pattern_.match(Text);
    if (!Match.isValid()) { error_ = QObject::tr("정규식 실행 한도를 넘었습니다. 식을 단순하게 바꿔 다시 찾으세요."); return false; }
    return Match.hasMatch();
}
bool RegistrySearchCursor::typeAllowed(quint32 Type) const
{
    const bool Text = Type == REG_SZ || Type == REG_EXPAND_SZ || Type == REG_MULTI_SZ;
    const bool Number = Type == REG_DWORD || Type == REG_DWORD_BIG_ENDIAN || Type == REG_QWORD;
    return options_.type == RegistrySearchOptions::Type::All
        || (options_.type == RegistrySearchOptions::Type::Text && Text)
        || (options_.type == RegistrySearchOptions::Type::Number && Number)
        || (options_.type == RegistrySearchOptions::Type::Binary && !Text && !Number);
}
RegistryLocation RegistrySearchCursor::sourceLocation(IRegistryBackend& Backend, const RegistryLocation& Location, const std::optional<RegistryValue>& Value)
{
    auto Physical = Location;
    if (Location.hive == Hive::ClassesRoot) {
        const int Bits = int(Location.view);
        if (!userClasses_.contains(Bits)) {
            const auto User = RegistryLinkResolver::PhysicalClasses(Backend, Hive::CurrentUser, Location.view);
            const auto Machine = RegistryLinkResolver::PhysicalClasses(Backend, Hive::LocalMachine, Location.view);
            if (User) userClasses_[Bits] = User.value;
            if (Machine) machineClasses_[Bits] = Machine.value;
        }
        for (const auto* Roots : {&userClasses_, &machineClasses_}) {
            if (!Roots->contains(Bits)) continue;
            const auto Candidate = Location.key.isEmpty() ? Roots->value(Bits) : Roots->value(Bits).Child(Location.key);
            if (Value) {
                const auto Actual = Backend.Read(Candidate, Value->name);
                if (Actual && Actual.value.type == Value->type && Actual.value.bytes == Value->bytes) { Physical = Candidate; break; }
                if (!Actual && Actual.error != ERROR_FILE_NOT_FOUND && Actual.error != ERROR_PATH_NOT_FOUND) break;
            } else if (Backend.QueryInfo(Candidate)) { Physical = Candidate; break; }
        }
    }
    if (Physical.hive == Hive::CurrentUser && !options_.callerSid.isEmpty()) Physical.sid = options_.callerSid;
    return Physical;
}
RegistrySearchBatch RegistrySearchCursor::Advance(IRegistryBackend& Backend, const std::atomic_bool* Cancel, qsizetype MaxRows, int Milliseconds)
{
    RegistrySearchBatch Batch; QElapsedTimer Timer; Timer.start();
    const auto Add = [&](const std::optional<RegistryValue>& Value, const QString& Where) {
        const auto Physical = sourceLocation(Backend, currentLocation_, Value);
        const auto Id = Value ? "value:" + RegistryTreeModel::ValueId(Physical, Value->name) : "key:" + Physical.Id(); if (emitted_.contains(Id)) return;
        const qsizetype Bytes = (Value ? Value->bytes.size() : 0) + currentLocation_.key.size() * 2 + 256;
        if (count_ >= 100000 || retained_ + Bytes > 256 * 1024 * 1024) { limited_ = true; return; }
        RegistryResultRow Row; Row.id = Id; Row.location = Physical; Row.value = Value; Row.groups = {Physical.PhysicalPath()};
        Row.cells = {Value ? (Value->name.isEmpty() ? QObject::tr("(기본값)") : Value->name) : currentLocation_.key.section('\\', -1),
            Value ? RegistryTreeModel::ValueTypeLabel(Value->type) : QObject::tr("키"),
            Value ? RegistryTreeModel::ValuePreview(*Value) : QString(), Where};
        Row.payload = {{"match", Where}, {"view", int(currentLocation_.view)}, {"written", QString::number(current_->info.lastWrite)}};
        Row.searchText = currentLocation_.Path();
        Batch.rows.append(std::move(Row)); emitted_.insert(Id); retained_ += Bytes; ++count_;
    };
    while (error_.isEmpty() && !limited_ && Batch.rows.size() < MaxRows && Timer.elapsed() < Milliseconds && !(Cancel && Cancel->load())) {
        if (!current_) {
            if (pending_.isEmpty()) break;
            currentLocation_ = pending_.takeLast(); if (visited_.contains(currentLocation_.Id())) continue;
            visited_.insert(currentLocation_.Id()); ++scanned_;
            const auto Snapshot = Backend.Enumerate(currentLocation_, Cancel);
            if (!Snapshot) {
                if (Snapshot.error == ERROR_CANCELLED) { visited_.remove(currentLocation_.Id()); pending_.append(currentLocation_); break; }
                ++denied_; if (Batch.diagnostics.size() < 100) Batch.diagnostics.append(currentLocation_.Path() + QString(" (%1)").arg(Snapshot.error)); continue;
            }
            current_ = Snapshot.value; value_ = 0; keyChecked_ = false;
            if (!current_->info.link) for (const auto& Child : current_->keys) pending_.append(currentLocation_.Child(Child.name));
            if (options_.since && current_->info.lastWrite < options_.since) { current_.reset(); continue; }
        }
        if (!keyChecked_) {
            keyChecked_ = true;
            if ((options_.text.isEmpty() || (options_.keys && options_.type == RegistrySearchOptions::Type::All && hit(currentLocation_.key.section('\\', -1)))))
                Add({}, QObject::tr("키 이름"));
            if (Batch.rows.size() >= MaxRows || limited_) break;
        }
        if (options_.text.isEmpty()) { current_.reset(); continue; }
        while (value_ < current_->values.size() && Batch.rows.size() < MaxRows && Timer.elapsed() < Milliseconds && !(Cancel && Cancel->load()) && !limited_ && error_.isEmpty()) {
            const auto& Value = current_->values[value_++]; if (!typeAllowed(Value.type)) continue;
            if (options_.names && hit(Value.name.isEmpty() ? QObject::tr("(기본값)") : Value.name)) { Add(Value, QObject::tr("값 이름")); continue; }
            if (options_.data) {
                if (options_.regex && Value.bytes.size() > 2 * 1024 * 1024) { error_ = QObject::tr("2MB보다 큰 값의 정규식 검색은 제한됩니다. 일반 문자열 검색을 사용하거나 범위를 좁히세요."); break; }
                const auto Text = RegistryValueCodec::DecodeText(Value, false); const auto Number = RegistryValueCodec::DecodeNumber(Value);
                const auto Data = Text.lossless ? Text.text : Number ? QString::number(Number.value) : QString::fromLatin1(Value.bytes.toHex(' '));
                if (hit(Data) || (Number && !options_.regex && hit("0x" + QString::number(Number.value, 16)))) Add(Value, QObject::tr("데이터"));
            }
        }
        if (value_ >= current_->values.size()) current_.reset();
    }
    Batch.scanned = scanned_; Batch.denied = denied_; Batch.error = error_; Batch.limited = limited_;
    Batch.done = !error_.isEmpty() || limited_ || (!current_ && pending_.isEmpty());
    return Batch;
}
RegistrySearch::RegistrySearch(QObject* Parent) : QObject(Parent)
{ model_ = new RegistryResultModel({tr("이름"), tr("형식"), tr("데이터"), tr("일치 위치")}, this); }
RegistrySearch::~RegistrySearch() { if (cancel_) cancel_->store(true); }
bool RegistrySearch::Start(RegistrySearchOptions Options)
{
    Stop(); ++generation_; inFlight_ = false; options_ = std::move(Options);
    cursor_ = std::make_shared<RegistrySearchCursor>(options_); status_ = {}; status_.error = cursor_->Error();
    rows_.clear(); model_->SetRows({}); cancel_ = std::make_shared<std::atomic_bool>(false);
    if (!status_.error.isEmpty()) { status_.done = true; emit Changed(); return false; }
    running_ = true; next(); emit Changed(); return true;
}
void RegistrySearch::Stop() { running_ = false; if (cancel_) cancel_->store(true); emit Changed(); }
void RegistrySearch::Continue()
{
    if (!cursor_ || status_.done || inFlight_) return;
    cancel_->store(false); running_ = true; next(); emit Changed();
}
void RegistrySearch::next()
{
    if (!running_ || inFlight_ || !cursor_) return; inFlight_ = true;
    const auto Cursor = cursor_; const auto Cancel = cancel_; const auto Generation = generation_;
    auto* Watcher = new QFutureWatcher<RegistrySearchBatch>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Generation] {
        auto Batch = Watcher->result(); Watcher->deleteLater(); if (Generation != generation_) return; inFlight_ = false;
        const auto Errors = status_.diagnostics; status_ = Batch; status_.diagnostics = (Errors + Batch.diagnostics).mid(0, 100);
        if (!Batch.rows.isEmpty()) { emit BatchStarting(); rows_.append(Batch.rows); model_->AppendRows(std::move(Batch.rows)); emit BatchFinished(); }
        if (Batch.done) running_ = false; emit Changed();
        if (running_) QTimer::singleShot(0, this, &RegistrySearch::next);
    });
    Watcher->setFuture(QtConcurrent::run([Cursor, Cancel] {
        const bool Background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        const auto Priority = qScopeGuard([&] { if (Background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END); });
        WinRegistryBackend Backend; return Cursor->Advance(Backend, Cancel.get());
    }));
}
} // namespace hc::registry