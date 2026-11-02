#include "stdafx.h"
#include "documentSearchMode.hpp"

/// 모듈
#include "UniqueLibs/document/search/core/searchWorkerProtocol.hpp"
#include "UniqueLibs/document/search/core/textSearchMatcher.hpp"
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/document/documentTextCodec.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <Windows.h>
#include <io.h>
#include <fcntl.h>
#include <thread>
#include <atomic>
#include <QSemaphore>

namespace hc::document::search {
namespace {
// This mode never constructs an Everything client and has no file-write operation.
class Server final {
public:
    int Run()
    {
        _setmode(_fileno(stdout), _O_BINARY);
        _setmode(_fileno(stdin), _O_BINARY);
        if (!output_.open(stdout, QIODevice::WriteOnly)) return 2;
        QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] { tick(); });
        timer_.setSingleShot(true);
        // Anonymous stdin pipes cannot use QWinEventNotifier directly. One
        // blocking reader wakes the event loop, avoiding a timer round trip per
        // small source file. Four queued chunks bound input memory as well.
        std::atomic_bool Stopping{false};QSemaphore InputBudget(4);
        std::thread Reader([this,&Stopping,&InputBudget] {
            while(!Stopping.load()) {
                char Bytes[64*1024];DWORD Read=0;
                if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE),Bytes,sizeof(Bytes),&Read,nullptr)||!Read) {
                    if(!Stopping.load())QMetaObject::invokeMethod(&timer_,[]{QCoreApplication::quit();},Qt::QueuedConnection);
                    break;
                }
                InputBudget.acquire();if(Stopping.load())break;
                QMetaObject::invokeMethod(&timer_,[this,Data=QByteArray(Bytes,Read),&InputBudget] {
                    InputBudget.release();decoder_.Append(Data);
                    while(auto Frame=decoder_.Take())receive(*Frame);
                    if(!decoder_.Error().isEmpty()){QCoreApplication::exit(2);return;}
                    tick();
                },Qt::QueuedConnection);
            }
        });
        const auto Result=QCoreApplication::exec();Stopping=true;InputBudget.release(4);
        while(WaitForSingleObject(Reader.native_handle(),10)==WAIT_TIMEOUT)CancelSynchronousIo(Reader.native_handle());
        Reader.join();return Result;
    }
