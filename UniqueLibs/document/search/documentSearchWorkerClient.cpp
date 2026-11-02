#include "stdafx.h"
#include "documentSearchWorkerClient.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QRandomGenerator>

namespace hc::document::search {
WorkerClient::WorkerClient(QObject* Parent) : QObject(Parent)
{
    process_ = new QProcess;
    watchdog_.setSingleShot(true); cancelTimer_.setSingleShot(true); cancelTimer_.setInterval(150);
    connect(&watchdog_, &QTimer::timeout, this, [this] { fail(QStringLiteral("검색 helper 응답 시간 초과")); });
    connect(&cancelTimer_, &QTimer::timeout, this, [this] { process_->kill(); ready_ = busy_ = false; resident_.clear(); emit Canceled(); });
    connect(process_, &QProcess::started, this, [this] { wire::Writer Hello; Hello.U32(chunkBytes_); send(wire::Type::Hello, Hello.bytes_); watchdog_.start(10000); });
    connect(process_, &QProcess::readyReadStandardOutput, this, &WorkerClient::receive);
    connect(process_, &QProcess::readyReadStandardError, this, [this] { process_->readAllStandardError(); });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) { if (!canceling_) fail(process_->errorString()); });
    connect(process_, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        ready_ = busy_ = false; resident_.clear(); watchdog_.stop(); cancelTimer_.stop(); emit ReadyChanged();
        if (!canceling_ && !failed_) fail(QStringLiteral("검색 helper가 종료되었습니다."));
    });
}
WorkerClient::~WorkerClient()
{
    process_->disconnect(this);
    if (process_->state() == QProcess::NotRunning) delete process_;
    else {
        // Reap asynchronously: destroying QProcess on the GUI thread waits for termination.
        connect(process_, &QProcess::finished, process_, &QObject::deleteLater);
        process_->kill();
    }
}
void WorkerClient::Start(const QString& Program)
{
    if (process_->state() != QProcess::NotRunning) return;
    decoder_ = {}; incoming_ = outgoing_ = 0; instance_ = QRandomGenerator::global()->generate64() | 1;
    generation_ = 0;
    ready_ = failed_ = canceling_ = busy_ = false; resident_.clear(); upload_.clear(); uploadId_.clear(); pendingChunks_.clear(); batch_.clear();
    process_->setProgram(Program.isEmpty() ? QCoreApplication::applicationDirPath() + "/hc-search-index.exe" : Program);
    process_->setArguments({"--document-search"}); process_->start();
}
void WorkerClient::send(wire::Type Type, QByteArray Payload)
{
    const auto Bytes = wire::Encode({Type, instance_, generation_, outgoing_++, std::move(Payload)});
    if (Bytes.isEmpty() || process_->write(Bytes) != Bytes.size()) fail("ipc-write-failed");
}
void WorkerClient::RegisterSnapshot(const QString& Id, const QString& Text)
{
    if (!ready_ || Busy()) { fail("snapshot-client-busy"); return; }
    if (resident_.contains(Id)) { emit Registered(Id); return; }
    uploadId_ = Id; upload_ = wire::EncodeText(Text); uploadOffset_ = 0; uploadEnd_ = false;
    wire::Writer Begin; Begin.Text(Id); Begin.U64(upload_.size()); send(wire::Type::PutBegin, Begin.bytes_);
    watchdog_.start(30000); pump();
}
void WorkerClient::pump()
{
    while (!uploadId_.isEmpty() && !uploadEnd_ && pendingChunks_.size() < 4 && uploadOffset_ < upload_.size()) {
        const auto Bytes = upload_.mid(uploadOffset_, chunkBytes_); wire::Writer Chunk; Chunk.U64(uploadOffset_); Chunk.Bytes(Bytes);
        pendingChunks_.insert(outgoing_); uploadOffset_ += Bytes.size(); send(wire::Type::PutChunk, Chunk.bytes_);
    }
    if (!uploadId_.isEmpty() && !uploadEnd_ && uploadOffset_ == upload_.size() && pendingChunks_.isEmpty()) {
        uploadEnd_ = true; send(wire::Type::PutEnd);
    }
}
void WorkerClient::Search(const QString& Id, const QString& Path, const QString& Query, Options Settings, quint64 Generation)
{
    if (!ready_ || Busy()) { fail("search-client-not-ready"); return; }
    busy_ = true;
    generation_ = Generation; canceling_ = false; batch_.clear(); batchSize_ = 0;
    wire::Writer Data; Data.Text(Id); Data.Text(Path); Data.Text(Query); Data.U32((Settings.regex_ ? 1 : 0) | (Settings.caseSensitive_ ? 2 : 0));
    send(wire::Type::Search, Data.bytes_); watchdog_.start(30000);
}
void WorkerClient::Cancel()
{
    watchdog_.stop(); canceling_ = true; busy_ = true; batch_.clear();
    if (ready_ && uploadId_.isEmpty()) { send(wire::Type::Cancel); cancelTimer_.start(); }
    else { ready_ = busy_ = false; resident_.clear(); process_->kill(); emit Canceled(); }
}
void WorkerClient::fail(const QString& Error)
{
    if (failed_) return;
    failed_ = true; ready_ = busy_ = false; watchdog_.stop(); cancelTimer_.stop(); process_->kill(); resident_.clear();
    emit Failed(generation_, Error);
}
void WorkerClient::receive()
{
    QElapsedTimer Budget; Budget.start();
    for (;;) {
        while (auto Frame = decoder_.Take()) {
            dispatch(*Frame);
            if (failed_) return;
            // Even a continuously readable pipe must yield to input and paint events.
            // Outstanding worker credits bound the backlog while this continuation waits.
            if (Budget.elapsed() >= 4) {
                if (!receiveScheduled_) {
                    receiveScheduled_ = true;
                    QTimer::singleShot(0, this, [this] { receiveScheduled_ = false; receive(); });
                }
                return;
            }
        }
        if (!decoder_.Error().isEmpty()) { fail(decoder_.Error()); return; }
        if (!process_->bytesAvailable()) return;
        if (!decoder_.Append(process_->read(64 * 1024))) { fail(decoder_.Error()); return; }
    }
}
void WorkerClient::dispatch(const wire::Frame& Frame)
{
    using wire::Type;
    if (Frame.instance_ != instance_ || Frame.sequence_ != incoming_++) { fail("ipc-instance-sequence"); return; }
    wire::Reader Data(Frame.payload_);
    if (Frame.type_ == Type::Ready) { watchdog_.stop(); ready_ = true; emit ReadyChanged(); return; }
    if (Frame.type_ == Type::Credit) {
        const auto Seq = Data.U32(); if (!Data.Done() || !pendingChunks_.remove(Seq)) { fail("ipc-invalid-upload-credit"); return; }
        watchdog_.start(30000); pump(); return;
    }
    if (Frame.type_ == Type::SnapshotReady) {
        const auto Id = Data.Text(); if (!Data.Done() || Id != uploadId_) { fail("snapshot-invalid-ack"); return; }
        watchdog_.stop(); resident_.insert(Id); uploadId_.clear(); upload_.clear(); emit Registered(Id); return;
    }
    if (Frame.type_ == Type::Release) { const auto Id = Data.Text(); if (!Data.Done()) { fail("snapshot-invalid-eviction"); return; } resident_.remove(Id); return; }
    if (Frame.type_ == Type::Canceled) { watchdog_.stop(); cancelTimer_.stop(); busy_ = false; emit Canceled(); return; }
    if (Frame.generation_ != generation_ || canceling_) return;
    if (Frame.type_ == Type::Started) {
        const auto Phase = Data.Text(); if (!Data.Done()) { fail("ipc-invalid-phase"); return; }
        if (Phase == "waiting") watchdog_.stop(); else watchdog_.start(Phase == "matching" ? 1500 : 30000); return;
    }
    if (Frame.type_ == Type::Error) {
        const auto Error = Data.Text(); if (!Data.Done()) { fail("ipc-invalid-error"); return; }
        watchdog_.stop(); busy_ = false; emit Failed(generation_, Error); return;
    }
    if (Frame.type_ == Type::SourceMetadata) {
        const auto Hash = Data.Bytes(); const auto Encoding = Data.Text(); const auto Bom = Data.U32();
        if (!Data.Done() || Hash.size() != 32 || Bom > 1) { fail("ipc-invalid-source"); return; }
        emit SourceMetadata(Hash, Encoding, Bom != 0); return;
    }
    if (Frame.type_ == Type::Search) {
        const auto Count = Data.U32(); if (Count > 65536) { fail("ipc-invalid-groups"); return; }
        QStringList Groups; for (quint32 I = 0; I < Count; ++I) Groups.append(Data.Text());
        if (!Data.Done()) { fail("ipc-invalid-groups"); return; } emit PatternGroups(Groups); return;
    }
    if (Frame.type_ == Type::MatchBatch) {
        QVector<Match> Hits;
        const auto Total = Data.U64(), Offset = Data.U64(); const auto Bytes = Data.Bytes();
        if (!Data.Done() || Total > 2 * 1024 * 1024 || Offset != quint64(batch_.size()) || Offset > Total || quint64(Bytes.size()) > Total - Offset || (Offset && Total != batchSize_)) { fail("ipc-invalid-batch-chunk"); return; }
        batchSize_ = Total; batch_ += Bytes;
        if (quint64(batch_.size()) == Total) {
            wire::Reader Rows(std::move(batch_)); batch_.clear(); const auto Count = Rows.U32();
            if (Count > 100) { fail("ipc-invalid-batch-count"); return; }
            for (quint32 I = 0; I < Count; ++I) {
                Match Hit; Hit.range_ = {qint64(Rows.U64()), qint64(Rows.U64())}; Hit.line_ = qint64(Rows.U64()); Hit.column_ = qint64(Rows.U64());
                const auto Captures = Rows.U32(); if (Captures > 65536) { fail("ipc-invalid-captures"); return; }
                for (quint32 C = 0; C < Captures; ++C) Hit.captures_.append({qint64(Rows.U64()), qint64(Rows.U64())});
                if (Hit.range_.start_ < 0 || Hit.range_.length_ < 0 || Hit.line_ < 1 || Hit.column_ < 1) { fail("ipc-invalid-range"); return; }
                Hits.append(std::move(Hit));
            }
            if (!Rows.Done()) { fail("ipc-invalid-batch"); return; }
        }
        // Slots may cancel or replace the request. Settle this frame before calling them.
        wire::Writer Credit; Credit.U32(1); send(Type::Credit, Credit.bytes_); watchdog_.start(1500);
        if (!Hits.isEmpty()) emit Matches(generation_, Hits);
        return;
    }
    if (Frame.type_ == Type::FileComplete) return;
    if (Frame.type_ == Type::Complete) { watchdog_.stop(); if (!batch_.isEmpty()) { fail("ipc-incomplete-batch"); return; } busy_ = false; emit Finished(generation_); return; }
    fail("ipc-unexpected-response");
}
}
