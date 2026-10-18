#pragma once
#include "qtPagedStorage.hpp"
#include "operationMetrics.hpp"
#include <QTemporaryFile>
#include <QDir>
#include <array>
#include <QDataStream>
#include <QIODevice>

namespace hc::operations {
// Derived pointers only: the checksummed, synchronously flushed outcome log
// remains authoritative. A single 64 KiB page bounds memory independently of
// job size. Eviction writes a rebuildable page, never a recovery checkpoint.
class OutcomePointerIndex final {
public:
    using Pointer = std::pair<std::uint64_t,std::uint64_t>;
    explicit OutcomePointerIndex(bool Buffered) : buffered_(Buffered), file_(QDir::temp().filePath("hc-outcome-index-XXXXXX")) {}
    storage::Result<storage::Unit> Open() {
        if (buffered_) { if (!file_.open()) return failure(); }
        else { auto Created=storage::DiskRecords::Create(); if (!Created.HasValue()) return Created.Failure(); legacy_=Created.Value(); }
        return storage::Unit{};
    }
    std::uint64_t Count() const { return buffered_ ? count_ : legacy_->Count(); }
    storage::Result<storage::Unit> Update(std::uint64_t Row,std::uint64_t Position,std::uint64_t Size) {
        OperationMetric Timing("recovery.index.update.nested");
        if (!buffered_) {
            QByteArray Bytes; QDataStream Out(&Bytes,QIODevice::WriteOnly); Out << quint64(Position) << quint64(Size);
            OperationMetric WriteTiming("recovery.index.write.nested",32);
            if (Row==legacy_->Count()) { auto Added=legacy_->Append(Bytes); if (!Added.HasValue()) return Added.Failure(); }
            else { auto Replaced=legacy_->Replace(Row,Bytes); if (!Replaced.HasValue()) return Replaced; }
            return storage::Unit{};
        }
        if (Row>count_) return failure();
        auto Loaded=load(Row/RowsPerPage); if (!Loaded.HasValue()) return Loaded;
        page_[Row%RowsPerPage]={Position,Size}; dirty_=true; if (Row==count_) ++count_;
        return storage::Unit{};
    }
    storage::Result<Pointer> Read(std::uint64_t Row) {
        if (Row>=Count()) return failure();
        if (!buffered_) {
            auto Bytes=legacy_->Read(Row); if (!Bytes.HasValue()) return Bytes.Failure();
            QDataStream In(Bytes.Value()); quint64 Position=0,Size=0; In >> Position >> Size;
            if (In.status()!=QDataStream::Ok || !In.atEnd()) return failure();
            return Pointer{Position,Size};
        }
        auto Loaded=load(Row/RowsPerPage); if (!Loaded.HasValue()) return Loaded.Failure();
        const auto& Value=page_[Row%RowsPerPage]; return Pointer{Value.position_,Value.size_};
    }
private:
    static storage::Error failure() { return {storage::ErrorCategory::Unavailable,"outcome-index-io",{"operation-journal"}}; }
    storage::Result<storage::Unit> load(std::uint64_t Page) {
        if (Page==pageNumber_) return storage::Unit{};
        if (Page>std::uint64_t(INT64_MAX)/PageBytes) return failure();
        if (dirty_) {
            OperationMetric Timing("recovery.index.write.nested",PageBytes);
            if (!file_.seek(qint64(pageNumber_*PageBytes)) || file_.write(reinterpret_cast<const char*>(page_.data()),PageBytes)!=PageBytes || !file_.flush()) return failure();
            dirty_=false;
        }
        if (Page*RowsPerPage<count_) {
            OperationMetric Timing("recovery.index.read.nested",PageBytes);
            std::array<Entry,RowsPerPage> Next{};
            if (!file_.seek(qint64(Page*PageBytes)) || file_.read(reinterpret_cast<char*>(Next.data()),PageBytes)!=PageBytes) return failure();
            page_=Next;
        } else page_.fill({});
        pageNumber_=Page; return storage::Unit{};
    }
    struct Entry { std::uint64_t position_=0,size_=0; };
    static_assert(sizeof(Entry)==16);
    static constexpr std::uint64_t RowsPerPage=4096,PageBytes=RowsPerPage*sizeof(Entry);
    bool buffered_=true,dirty_=false;
    QTemporaryFile file_; std::shared_ptr<storage::DiskRecords> legacy_;
    std::array<Entry,RowsPerPage> page_{};
    std::uint64_t count_=0,pageNumber_=UINT64_MAX;
};
}