private:
    QFile output_;
    QTimer timer_;
    wire::Decoder decoder_;
    quint64 instance_ = 0, generation_ = 0;
    quint32 incoming_ = 0, outgoing_ = 0, credit_ = 4, chunkBytes_ = 64 * 1024;
    QString transferId_, currentId_;
    QByteArray transfer_;
    quint64 transferSize_ = 0;
    QHash<QString, QString> snapshots_;
    QStringList recent_;
    qint64 cacheBytes_ = 0;
    std::unique_ptr<TextMap> text_;
    std::unique_ptr<Matcher> matcher_;
    std::unique_ptr<Matcher> cachedMatcher_;
    QString cachedQuery_;
    quint32 cachedFlags_ = 0;
    bool done_ = false, waiting_ = false;

    bool send(wire::Type Type, QByteArray Payload = {})
    {
        const auto Bytes = wire::Encode({Type, instance_, generation_, outgoing_++, std::move(Payload)});
        if (Bytes.isEmpty() || output_.write(Bytes) != Bytes.size() || !output_.flush()) { QCoreApplication::exit(3); return false; }
        return true;
    }
    void fail(const QString& Error)
    {
        matcher_.reset(); text_.reset(); done_ = false; pendingBatch_.clear(); pendingOffset_ = 0;
        wire::Writer Message; Message.Text(Error); send(wire::Type::Error, Message.bytes_);
    }
    void phase(const QString& Value) { wire::Writer Message; Message.Text(Value); send(wire::Type::Started, Message.bytes_); }
    void cache(const QString& Id, QString Text)
    {
        cacheBytes_ -= snapshots_.value(Id).size() * 2; recent_.removeAll(Id);
        while (!recent_.isEmpty() && cacheBytes_ + Text.size() * 2 > 128 * 1024 * 1024) {
            const auto Old = recent_.takeFirst(); cacheBytes_ -= snapshots_.take(Old).size() * 2;
            wire::Writer Message; Message.Text(Old); send(wire::Type::Release, Message.bytes_);
        }
        cacheBytes_ += Text.size() * 2; snapshots_.insert(Id, std::move(Text)); recent_.append(Id);
    }
    void receive(const wire::Frame& Frame)
    {
        using wire::Type;
        if (Frame.sequence_ != incoming_++) { QCoreApplication::exit(2); return; }
        if (!instance_) {
            if (Frame.type_ != Type::Hello || !Frame.instance_) { QCoreApplication::exit(2); return; }
            wire::Reader Hello(Frame.payload_); chunkBytes_ = Hello.U32();
            if (!Hello.Done() || chunkBytes_ < 64 || chunkBytes_ > wire::MaxPayload - 32) { QCoreApplication::exit(2); return; }
            instance_ = Frame.instance_; send(Type::Ready); return;
        }
        if (Frame.instance_ != instance_) { QCoreApplication::exit(2); return; }
        wire::Reader Data(Frame.payload_);
        if (Frame.type_ == Type::Shutdown) { QCoreApplication::quit(); return; }
        if (Frame.type_ == Type::Cancel) {
            matcher_.reset(); text_.reset(); done_ = false; credit_ = 4; waiting_ = false;
            pendingBatch_.clear(); pendingOffset_ = 0;
            generation_ = Frame.generation_; send(Type::Canceled); return;
        }
        if (Frame.type_ == Type::Credit) {
            const auto Count = Data.U32();
            if (!Data.Done() || Count > 4) { fail("ipc-invalid-credit"); return; }
            if (Frame.generation_ == generation_) credit_ = qMin<quint32>(4, credit_ + Count);
            return;
        }
        if (Frame.type_ == Type::PutBegin) {
            const auto Id = Data.Text(); const auto Size = Data.U64();
            if (!Data.Done() || Id.isEmpty() || !transferId_.isEmpty() || Size > 64 * 1024 * 1024 || Size % 2) { fail("snapshot-invalid-size"); return; }
            transferId_ = Id; transferSize_ = Size; transfer_.clear(); transfer_.reserve(qsizetype(Size)); return;
        }
        if (Frame.type_ == Type::PutChunk) {
            const auto Offset = Data.U64(); const auto Chunk = Data.Bytes();
            if (!Data.Done() || transferId_.isEmpty() || Offset != quint64(transfer_.size()) || quint64(Chunk.size()) > transferSize_ - Offset) { fail("snapshot-invalid-chunk"); return; }
            transfer_ += Chunk; wire::Writer Ack; Ack.U32(Frame.sequence_); send(Type::Credit, Ack.bytes_); return;
        }
        if (Frame.type_ == Type::PutEnd) {
            if (!Data.Done() || transferId_.isEmpty() || quint64(transfer_.size()) != transferSize_) { fail("snapshot-incomplete"); return; }
            auto Text = wire::DecodeText(transfer_);
            if (!Text) { fail("snapshot-invalid-utf16"); return; }
            const auto Id = std::exchange(transferId_, {}); transfer_.clear();
            cache(Id, std::move(*Text)); wire::Writer Ack; Ack.Text(Id); send(Type::SnapshotReady, Ack.bytes_); return;
        }
        if (Frame.type_ == Type::Release) {
            const auto Id = Data.Text(); if (!Data.Done()) { fail("snapshot-invalid-release"); return; }
            cacheBytes_ -= snapshots_.take(Id).size() * 2; recent_.removeAll(Id); return;
        }
        if (Frame.type_ != Type::Search || matcher_) { fail("ipc-unexpected-command"); return; }
        currentId_ = Data.Text(); const auto Path = Data.Text(), Query = Data.Text(); const auto Flags = Data.U32();
        if (!Data.Done() || Query.size() > 65536 || Flags > 3) { fail("search-invalid-request"); return; }
        generation_ = Frame.generation_; credit_ = 4; done_ = waiting_ = false;
        QString Original;
        if (Path.isEmpty()) {
            if (!snapshots_.contains(currentId_)) { fail("snapshot-not-resident"); return; }
            Original = snapshots_.value(currentId_); recent_.removeAll(currentId_); recent_.append(currentId_);
        } else {
            phase("reading"); QFile File(Path);
            if (!File.open(QIODevice::ReadOnly)) { fail(File.errorString()); return; }
            if (File.size() > 32 * 1024 * 1024) { fail("search-file-size-limit"); return; }
            const auto Prefix=File.peek(8192);
            if(!Prefix.startsWith("\xff\xfe") && !Prefix.startsWith("\xfe\xff") && Prefix.contains('\0')){fail("binary-file");return;}
            const auto Bytes = File.read(32 * 1024 * 1024 + 1);
            if (Bytes.size() > 32 * 1024 * 1024 || File.error() != QFileDevice::NoError) { fail("search-read-failed"); return; }
            const auto Decoded = DecodeDocumentText(Bytes);
            if (!Decoded.HasValue()) { fail(QString::fromStdString(Decoded.Failure().code_)); return; }
            Original = Decoded.Value().text_;
            wire::Writer Source; Source.Bytes(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256));
            Source.Text(Decoded.Value().encoding_); Source.U32(Decoded.Value().bom_ ? 1 : 0);
            send(Type::SourceMetadata, Source.bytes_);
        }
        if(cachedMatcher_ && cachedQuery_==Query && cachedFlags_==Flags)matcher_=std::move(cachedMatcher_);
        else {matcher_=std::make_unique<Matcher>(Query, Options{bool(Flags & 1), bool(Flags & 2)});cachedQuery_=Query;cachedFlags_=Flags;}
        if (!matcher_->Error().isEmpty()) { fail(matcher_->Error()); return; }
        text_ = std::make_unique<TextMap>(std::move(Original));
        wire::Writer Metadata; const auto Groups = matcher_->Groups(); Metadata.U32(quint32(Groups.size()));
        for (const auto& Group : Groups) Metadata.Text(Group);
        send(Type::Search, Metadata.bytes_); phase("matching"); matcher_->Start(*text_);
    }
    void tick()
    {
        if (!matcher_) return;
        if (!credit_) { if (!waiting_) { waiting_ = true; phase("waiting"); } return; }
        if (!pendingBatch_.isEmpty()) { flushBatch(); if(credit_)timer_.start(0); return; }
        if (done_) {
            if (credit_ == 4) { cachedMatcher_=std::move(matcher_); text_.reset(); send(wire::Type::FileComplete); send(wire::Type::Complete); }
            return;
        }
        if (waiting_) { waiting_ = false; phase("matching"); }
        wire::Writer Batch; QVector<Match> Hits; QElapsedTimer Clock; Clock.start();
        qsizetype Estimated = 4;
        while (Hits.size() < 100 && Clock.elapsed() < 30) {
            auto Hit = matcher_->Next(); if (!Hit) {
                if (!matcher_->Error().isEmpty()) { fail(matcher_->Error()); return; }
                done_ = true; break;
            }
            Estimated += 36 + Hit->captures_.size() * 16;
            // Captures are bounded by the regex engine; a single hit can span many wire chunks later.
            Hits.append(std::move(*Hit));
            if (Estimated > 128 * 1024) break;
        }
        if (!Hits.isEmpty()) {
            Batch.U32(quint32(Hits.size()));
            for (const auto& Hit : Hits) {
                Batch.U64(Hit.range_.start_); Batch.U64(Hit.range_.length_); Batch.U64(Hit.line_); Batch.U64(Hit.column_);
                Batch.U32(quint32(Hit.captures_.size()));
                for (const auto& Capture : Hit.captures_) { Batch.U64(quint64(Capture.start_)); Batch.U64(Capture.length_); }
            }
            // Split the logical batch, including an oversized capture record, without dropping a hit.
            pendingBatch_ = std::move(Batch.bytes_); pendingOffset_ = 0;
        }
        flushBatch();
        if(matcher_ && credit_)timer_.start(0);
    }
    QByteArray pendingBatch_; qsizetype pendingOffset_ = 0;
    void flushBatch()
    {
        while (credit_ && pendingOffset_ < pendingBatch_.size()) {
            wire::Writer Chunk; Chunk.U64(quint64(pendingBatch_.size())); Chunk.U64(pendingOffset_);
            const auto Part = pendingBatch_.mid(pendingOffset_, chunkBytes_); Chunk.Bytes(Part);
            pendingOffset_ += Part.size(); --credit_; send(wire::Type::MatchBatch, Chunk.bytes_);
        }
        if (pendingOffset_ == pendingBatch_.size()) { pendingBatch_.clear(); pendingOffset_ = 0; }
    }
};
}
int RunDocumentSearch() { Server Instance; return Instance.Run(); }
}
