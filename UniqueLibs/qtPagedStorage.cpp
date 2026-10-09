#include "qtPagedStorage.hpp"
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <queue>
#include <array>

namespace hc::storage
{
namespace
{
    Error DiskError(const char* Code) { return {ErrorCategory::Unavailable, Code, {}, {}, true, "retry"}; }
    Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code}; }
    void WriteString(QDataStream& Out, const std::string& Value) { Out << QByteArray::fromStdString(Value); }
    void ReadString(QDataStream& In, std::string& Value) { QByteArray Bytes; In >> Bytes; Value = Bytes.toStdString(); }
}
struct DiskRecords::State
{
    QTemporaryDir directory_;
    mutable QFile data_, offsets_;
    mutable std::mutex mutex_;
    std::uint64_t count_ = 0;
    qint64 end_ = 0;
    explicit State(const QString& Parent) : directory_(QDir(Parent.isEmpty() ? QDir::tempPath() : Parent).filePath("hc-index-XXXXXX")),
        data_(directory_.filePath("records")), offsets_(directory_.filePath("offsets")) {}
    Result<Unit> write(std::uint64_t Row, const QByteArray& Bytes)
    {
        const auto Offset = end_;
        if (Bytes.size() > std::numeric_limits<qint64>::max() - Offset) return DiskError("index-file-too-large");
        if ((data_.pos() != Offset && !data_.seek(Offset)) || data_.write(Bytes) != Bytes.size())
            return DiskError("index-write-failed");
        end_ += Bytes.size();
        if (Row > std::uint64_t(std::numeric_limits<qint64>::max() / 16) ||
            (offsets_.pos() != qint64(Row * 16) && !offsets_.seek(qint64(Row * 16))))
            return DiskError("index-offset-failed");
        QDataStream Out(&offsets_); Out << quint64(Offset) << quint64(Bytes.size());
        if (Out.status() != QDataStream::Ok) return DiskError("index-write-failed");
        return Unit{};
    }
};
DiskRecords::DiskRecords(std::unique_ptr<State> Value) : state_(std::move(Value)) {}
DiskRecords::~DiskRecords() = default;
Result<std::shared_ptr<DiskRecords>> DiskRecords::Create(const QString& Parent)
{
    auto Value = std::make_unique<State>(Parent);
    if (!Value->directory_.isValid() || !Value->data_.open(QIODevice::ReadWrite) || !Value->offsets_.open(QIODevice::ReadWrite))
        return DiskError("index-create-failed");
    return std::shared_ptr<DiskRecords>(new DiskRecords(std::move(Value)));
}
std::uint64_t DiskRecords::Count() const { std::lock_guard Lock(state_->mutex_); return state_->count_; }
QString DiskRecords::Directory() const { return state_->directory_.path(); }
Result<Unit> DiskRecords::Flush()
{
    std::lock_guard Lock(state_->mutex_);
    if (!state_->data_.flush() || !state_->offsets_.flush()) return DiskError("index-write-failed");
    return Unit{};
}
Result<std::uint64_t> DiskRecords::Append(const QByteArray& Bytes)
{
    std::lock_guard Lock(state_->mutex_);
    auto Written = state_->write(state_->count_, Bytes);
    if (!Written.HasValue()) return Written.Failure();
    return state_->count_++;
}
Result<Unit> DiskRecords::Replace(std::uint64_t Row, const QByteArray& Bytes)
{
    std::lock_guard Lock(state_->mutex_);
    if (Row >= state_->count_) return Invalid("index-row-out-of-range");
    return state_->write(Row, Bytes);
}
Result<QByteArray> DiskRecords::Read(std::uint64_t Row) const
{
    std::lock_guard Lock(state_->mutex_);
    if (Row >= state_->count_) return Invalid("index-row-out-of-range");
    // Adjacent records already have the offset/data cursors in place.
    // Keep random access without repeating seek calls for sequential reads.
    if (!state_->data_.flush() || !state_->offsets_.flush() ||
        (state_->offsets_.pos() != qint64(Row * 16) && !state_->offsets_.seek(qint64(Row * 16)))) return DiskError("index-read-failed");
    QDataStream In(&state_->offsets_); quint64 Offset = 0, Size = 0; In >> Offset >> Size;
    const auto End = state_->data_.size();
    if (In.status() != QDataStream::Ok || End < 0 || Offset > quint64(End) || Size > quint64(End) - Offset ||
        Size > quint64(std::numeric_limits<qsizetype>::max()) ||
        (state_->data_.pos() != qint64(Offset) && !state_->data_.seek(qint64(Offset)))) return DiskError("index-corrupt");
    auto Bytes = state_->data_.read(qint64(Size));
    if (quint64(Bytes.size()) != Size) return DiskError("index-read-failed");
    return Bytes;
}
QByteArray EncodeEntry(const EntrySnapshot& Entry)
{
    QByteArray Bytes; QDataStream Out(&Bytes, QIODevice::WriteOnly); Out.setVersion(QDataStream::Qt_6_0);
    Out << quint32(1);
    WriteString(Out, Entry.id_.providerId_.value_); WriteString(Out, Entry.id_.key_);
    Out << quint8(Entry.id_.scope_) << bool(Entry.id_.revision_);
    if (Entry.id_.revision_) WriteString(Out, Entry.id_.revision_->token_);
    Out << quint8(Entry.locator_.kind_); WriteString(Out, Entry.locator_.root_);
    Out << quint64(Entry.locator_.nesting_.size());
    for (const auto& Hop : Entry.locator_.nesting_) { WriteString(Out, Hop.provider_); WriteString(Out, Hop.entry_); }
    WriteString(Out, Entry.name_);
    Out << Entry.directory_ << quint64(Entry.size_) << quint32(Entry.attributes_) << qint64(Entry.createdUtcMs_) << qint64(Entry.modifiedUtcMs_);
    Out << quint64(Entry.hints_.size());
    for (const auto& [Key, Value] : Entry.hints_) { WriteString(Out, Key); WriteString(Out, Value); }
    return Bytes;
}
Result<EntrySnapshot> DecodeEntry(const QByteArray& Bytes)
{
    QDataStream In(Bytes); In.setVersion(QDataStream::Qt_6_0);
    EntrySnapshot Entry; quint32 Version = 0, Attributes = 0; quint8 Scope = 0, Kind = 0; bool Revision = false;
    quint64 Count = 0, Size = 0; qint64 Created = 0, Modified = 0;
    In >> Version; if (Version != 1) return DiskError("index-record-version");
    ReadString(In, Entry.id_.providerId_.value_); ReadString(In, Entry.id_.key_); In >> Scope >> Revision;
    if (Scope > quint8(IdentityScope::NavigationSession)) return DiskError("index-corrupt");
    Entry.id_.scope_ = IdentityScope(Scope);
    if (Revision) { Entry.id_.revision_.emplace(); ReadString(In, Entry.id_.revision_->token_); }
    In >> Kind; if (Kind > quint8(LocatorKind::Plugin)) return DiskError("index-corrupt");
    Entry.locator_.kind_ = LocatorKind(Kind); ReadString(In, Entry.locator_.root_);
    In >> Count; if (Count > quint64(Bytes.size()) / 8) return DiskError("index-corrupt");
    for (quint64 I = 0; I < Count; ++I) { ContainerHop Hop; ReadString(In, Hop.provider_); ReadString(In, Hop.entry_); Entry.locator_.nesting_.push_back(std::move(Hop)); }
    ReadString(In, Entry.name_); In >> Entry.directory_ >> Size >> Attributes >> Created >> Modified;
    Entry.size_ = Size; Entry.attributes_ = Attributes; Entry.createdUtcMs_ = Created; Entry.modifiedUtcMs_ = Modified;
    In >> Count; if (Count > quint64(Bytes.size()) / 8) return DiskError("index-corrupt");
    for (quint64 I = 0; I < Count; ++I) { std::string Key, Value; ReadString(In, Key); ReadString(In, Value); Entry.hints_.emplace(std::move(Key), std::move(Value)); }
    if (In.status() != QDataStream::Ok || !In.atEnd()) return DiskError("index-corrupt");
    return Entry;
}
std::uint64_t PagedEntryIndex::Count() const { return records_->Count(); }
Result<std::shared_ptr<PagedEntryIndex>> PagedEntryIndex::Build(IEntryCursor& Cursor, const OperationContext& Context, const QString& Parent)
{
    auto Created = DiskRecords::Create(Parent); if (!Created.HasValue()) return Created.Failure();
    auto Records = Created.Value();
    std::optional<ResourceRevision> Revision; bool First = true;
    for (;;) {
        if (auto Failure = Context.Check({})) return *Failure;
        auto Page = Cursor.Next(std::max<std::size_t>(1, Context.maxPageItems_), Context).get();
        if (!Page.HasValue()) return Page.Failure();
        if (First) { Revision = Page.Value().revision_; First = false; }
        else if (Revision != Page.Value().revision_) return Error{ErrorCategory::StaleRevision, "enumeration-revision-changed"};
        if (Records->Count() > Context.EnumerationLimit() || Page.Value().entries_.size() > Context.EnumerationLimit() - Records->Count())
            return Error{ErrorCategory::BudgetExceeded, "entry-limit"};
        if (Page.Value().entries_.empty() && !Page.Value().end_) return Invalid("cursor-no-progress");
        for (const auto& Entry : Page.Value().entries_) {
            if (auto Failure = Context.Check({})) return *Failure;
            auto Written = Records->Append(EncodeEntry(Entry)); if (!Written.HasValue()) return Written.Failure();
        }
        if (Context.progress_) Context.progress_(Records->Count());
        if (Page.Value().end_) break;
    }
    auto Flushed = Records->Flush(); if (!Flushed.HasValue()) return Flushed.Failure();
    return std::shared_ptr<PagedEntryIndex>(new PagedEntryIndex(std::move(Records), std::move(Revision)));
}
Result<EntryPage> PagedEntryIndex::Read(std::uint64_t Offset, std::size_t MaxItems, const OperationContext& Context) const
{
    if (!MaxItems || Offset > Count()) return Invalid("index-page-out-of-range");
    if (auto Failure = Context.Check({})) return *Failure;
    EntryPage Page; Page.revision_ = revision_;
    const auto End = Offset + std::min<std::uint64_t>(Count() - Offset, MaxItems);
    for (auto Row = Offset; Row < End; ++Row) {
        if (auto Failure = Context.Check({})) return *Failure;
        auto Bytes = records_->Read(Row); if (!Bytes.HasValue()) return Bytes.Failure();
        auto Entry = DecodeEntry(Bytes.Value()); if (!Entry.HasValue()) return Entry.Failure();
        Page.entries_.push_back(std::move(Entry.Value()));
    }
    Page.end_ = End == Count(); return Page;
}
Result<std::shared_ptr<PagedEntryIndex>> PagedEntryIndex::Project(const Less& Compare, const Accept& Filter,
    const OperationContext& Context, SortBuffer Buffer) const
{
    if (!Buffer.entries_ || !Buffer.bytes_ || Buffer.fanIn_ < 2 || Buffer.fanIn_ > 64) return Invalid("invalid-sort-buffer");
    if (auto Failure = Context.Check({})) return *Failure;
    struct Row { EntrySnapshot entry_; QByteArray bytes_; std::uint64_t ordinal_ = 0; };
    const auto Before = [&](const Row& A, const Row& B) {
        if (!Compare) return A.ordinal_ < B.ordinal_;
        // On equal keys the earlier ordinal wins. Test whether the later row
        // precedes it, so stable ordering needs only one expensive comparison.
        return A.ordinal_ < B.ordinal_ ? !Compare(B.entry_, A.entry_) : Compare(A.entry_, B.entry_);
    };
    // Runs contain original ordinals for stable ties across merge passes.
    const auto Encode = [](const Row& Value) { QByteArray Bytes; QDataStream Out(&Bytes, QIODevice::WriteOnly); Out << quint64(Value.ordinal_) << Value.bytes_; return Bytes; };
    const auto Decode = [](const QByteArray& Bytes) -> Result<Row> {
        QDataStream In(Bytes); quint64 Ordinal = 0; QByteArray Payload; In >> Ordinal >> Payload;
        if (In.status() != QDataStream::Ok || !In.atEnd()) return DiskError("index-corrupt");
        auto Entry = DecodeEntry(Payload); if (!Entry.HasValue()) return Entry.Failure();
        return Row{std::move(Entry.Value()), std::move(Payload), Ordinal};
    };
    // Leveled compaction bounds both open files and the run registry. A million
    // tiny runs must not itself become an unbounded in-memory directory.
    std::vector<std::vector<std::shared_ptr<DiskRecords>>> Levels;
    const auto Merge = [&](const std::vector<std::shared_ptr<DiskRecords>>& Runs) -> Result<std::shared_ptr<DiskRecords>> {
        auto Output = DiskRecords::Create(); if (!Output.HasValue()) return Output.Failure();
        struct Head { Row value_; std::size_t run_; std::uint64_t next_; };
        const auto Later = [&](const Head& A, const Head& B) { return Before(B.value_, A.value_); };
        std::priority_queue<Head, std::vector<Head>, decltype(Later)> Heap(Later);
        for (std::size_t I = 0; I < Runs.size(); ++I) if (Runs[I]->Count()) {
            auto Bytes = Runs[I]->Read(0); if (!Bytes.HasValue()) return Bytes.Failure();
            auto Value = Decode(Bytes.Value()); if (!Value.HasValue()) return Value.Failure();
            Heap.push({std::move(Value.Value()), I, 1});
        }
        while (!Heap.empty()) {
            if (auto Failure = Context.Check({})) return *Failure;
            auto Head = Heap.top(); Heap.pop();
            auto Written = Output.Value()->Append(Encode(Head.value_)); if (!Written.HasValue()) return Written.Failure();
            if (Head.next_ < Runs[Head.run_]->Count()) {
                auto Bytes = Runs[Head.run_]->Read(Head.next_++); if (!Bytes.HasValue()) return Bytes.Failure();
                auto Value = Decode(Bytes.Value()); if (!Value.HasValue()) return Value.Failure();
                Head.value_ = std::move(Value.Value()); Heap.push(std::move(Head));
            }
        }
        auto Flushed = Output.Value()->Flush(); if (!Flushed.HasValue()) return Flushed.Failure();
        return Output.Value();
    };
    std::uint64_t Offset = 0;
    while (Offset < Count()) {
        std::vector<Row> Rows; std::size_t BytesUsed = 0;
        while (Offset < Count() && Rows.size() < Buffer.entries_ && BytesUsed < Buffer.bytes_) {
            if (auto Failure = Context.Check({})) return *Failure;
            const auto Ordinal = Offset++;
            auto Bytes = records_->Read(Ordinal); if (!Bytes.HasValue()) return Bytes.Failure();
            auto Entry = DecodeEntry(Bytes.Value()); if (!Entry.HasValue()) return Entry.Failure();
            if (Filter && !Filter(Entry.Value())) continue;
            // Encoded payload plus decoded strings/map nodes and row overhead.
            BytesUsed += std::size_t(Bytes.Value().size()) * 3 + sizeof(Row) + Entry.Value().hints_.size() * 96;
            Rows.push_back({std::move(Entry.Value()), std::move(Bytes.Value()), Ordinal});
        }
        std::sort(Rows.begin(), Rows.end(), Before);
        auto Run = DiskRecords::Create(); if (!Run.HasValue()) return Run.Failure();
        for (const auto& Row : Rows) {
            if (auto Failure = Context.Check({})) return *Failure;
            auto Written = Run.Value()->Append(Encode(Row)); if (!Written.HasValue()) return Written.Failure();
        }
        auto Flushed = Run.Value()->Flush(); if (!Flushed.HasValue()) return Flushed.Failure();
        std::size_t Level = 0;
        for (;;) {
            if (Level == Levels.size()) Levels.emplace_back();
            Levels[Level].push_back(Run.Value());
            if (Levels[Level].size() < Buffer.fanIn_) break;
            Run = Merge(Levels[Level]); if (!Run.HasValue()) return Run.Failure();
            Levels[Level++].clear();
        }
    }
    std::shared_ptr<DiskRecords> Combined;
    for (auto& Level : Levels) {
        if (Combined) Level.push_back(std::move(Combined));
        if (Level.empty()) continue;
        auto Merged = Merge(Level); if (!Merged.HasValue()) return Merged.Failure();
        Combined = Merged.Value(); Level.clear();
    }
    auto Output = DiskRecords::Create(); if (!Output.HasValue()) return Output.Failure();
    for (std::uint64_t I = 0; Combined && I < Combined->Count(); ++I) {
        if (auto Failure = Context.Check({})) return *Failure;
        auto Bytes = Combined->Read(I); if (!Bytes.HasValue()) return Bytes.Failure();
        auto Value = Decode(Bytes.Value()); if (!Value.HasValue()) return Value.Failure();
        auto Written = Output.Value()->Append(Value.Value().bytes_); if (!Written.HasValue()) return Written.Failure();
    }
    auto Flushed = Output.Value()->Flush(); if (!Flushed.HasValue()) return Flushed.Failure();
    return std::shared_ptr<PagedEntryIndex>(new PagedEntryIndex(Output.Value(), revision_));
}
namespace
{
    class Selection final : public ISelectionManifest
    {
        std::shared_ptr<const PagedEntryIndex> source_;
        std::shared_ptr<DiskRecords> ranges_;
        std::uint64_t count_ = 0;
    public:
        Selection(std::shared_ptr<const PagedEntryIndex> Source, std::shared_ptr<DiskRecords> Ranges, std::uint64_t Count)
            : source_(std::move(Source)), ranges_(std::move(Ranges)), count_(Count) {}
        std::uint64_t Count() const override { return count_; }
        Result<EntryPage> Read(std::uint64_t Offset, std::size_t MaxItems, const OperationContext& Context) const override
        {
            if (!MaxItems || Offset > count_) return Invalid("selection-page-out-of-range");
            if (auto Failure = Context.Check({})) return *Failure;
            const auto Start = Offset;
            EntryPage Page;
            const auto RangeAt = [&](std::uint64_t Row) -> Result<std::array<quint64, 3>> {
                auto Bytes = ranges_->Read(Row); if (!Bytes.HasValue()) return Bytes.Failure();
                QDataStream In(Bytes.Value()); std::array<quint64, 3> Value{}; In >> Value[0] >> Value[1] >> Value[2];
                if (In.status() != QDataStream::Ok || !In.atEnd()) return DiskError("selection-corrupt");
                return Value;
            };
            std::uint64_t Low = 0, High = ranges_->Count();
            while (Low < High) {
                const auto Mid = Low + (High - Low) / 2;
                auto Range = RangeAt(Mid); if (!Range.HasValue()) return Range.Failure();
                if (Range.Value()[2] <= Offset) Low = Mid + 1; else High = Mid;
            }
            for (auto Row = Low; Row < ranges_->Count() && Page.entries_.size() < MaxItems; ++Row) {
                auto Range = RangeAt(Row); if (!Range.HasValue()) return Range.Failure();
                const auto& Value = Range.Value();
                const auto Within = Offset - (Value[2] - Value[1]);
                const auto Size = std::min<std::uint64_t>(Value[1] - Within, MaxItems - Page.entries_.size());
                auto Part = source_->Read(Value[0] + Within, std::size_t(Size), Context); if (!Part.HasValue()) return Part.Failure();
                Page.revision_ = Part.Value().revision_;
                for (auto& Entry : Part.Value().entries_) Page.entries_.push_back(std::move(Entry));
                Offset += Size;
            }
            Page.end_ = Start + Page.entries_.size() == count_;
            return Page;
        }
    };
}
Result<std::shared_ptr<ISelectionManifest>> PagedEntryIndex::Select(const std::vector<EntryRange>& Ranges, const OperationContext& Context) const
{
    std::size_t Offset = 0;
    return SelectRanges([&]() -> Result<std::optional<EntryRange>> {
        if (Offset == Ranges.size()) return std::optional<EntryRange>{};
        return std::optional<EntryRange>{Ranges[Offset++]};
    }, Context);
}
Result<std::shared_ptr<ISelectionManifest>> PagedEntryIndex::SelectRanges(const NextRange& Next, const OperationContext& Context) const
{
    if (!Next) return Invalid("missing-selection-source");
    if (auto Failure = Context.Check({})) return *Failure;
    std::uint64_t End = 0, Count = 0;
    auto Manifest = DiskRecords::Create(); if (!Manifest.HasValue()) return Manifest.Failure();
    for (;;) {
        if (auto Failure = Context.Check({})) return *Failure;
        auto NextValue = Next(); if (!NextValue.HasValue()) return NextValue.Failure();
        if (!NextValue.Value()) break;
        const auto& Range = *NextValue.Value();
        if (Range.offset_ < End || Range.offset_ > this->Count() || Range.count_ > this->Count() - Range.offset_) return Invalid("invalid-selection-ranges");
        End = Range.offset_ + Range.count_;
        if (!Range.count_) continue;
        Count += Range.count_;
        QByteArray Bytes; QDataStream Out(&Bytes, QIODevice::WriteOnly); Out << quint64(Range.offset_) << quint64(Range.count_) << quint64(Count);
        auto Written = Manifest.Value()->Append(Bytes); if (!Written.HasValue()) return Written.Failure();
    }
    auto Flushed = Manifest.Value()->Flush(); if (!Flushed.HasValue()) return Flushed.Failure();
    // The facade shares files, not the lifetime of the UI/provider object.
    auto Source = std::shared_ptr<PagedEntryIndex>(new PagedEntryIndex(records_, revision_));
    return std::shared_ptr<ISelectionManifest>(new Selection(std::move(Source), Manifest.Value(), Count));
}
Result<std::shared_ptr<ISelectionManifest>> PagedEntryIndex::SelectAll(const OperationContext& Context) const { return Select({{0, Count()}}, Context); }
}
