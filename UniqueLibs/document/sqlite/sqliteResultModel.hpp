#pragma once

/// 고유 라이브러리
#include "sqliteSchema.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <functional>
#include <memory>
#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QSet>
#include <QVariant>
#include <QVector>

namespace hc::sqlite
{
    class Connection;
    class WorkerJob;

    // 선언 형식으로 정한 열 친화도(SQLite 3.1 규칙).
    enum class Affinity { Integer, Real, Numeric, Text, Blob };
    Affinity AffinityOf(const QString& DeclaredType);
    // 저장 형식(SQLITE_INTEGER …)이 선언과 어긋나는가 — 셀에 삼각형 표시(설계 6.1).
    bool StorageMismatch(Affinity Column, int StorageType);
    // 그리드에 보일 한 줄 글(긴 글 · 줄바꿈 정리). BLOB 은 크기 + 앞 바이트.
    QString DisplayText(const QVariant& Value);
    QString BlobSummary(const QByteArray& Blob);
    // 셀 하나의 역할 값(테이블 · 쿼리 결과 공용). RowIdRole 은 모델이 따로 낸다.
    QVariant CellData(const QVariant& Value, int Role, Affinity Column, const QString& DeclaredType);
    // 편집기에서 받은 값을 열 친화도에 맞춘다(INTEGER 열의 "12" → 12). 무효 QVariant 는 NULL.
    QVariant CoerceValue(const QVariant& Value, Affinity Column);

    // 셀 역할. 그리기는 델리게이트(sqliteCellDelegate)가 이 값을 보고 한다(설계 6.4).
    enum CellRole
    {
        ValueRole = Qt::UserRole + 1,   // 원래 값(QVariant, NULL 은 무효).
        NullRole,                       // bool
        StorageRole,                    // int — SQLITE_INTEGER · FLOAT · TEXT · BLOB · NULL
        DeclaredTypeRole,               // QString — 열의 선언 형식
        MismatchRole,                   // bool — 선언과 저장 형식이 어긋남
        NumericRole,                    // bool — 오른쪽 맞춤
        RowIdRole,                      // qint64 — rowid 테이블일 때
        PendingRole,                    // int — TableModel::PendingState(보류 편집, 설계 6.2)
        ConflictRole,                   // bool — 커밋 때 원래 값이 바뀌어 있던 칸
        UnsetRole                       // bool — 새 행에서 아직 넣지 않은 칸(rowid 별칭 ‘자동’ · 기본값)
    };

    // 테이블 · 뷰 한 개를 보여 주는 모델. rowid 테이블은 rowid 목록(keyset)을 작업 스레드에서 만들고 보이는 페이지만
    // rowid 로 읽는다. 뷰 · WITHOUT ROWID · 가상 테이블은 행 수를 센 뒤 LIMIT/OFFSET 페이지로 읽는다.
    // 정렬(sort)은 ORDER BY, 필터는 WHERE 로 바꿔 다시 읽는다(메모리 정렬 없음, 설계 6.4).
    class TableModel final : public QAbstractTableModel
    {
        Q_OBJECT
    public:
        using Opener = std::function<std::unique_ptr<Connection>(QString*)>;
        static constexpr int PageRows = 256;
        static constexpr int KeptPages = 64;

        // Reader 는 GUI 스레드에서 페이지를 읽는 연결(문서가 소유), OpenWorker 는 작업 스레드용 새 연결을 연다.
        TableModel(Connection& Reader, Opener OpenWorker, QObject* Parent = nullptr);
        ~TableModel() override;

        void SetSource(const ObjectInfo& Object);
        const ObjectInfo& Source() const { return source_; }
        // WHERE · ORDER BY 막대의 글(앞의 키워드 없이). 빈 글이면 없앤다. 다시 읽기는 Reload 가 한다.
        void SetFilter(const QString& Where, const QString& OrderBy);
        const QString& Where() const { return where_; }
        const QString& OrderBy() const { return orderBy_; }
        void Reload();
        void Cancel();
        bool Loading() const { return loading_; }
        bool KeysetMode() const { return keyset_; }
        qint64 ElapsedMs() const { return elapsedMs_; }
        const QString& LastError() const { return error_; }
        // 막대 아래에 보이는 실제 SQL 한 줄(읽기 쿼리).
        QString GeneratedSql(int Limit = 1000) const;

        int rowCount(const QModelIndex& Parent = {}) const override;
        int columnCount(const QModelIndex& Parent = {}) const override;
        QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override;
        QVariant headerData(int Section, Qt::Orientation Orientation, int Role = Qt::DisplayRole) const override;
        void sort(int Column, Qt::SortOrder Order = Qt::AscendingOrder) override;

