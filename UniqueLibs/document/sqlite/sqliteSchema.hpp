#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QList>
#include <QString>
#include <QStringList>

namespace hc::sqlite
{
    class Connection;

    struct ColumnInfo
    {
        QString name_, type_, defaultValue_;
        bool notNull_ = false, hidden_ = false;
        bool generated_ = false;        // GENERATED ALWAYS AS(table_xinfo hidden 2 · 3). 값을 넣을 수 없다.
        int primaryKey_ = 0;            // pragma_table_xinfo 의 pk(1부터, 0 이면 키 아님).
        QString foreignTable_, foreignColumn_;
    };

    enum class ObjectType { Table, View, Index, Trigger };

    struct ObjectInfo
    {
        QString database_, name_, table_, sql_;     // table_: 인덱스 · 트리거가 붙은 테이블.
        ObjectType type_ = ObjectType::Table;
        bool withoutRowid_ = false, strict_ = false, virtual_ = false, system_ = false;
        QList<ColumnInfo> columns_;                 // 테이블 · 뷰만.
        bool HasRowid() const { return type_ == ObjectType::Table && !withoutRowid_ && !virtual_; }
        QStringList PrimaryKey() const;             // pk 순서대로.
    };

    struct DatabaseInfo
    {
        QString name_, file_;                       // main · temp · ATTACH 이름.
        QList<ObjectInfo> objects_;
        QList<const ObjectInfo*> Objects(ObjectType Type, bool IncludeSystem = false) const;
        const ObjectInfo* Find(const QString& Name) const;
    };

    struct SchemaInfo
    {
        QList<DatabaseInfo> databases_;
        int schemaVersion_ = 0;                     // PRAGMA schema_version(main). 바뀌면 다시 읽는다.
        const DatabaseInfo* Database(const QString& Name) const;
        const ObjectInfo* Find(const QString& Database, const QString& Name) const;
    };

    // 연결이 붙은 모든 데이터베이스(main · temp · ATTACH)의 개체와 열을 읽는다. 행 수는 읽지 않는다(CountRows).
    bool ReadSchema(Connection& Db, SchemaInfo& Out, QString* Error);
    int SchemaVersion(Connection& Db);
    // 행 수(count(*)). 큰 테이블은 오래 걸리므로 별도 연결 · 작업 스레드에서 부른다. 실패하면 -1.
    qint64 CountRows(Connection& Db, const QString& Database, const QString& Table);
}
