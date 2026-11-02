#pragma once
#include "core/searchWorkerProtocol.hpp"
#include "core/textSearchMatcher.hpp"
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QTimer>

namespace hc::document::search {
class WorkerClient final : public QObject {
    Q_OBJECT
public:
    explicit WorkerClient(QObject* Parent = nullptr);
    ~WorkerClient() override;
    void Start(const QString& Program = {});
    void RegisterSnapshot(const QString& Id, const QString& Text);
    void Search(const QString& Id, const QString& Path, const QString& Query, Options Settings, quint64 Generation);
    void Cancel();
    bool Ready() const { return ready_; }
    bool Busy() const { return busy_ || !uploadId_.isEmpty(); }
    bool IsRunning() const { return process_->state() != QProcess::NotRunning; }
    bool Resident(const QString& Id) const { return resident_.contains(Id); }
    void SetChunkBytes(quint32 Bytes) { chunkBytes_ = qBound<quint32>(64, Bytes, wire::MaxPayload - 32); }
signals:
    void ReadyChanged();
    void Registered(const QString& Id);
    void PatternGroups(const QStringList& Groups);
    void SourceMetadata(const QByteArray& Hash, const QString& Encoding, bool Bom);
    void Matches(quint64 Generation, const QVector<hc::document::search::Match>& Hits);
    void Finished(quint64 Generation);
    void Failed(quint64 Generation, const QString& Error);
    void Canceled();
private:
    void send(wire::Type Type, QByteArray Payload = {});
    void receive();
    void dispatch(const wire::Frame& Frame);
    void pump();
    void fail(const QString& Error);
    QProcess* process_ = nullptr;
    QTimer watchdog_, cancelTimer_;
    wire::Decoder decoder_;
    quint64 instance_ = 0, generation_ = 0;
    quint32 incoming_ = 0, outgoing_ = 0, chunkBytes_ = 64 * 1024;
    bool ready_ = false, failed_ = false, canceling_ = false, uploadEnd_ = false, busy_ = false, receiveScheduled_ = false;
    QString uploadId_;
    QByteArray upload_, batch_;
    qsizetype uploadOffset_ = 0;
    quint64 batchSize_ = 0;
    QSet<quint32> pendingChunks_;
    QSet<QString> resident_;
};
}
