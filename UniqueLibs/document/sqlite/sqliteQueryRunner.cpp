#include "stdafx.h"
#include "sqliteQueryRunner.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteApi.hpp"
#include "sqliteConnection.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <atomic>
#include <QElapsedTimer>
#include <QHash>
#include <QRegularExpression>
#include <QThread>

namespace hc::sqlite
{
    // 실행기 스레드 쪽 상태. GUI 는 db_ 를 잠금 아래에서만 본다(중단).
    struct QueryRunner::State
    {
        Opener open_;
        QPointer<QObject> receiver_;
        RunSink sink_;
        std::unique_ptr<Connection> db_;            // 실행기 스레드만 쓴다.
        QHash<int, std::shared_ptr<Statement>> pending_;
        quint64 batch_ = 0;
        std::mutex mutex_;
        Connection* live_ = nullptr;                // 중단용(잠금 아래)
        std::atomic_int busy_ {0};

        template <class Fn> void Post(Fn&& Send)
        {
            if (auto* Receiver = receiver_.data()) QMetaObject::invokeMethod(Receiver, std::forward<Fn>(Send), Qt::QueuedConnection);
        }

        // 남은 행을 Count 개까지 읽는다. More 는 행이 더 있는가, Error 는 실패 메시지.
        QVector<QVector<QVariant>> Fetch(Statement& Query, int Count, bool& More, QString& Error)
        {
            QVector<QVector<QVariant>> Rows;
            const int Columns = Query.ColumnCount();
            More = false;
            while (Rows.size() < Count) {
                const int Rc = Query.Step();
                if (Rc == SQLITE_ROW) {
                    QVector<QVariant> Row(Columns);
                    for (int Column = 0; Column < Columns; ++Column) Row[Column] = Query.Value(Column);
                    Rows.append(std::move(Row));
                    continue;
                }
                if (Rc != SQLITE_DONE) Error = Query.Error();
                return Rows;
            }
            // 행 제한에 닿았다: 다음 행이 있는지는 다음 Step 이 안다. 있다고 보고, 다음 요청에서 0행이면 끝낸다.
            More = true;
            return Rows;
        }

        void Close()
        {
            pending_.clear();
            {
                std::lock_guard Lock(mutex_);
                live_ = nullptr;
            }
            db_.reset();
        }
    };

    QueryRunner::QueryRunner(Opener OpenConnection, QObject* Receiver, RunSink Sink)
        : state_(std::make_shared<State>())
    {
        state_->open_ = std::move(OpenConnection);
        state_->receiver_ = Receiver;
        state_->sink_ = std::move(Sink);
        thread_ = new QThread;
        thread_->setObjectName(QStringLiteral("sqliteQueryRunner"));
        context_ = new QObject;
        context_->moveToThread(thread_);
        QObject::connect(thread_, &QThread::finished, context_, &QObject::deleteLater);
        thread_->start();
    }

    QueryRunner::~QueryRunner()
    {
        Release();
        thread_->quit();
        thread_->wait();
        delete thread_;
    }

    bool QueryRunner::Busy() const { return state_->busy_.load() > 0; }

    void QueryRunner::Interrupt()
    {
        std::lock_guard Lock(state_->mutex_);
        if (state_->live_) state_->live_->Interrupt();
    }

    void QueryRunner::Release()
    {
        Interrupt();
        auto State = state_;
        QMetaObject::invokeMethod(context_, [State] { State->Close(); }, Qt::BlockingQueuedConnection);
    }

    bool QueryRunner::NonTransactional(const QString& Sql)
    {
        static const QRegularExpression Pattern(QStringLiteral("^\\s*(?:VACUUM|ATTACH|DETACH)\\b"), QRegularExpression::CaseInsensitiveOption);
        return Pattern.match(Sql).hasMatch();
    }