        // 보류 편집(설계 6.2). Commit 전까지 파일에 쓰지 않고 잠금도 잡지 않는다. rowid(WITHOUT ROWID 는 기본 키)로 행을
        // 묶어 정렬 · 필터를 바꿔도 유지한다. 뷰 · 가상 테이블 · 읽기 전용 연결은 편집하지 않는다.
        enum class PendingState { None, Modified, Inserted, Deleted };
        struct PendingSummary
        {
            int modified_ = 0, inserted_ = 0, deleted_ = 0;
            int Total() const { return modified_ + inserted_ + deleted_; }
        };
        void SetReadOnly(bool ReadOnly);
        bool Editable() const;
        PendingSummary Pending() const;
        bool HasPending() const { return !edits_.isEmpty() || !inserted_.isEmpty(); }
        Qt::ItemFlags flags(const QModelIndex& Index) const override;
        bool setData(const QModelIndex& Index, const QVariant& Value, int Role = Qt::EditRole) override;
        // 새 행(맨 아래). CopyFrom 이 있으면 그 행의 값(rowid 별칭 · 기본 키는 빼고)으로 채운다. 새 행 번호, 못 넣으면 -1.
        int InsertRow(int CopyFrom = -1);
        // 고른 행을 지우기로 표시한다(이미 표시된 행은 되살린다). 새 행은 바로 뺀다.
        void ToggleDelete(QList<int> Rows);
        void RevertAll();
        // 변경을 SQL 로(‘변경 SQL 보기’). 커밋이 실행할 문과 같은 모양에 값을 리터럴로 넣는다.
        QStringList PendingStatements() const;
        // BEGIN IMMEDIATE → UPDATE … WHERE 키 AND 원래 값 일치 · DELETE · INSERT → COMMIT. 영향 행이 0 이면 충돌로 보고
        // ROLLBACK 하고 그 칸을 ConflictRole 로 표시한다. 성공하면 보류를 비우고 다시 읽는다.
        bool Commit(QString* Error);
        int ConflictCount() const { return int(conflicts_.size()); }
    signals:
        void PendingChanged();
        void LoadingChanged(bool Loading);
        void Loaded(qint64 Rows, qint64 ElapsedMs);
        void Failed(const QString& Error);
        // 머리글 정렬로 ORDER BY 가 바뀌었다(막대를 맞춘다).
        void OrderChanged(const QString& OrderBy);
    private:
        struct Row { qint64 rowid_ = 0; QVector<QVariant> values_; };
        struct Page { QVector<Row> rows_; };
        QString selectList() const;
        QString fromClause() const;
        const Row* row(int Index) const;
        bool fetchPage(int PageIndex) const;
        QString rowKey(const Row& Item) const;
        bool rowidAlias(int Column) const;
        QString tableText() const;
        struct Edit { QVector<QVariant> original_, keyValues_; QHash<int, QVariant> changed_; qint64 rowid_ = 0; bool deleted_ = false; };
        struct Inserted { QHash<int, QVariant> values_; };
        QString keyCondition(const Edit& Item, bool Literal) const;
        void finishLoad(quint64 Generation, QVector<qint64> Keys, qint64 Count, QString Error);

        Connection& reader_;
        Opener openWorker_;
        ObjectInfo source_;
        QVector<Affinity> affinity_;
        QString where_, orderBy_;
        bool keyset_ = true, loading_ = false;
        quint64 generation_ = 0;
        std::shared_ptr<WorkerJob> job_;
        // 취소했지만 아직 연결을 닫는 중일 수 있는 작업. 모델이 사라질 때 끝나기를 기다린다.
        QList<std::shared_ptr<WorkerJob>> retired_;
        QVector<qint64> keys_;
        qint64 rows_ = 0, elapsedMs_ = 0;
        QString error_;
        QElapsedTimer timer_;
        mutable QHash<int, Page> pages_;
        mutable QList<int> recent_;
        mutable QString pageError_;
        // 보류 편집
        bool readOnly_ = false;
        QVector<int> keyColumns_;                   // WITHOUT ROWID: 기본 키 열(순서대로)
        QHash<QString, Edit> edits_;
        QList<Inserted> inserted_;
        QSet<QString> conflicts_;                   // "행 키/열"
    };

    // 쿼리 결과 하나(설계 5.3). 행 제한만큼 받고, 끝에 닿으면 실행기에 더 달라고 한다(canFetchMore · fetchMore).
    // 읽기 전용이다. 편집은 테이블 탭(보류 편집)에서 한다.
    class QueryModel final : public QAbstractTableModel
    {
        Q_OBJECT
    public:
        struct Column { QString name_, declaredType_, table_; };
        explicit QueryModel(QObject* Parent = nullptr);
        void SetColumns(const QList<Column>& Columns);
        const QList<Column>& Columns() const { return columns_; }
        const QVector<QVector<QVariant>>& Rows() const { return rows_; }
        void AppendRows(QVector<QVector<QVariant>> Rows, bool More);
        // fetchMore 가 부르는 요청(쿼리 탭이 실행기에 넘긴다).
        void SetFetcher(std::function<void()> Fetch) { fetch_ = std::move(Fetch); }
        void StopFetching() { more_ = false; fetching_ = false; fetch_ = {}; }
        bool HasMore() const { return more_; }

        int rowCount(const QModelIndex& Parent = {}) const override;
        int columnCount(const QModelIndex& Parent = {}) const override;
        QVariant data(const QModelIndex& Index, int Role = Qt::DisplayRole) const override;
        QVariant headerData(int Section, Qt::Orientation Orientation, int Role = Qt::DisplayRole) const override;
        bool canFetchMore(const QModelIndex& Parent) const override;
        void fetchMore(const QModelIndex& Parent) override;
    private:
        QList<Column> columns_;
        QVector<Affinity> affinity_;
        QVector<QVector<QVariant>> rows_;
        std::function<void()> fetch_;
        bool more_ = false, fetching_ = false;
    };
}
