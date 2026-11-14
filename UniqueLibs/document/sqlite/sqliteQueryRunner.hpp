#pragma once

/// 고유 라이브러리
#include "sqliteResultModel.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <functional>
#include <memory>
#include <mutex>
#include <QObject>
#include <QPointer>
#include <QVector>

class QThread;

namespace hc::sqlite
{
    class Connection;

    // 실행할 문 하나. start_ 는 편집기 글의 UTF-8 바이트 위치(오류 위치를 편집기 위치로 바꾼다).
    struct RunStatement
    {
        int start_ = 0;
        QString sql_;
    };

    // 실행기가 GUI 로 넘기는 일. 모두 GUI 스레드에서 불린다(받는 쪽 객체가 사라지면 Qt 가 버린다).
    struct RunSink
    {
        std::function<void(int Index)> started_;
        std::function<void(int Index, int Result, QList<QueryModel::Column> Columns, QVector<QVector<QVariant>> Rows, bool More, qint64 Ms)> result_;
        std::function<void(quint64 Batch, int Result, QVector<QVector<QVariant>> Rows, bool More)> rows_;
        // Error 가 비면 성공. Offset 은 편집기 위치(모르면 -1). Changes 는 쓰기 문의 바뀐 행 수(읽기 문이면 -1).
        std::function<void(int Index, QString Error, int Offset, qint64 Changes, qint64 Ms, bool ReadOnly)> message_;
        std::function<void(bool Interrupted, bool Wrote, qint64 Ms)> finished_;
        // 실행이 끝난 뒤 연결에 열린 트랜잭션이 있는가(수동 커밋 · 사용자가 친 BEGIN).
        std::function<void(bool Open)> transaction_;
    };

    // 쿼리 탭 하나의 실행기(설계 5.3): 자기 스레드와 자기 연결. 결과는 행 제한만큼 보내고, 남은 행은 문을 열어 둔 채
    // FetchMore 를 기다린다(다음 실행 · Release 에서 닫는다). 중지는 sqlite3_interrupt(어느 스레드에서나).
    class QueryRunner final
    {
    public:
        using Opener = std::function<std::unique_ptr<Connection>(QString*)>;
        QueryRunner(Opener OpenConnection, QObject* Receiver, RunSink Sink);
        ~QueryRunner();
        QueryRunner(const QueryRunner&) = delete;
        QueryRunner& operator=(const QueryRunner&) = delete;

        // 실행 방식(설계 6.3). Manual: 첫 쓰기 문 앞에서 BEGIN(트랜잭션 안에서 못 도는 VACUUM · ATTACH · DETACH 는 빼고).
        // AllowWrites 가 거짓이면(읽기 전용으로 연 문서) 쓰기 문은 실행 전에 막는다(sqlite3_stmt_readonly).
        struct Options
        {
            bool manual_ = false;
            bool allowWrites_ = true;
        };
        void Run(quint64 Batch, QVector<RunStatement> Statements, int Limit, Options Mode = {});
        // 실행기 스레드에서 Sql 을 실행하고 끝날 때까지 기다린다(닫을 때의 COMMIT · ROLLBACK). 실패하면 거짓과 Error.
        bool ExecBlocking(const QString& Sql, QString* Error);
        // 트랜잭션 안에서 실행할 수 없는 문인가.
        static bool NonTransactional(const QString& Sql);
        void FetchMore(quint64 Batch, int Result, int Count);
        void Interrupt();
        // 연결을 바로 닫는다(탭을 닫을 때 · 문서를 닫을 때). 실행 중이면 중단하고 끝날 때까지 기다린다.
        void Release();
        bool Busy() const;
    private:
        struct State;
        std::shared_ptr<State> state_;
        QThread* thread_ = nullptr;
        QObject* context_ = nullptr;
    };
}