    bool QueryRunner::ExecBlocking(const QString& Sql, QString* Error)
    {
        auto State = state_;
        bool Ok = false;
        QString Problem;
        QMetaObject::invokeMethod(context_, [State, Sql, &Ok, &Problem] {
            State->pending_.clear();
            if (!State->db_) { Ok = true; return; }
            Ok = State->db_->Exec(Sql, &Problem);
            const bool Open = sqlite3_get_autocommit(State->db_->Handle()) == 0;
            const auto Sink = State->sink_;
            State->Post([Sink, Open] { if (Sink.transaction_) Sink.transaction_(Open); });
        }, Qt::BlockingQueuedConnection);
        if (Error) *Error = Problem;
        return Ok;
    }

    void QueryRunner::Run(quint64 Batch, QVector<RunStatement> Statements, int Limit, Options Mode)
    {
        auto State = state_;
        State->busy_.fetch_add(1);
        QMetaObject::invokeMethod(context_, [State, Batch, Statements = std::move(Statements), Limit, Mode] {
            QElapsedTimer Total;
            Total.start();
            State->pending_.clear();
            State->batch_ = Batch;
            bool Interrupted = false, Wrote = false;
            if (!State->db_) {
                QString Error;
                State->db_ = State->open_(&Error);
                if (!State->db_) {
                    const auto Sink = State->sink_;
                    State->Post([Sink, Error] { if (Sink.message_) Sink.message_(0, Error.isEmpty() ? QObject::tr("데이터베이스를 열지 못했습니다.") : Error, -1, -1, 0, true); });
                    State->Post([Sink] { if (Sink.finished_) Sink.finished_(false, false, 0); });
                    State->busy_.fetch_sub(1);
                    return;
                }
                std::lock_guard Lock(State->mutex_);
                State->live_ = State->db_.get();
            }
            auto& Db = *State->db_;
            int ResultIndex = 0;
            for (int Index = 0; Index < Statements.size(); ++Index) {
                const auto& Each = Statements[Index];
                const auto Sink = State->sink_;
                State->Post([Sink, Index] { if (Sink.started_) Sink.started_(Index); });
                QElapsedTimer Timer;
                Timer.start();
                auto Query = std::make_shared<Statement>(Db, Each.sql_);
                if (!Query->Valid()) {
                    const int Offset = Db.ErrorOffset();
                    const auto Error = Query->Error().isEmpty() ? Db.ErrorMessage() : Query->Error();
                    const int Position = Offset >= 0 ? Each.start_ + Offset : -1;
                    Interrupted = Db.ErrorCode() == SQLITE_INTERRUPT;
                    State->Post([Sink, Index, Error, Position] { if (Sink.message_) Sink.message_(Index, Error, Position, -1, 0, true); });
                    break;
                }
                const bool ReadOnly = Query->ReadOnly();
                if (!ReadOnly && !Mode.allowWrites_) {
                    const auto Error = QObject::tr("읽기 전용으로 열었습니다 — 쓰기 문은 실행하지 않습니다.");
                    State->Post([Sink, Index, Error] { if (Sink.message_) Sink.message_(Index, Error, -1, -1, 0, true); });
                    break;
                }
                if (!ReadOnly && Mode.manual_ && sqlite3_get_autocommit(Db.Handle()) != 0 && !NonTransactional(Each.sql_)) {
                    // 수동 커밋: 첫 쓰기 문 앞에서 트랜잭션을 연다(Ctrl+S 가 커밋, ‘롤백’ 이 버린다).
                    QString Error;
                    if (!Db.Exec(QStringLiteral("BEGIN"), &Error)) {
                        State->Post([Sink, Index, Error] { if (Sink.message_) Sink.message_(Index, Error, -1, -1, 0, true); });
                        break;
                    }
                }
                if (Query->ColumnCount() > 0) {
                    QList<QueryModel::Column> Columns;
                    for (int Column = 0; Column < Query->ColumnCount(); ++Column)
                        Columns.append({Query->ColumnName(Column), Query->DeclaredType(Column), Query->TableName(Column)});
                    bool More = false;
                    QString Error;
                    auto Rows = State->Fetch(*Query, Limit, More, Error);
                    const qint64 Ms = Timer.elapsed();
                    const int Result = ResultIndex++;
                    State->Post([Sink, Index, Result, Columns, Rows = std::move(Rows), More, Ms]() mutable {
                        if (Sink.result_) Sink.result_(Index, Result, Columns, std::move(Rows), More, Ms);
                    });
                    if (!Error.isEmpty()) {
                        const int Offset = Db.ErrorOffset();
                        const int Position = Offset >= 0 ? Each.start_ + Offset : -1;
                        Interrupted = Db.ErrorCode() == SQLITE_INTERRUPT;
                        State->Post([Sink, Index, Error, Position] { if (Sink.message_) Sink.message_(Index, Error, Position, -1, 0, true); });
                        break;
                    }
                    if (More) State->pending_.insert(Result, Query);
                    if (!ReadOnly) Wrote = true;
                    State->Post([Sink, Index, Ms, ReadOnly] { if (Sink.message_) Sink.message_(Index, {}, -1, -1, Ms, ReadOnly); });
                } else {
                    int Rc = Query->Step();
                    while (Rc == SQLITE_ROW) Rc = Query->Step();
                    const qint64 Ms = Timer.elapsed();
                    if (Rc != SQLITE_DONE) {
                        const auto Error = Query->Error();
                        const int Offset = Db.ErrorOffset();
                        const int Position = Offset >= 0 ? Each.start_ + Offset : -1;
                        Interrupted = Rc == SQLITE_INTERRUPT || Db.ErrorCode() == SQLITE_INTERRUPT;
                        State->Post([Sink, Index, Error, Position] { if (Sink.message_) Sink.message_(Index, Error, Position, -1, 0, true); });
                        break;
                    }
                    const qint64 Changes = ReadOnly ? -1 : qint64(sqlite3_changes64(Db.Handle()));
                    if (!ReadOnly) Wrote = true;
                    State->Post([Sink, Index, Changes, Ms, ReadOnly] { if (Sink.message_) Sink.message_(Index, {}, -1, Changes, Ms, ReadOnly); });
                }
            }
            const qint64 Ms = Total.elapsed();
            const auto Sink = State->sink_;
            const bool Open = sqlite3_get_autocommit(Db.Handle()) == 0;
            State->Post([Sink, Open] { if (Sink.transaction_) Sink.transaction_(Open); });
            State->Post([Sink, Interrupted, Wrote, Ms] { if (Sink.finished_) Sink.finished_(Interrupted, Wrote, Ms); });
            State->busy_.fetch_sub(1);
        });
    }

    void QueryRunner::FetchMore(quint64 Batch, int Result, int Count)
    {
        auto State = state_;
        State->busy_.fetch_add(1);
        QMetaObject::invokeMethod(context_, [State, Batch, Result, Count] {
            const auto Sink = State->sink_;
            auto Query = State->batch_ == Batch ? State->pending_.value(Result) : nullptr;
            if (!Query) {
                State->Post([Sink, Batch, Result] { if (Sink.rows_) Sink.rows_(Batch, Result, {}, false); });
                State->busy_.fetch_sub(1);
                return;
            }
            bool More = false;
            QString Error;
            auto Rows = State->Fetch(*Query, Count, More, Error);
            if (!More) State->pending_.remove(Result);
            State->Post([Sink, Batch, Result, Rows = std::move(Rows), More]() mutable { if (Sink.rows_) Sink.rows_(Batch, Result, std::move(Rows), More); });
            if (!Error.isEmpty()) State->Post([Sink, Error] { if (Sink.message_) Sink.message_(-1, Error, -1, -1, 0, true); });
            State->busy_.fetch_sub(1);
        });
    }
}
