#pragma once
#include "documentSearchWorkerClient.hpp"
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QTemporaryDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <atomic>
#include <QLockFile>
#include <QFile>

namespace hc::document::search {
// All methods except RequestStop execute on the dedicated session thread.
// The GUI receives counters and requested pages, never an unbounded stream of rows.
class Engine final : public QObject {
    Q_OBJECT
public:
    Engine();
    ~Engine() override;
    void RequestStop() { stop_.store(true); }
    void Begin(quint64 Generation, QJsonObject Request, QJsonArray Buffers, std::function<QJsonArray()> BookSources = {});
    void Page(quint64 Generation, int First, int Count, QString ReplacementText, bool ReplaceMode, quint64 PreviewVersion);
    void Stop();
    void Shutdown();
    void SetWriter(std::function<QString(const QString&, const QByteArray&, const QByteArray&)> Writer) { writer_ = std::move(Writer); }
    void SetJournalDirectory(QString Directory) { journalDirectory_ = std::move(Directory); }
    void Apply(quint64 Generation, QString Scope, QJsonArray Ids, QString ReplacementText);
    void Undo();
    void BufferApplied(quint64 Ticket, const QString& Error);
signals:
    void Progress(quint64 Generation, qint64 Rows, qint64 Files, qint64 Errors, bool Complete, const QString& Message);
    void PageReady(quint64 Generation, int First, const QJsonArray& Rows, quint64 PreviewVersion);
    void EditBuffer(quint64 Ticket, const QJsonObject& Source, const QString& Expected, const QString& Replacement);
    void ApplyProgress(bool Busy, qint64 Changed, const QString& Message);
    void DiagnosticLog(quint64 Generation, const QString& Path);
private:
    bool initialize();
    void next();
    void searchFile(QJsonObject Source);
    void append(const QVector<Match>& Hits);
    void finishFile();
    void report(bool Complete = false, QString Message = {});
    void diagnostic(const QString& Path,const QString& Error);
    bool matchesFilter(const QString& Path, const QString& Patterns) const;
    std::optional<QString> sourceText(const QJsonObject& Source);
    bool journal();
    void applyNext();
    void commitEdit(QJsonObject Source, QString Before, QString After, qint64 JournalId, bool Undo);
    void editFinished(const QString& Error);
    std::function<QString(const QString&, const QByteArray&, const QByteArray&)> writer_;
    QSqlDatabase journal_;
    std::unique_ptr<QLockFile> journalLock_;
    QString journalPath_, journalDirectory_, applyScope_, replacementText_;
    qint64 operation_ = 0, applySource_ = 0, changed_ = 0, pendingJournal_ = 0;
    quint64 editTicket_ = 0;
    bool applying_ = false, undoing_ = false;
    QJsonArray pendingBook_;
    WorkerClient* worker_ = nullptr;
    QSqlDatabase database_;
    std::unique_ptr<QTemporaryDir> directory_;
    std::unique_ptr<QFile> errorLog_;
    QString lastError_;
    std::vector<std::unique_ptr<QDirIterator>> directories_;
    QJsonArray buffers_;
    QSet<QString> overlayPaths_;
    QJsonObject request_, current_;
    QStringList groups_;
    QString cachedId_, cachedText_;
    QString workerSourceId_;
    mutable QHash<QString, QList<QRegularExpression>> filters_;
    quint64 generation_ = 0;
    qint64 rows_ = 0, files_ = 0, errors_ = 0, sourceId_ = 0;
    int bufferIndex_ = 0;
    bool running_ = false, sourceInserted_ = false;
    QElapsedTimer reporting_;
    std::atomic_bool stop_{false};
};
}
