#include "stdafx.h"
#include "sqliteSchema.hpp"

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
#include <QHash>
#include <algorithm>

namespace hc::sqlite
{
    namespace
    {
        ObjectType TypeOf(const QString& Type)
        {
            if (Type == QLatin1String("view")) return ObjectType::View;
            if (Type == QLatin1String("index")) return ObjectType::Index;
            if (Type == QLatin1String("trigger")) return ObjectType::Trigger;
            return ObjectType::Table;
        }

        bool ReadColumns(Connection& Db, ObjectInfo& Object)
        {
            Statement Columns(Db, QStringLiteral("SELECT name, type, \"notnull\", dflt_value, pk, hidden FROM pragma_table_xinfo(?1, ?2)"));
            if (!Columns.Valid()) return false;
            Columns.Bind(1, Object.name_);
            Columns.Bind(2, Object.database_);
            while (Columns.Step() == SQLITE_ROW) {
                ColumnInfo Info;
                Info.name_ = Columns.Value(0).toString();
                Info.type_ = Columns.Value(1).toString();
                Info.notNull_ = Columns.Value(2).toInt() != 0;
                Info.defaultValue_ = Columns.Value(3).toString();
                Info.primaryKey_ = Columns.Value(4).toInt();
                // hidden: 1 = 가상 테이블의 숨은 열, 2 · 3 = 생성 열(보인다).
                Info.hidden_ = Columns.Value(5).toInt() == 1;
                Info.generated_ = Columns.Value(5).toInt() >= 2;
                Object.columns_.append(Info);
            }
            if (Object.type_ != ObjectType::Table) return true;
            Statement Keys(Db, QStringLiteral("SELECT \"from\", \"table\", \"to\" FROM pragma_foreign_key_list(?1, ?2)"));
            if (!Keys.Valid()) return true;
            Keys.Bind(1, Object.name_);
            Keys.Bind(2, Object.database_);
            while (Keys.Step() == SQLITE_ROW) {
                const auto From = Keys.Value(0).toString();
                for (auto& Column : Object.columns_) {
                    if (Column.name_.compare(From, Qt::CaseInsensitive) != 0) continue;
                    Column.foreignTable_ = Keys.Value(1).toString();
                    Column.foreignColumn_ = Keys.Value(2).toString();
                }
            }
            return true;
        }
    }

    QStringList ObjectInfo::PrimaryKey() const
    {
        QList<const ColumnInfo*> Keys;
        for (const auto& Column : columns_) if (Column.primaryKey_ > 0) Keys.append(&Column);
        std::sort(Keys.begin(), Keys.end(), [](const ColumnInfo* A, const ColumnInfo* B) { return A->primaryKey_ < B->primaryKey_; });
        QStringList Result;
        for (const auto* Column : Keys) Result.append(Column->name_);
        return Result;
    }

    QList<const ObjectInfo*> DatabaseInfo::Objects(ObjectType Type, bool IncludeSystem) const
    {
        QList<const ObjectInfo*> Result;
        for (const auto& Object : objects_) if (Object.type_ == Type && (IncludeSystem || !Object.system_)) Result.append(&Object);
        return Result;
    }

    const ObjectInfo* DatabaseInfo::Find(const QString& Name) const
    {
        for (const auto& Object : objects_) if (Object.name_.compare(Name, Qt::CaseInsensitive) == 0) return &Object;
        return nullptr;
    }

    const DatabaseInfo* SchemaInfo::Database(const QString& Name) const
    {
        for (const auto& Database : databases_) if (Database.name_.compare(Name, Qt::CaseInsensitive) == 0) return &Database;
        return nullptr;
    }

    const ObjectInfo* SchemaInfo::Find(const QString& Database, const QString& Name) const
    {
        const auto* Found = this->Database(Database);
        return Found ? Found->Find(Name) : nullptr;
    }

    bool ReadSchema(Connection& Db, SchemaInfo& Out, QString* Error)
    {
        Out = {};
        Out.schemaVersion_ = SchemaVersion(Db);
        Statement List(Db, QStringLiteral("SELECT name, file FROM pragma_database_list ORDER BY seq"));
        if (!List.Valid()) { if (Error) *Error = List.Error(); return false; }
        while (List.Step() == SQLITE_ROW) Out.databases_.append(DatabaseInfo{List.Value(0).toString(), List.Value(1).toString(), {}});
        for (auto& Database : Out.databases_) {
            // table_list(3.37+): 가상 테이블 · WITHOUT ROWID · STRICT 를 알려 준다.
            QHash<QString, std::pair<bool, bool>> Flags;
            QHash<QString, bool> Virtual;
            Statement Tables(Db, QStringLiteral("SELECT name, type, wr, strict FROM pragma_table_list WHERE schema = ?1"));
            if (Tables.Valid()) {
                Tables.Bind(1, Database.name_);
                while (Tables.Step() == SQLITE_ROW) {
                    const auto Name = Tables.Value(0).toString().toLower();
                    Flags.insert(Name, {Tables.Value(2).toInt() != 0, Tables.Value(3).toInt() != 0});
                    Virtual.insert(Name, Tables.Value(1).toString() == QLatin1String("virtual"));
                }
            }
            const auto Master = Database.name_.compare(QLatin1String("temp"), Qt::CaseInsensitive) == 0
                ? QStringLiteral("sqlite_temp_schema") : QStringLiteral("sqlite_schema");
            Statement Objects(Db, QStringLiteral("SELECT type, name, tbl_name, sql FROM %1.%2 ORDER BY name COLLATE NOCASE")
                .arg(QuoteIdentifier(Database.name_), Master));
            if (!Objects.Valid()) { if (Error) *Error = Objects.Error(); return false; }
            while (Objects.Step() == SQLITE_ROW) {
                ObjectInfo Object;
                Object.database_ = Database.name_;
                Object.type_ = TypeOf(Objects.Value(0).toString());
                Object.name_ = Objects.Value(1).toString();
                Object.table_ = Objects.Value(2).toString();
                Object.sql_ = Objects.Value(3).toString();
                Object.system_ = Object.name_.startsWith(QLatin1String("sqlite_"), Qt::CaseInsensitive);
                const auto Key = Object.name_.toLower();
                if (Flags.contains(Key)) { Object.withoutRowid_ = Flags[Key].first; Object.strict_ = Flags[Key].second; }
                Object.virtual_ = Virtual.value(Key, false);
                Database.objects_.append(Object);
            }
            for (auto& Object : Database.objects_)
                if (Object.type_ == ObjectType::Table || Object.type_ == ObjectType::View) ReadColumns(Db, Object);
        }
        return true;
    }

    int SchemaVersion(Connection& Db)
    {
        Statement Version(Db, QStringLiteral("PRAGMA main.schema_version"));
        return Version.Valid() && Version.Step() == SQLITE_ROW ? Version.Value(0).toInt() : 0;
    }

    qint64 CountRows(Connection& Db, const QString& Database, const QString& Table)
    {
        Statement Count(Db, QStringLiteral("SELECT count(*) FROM %1").arg(Qualified(Database, Table)));
        return Count.Valid() && Count.Step() == SQLITE_ROW ? Count.Value(0).toLongLong() : -1;
    }
}
