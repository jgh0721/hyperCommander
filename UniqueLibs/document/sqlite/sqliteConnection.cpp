#include "stdafx.h"
#include "sqliteConnection.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteApi.hpp"
#include "sqliteRuntime.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDir>
#include <QUrl>

namespace hc::sqlite
{
    namespace
    {
        QString Message(sqlite3* Db) { return Db ? QString::fromUtf8(sqlite3_errmsg(Db)) : QString(); }
    }

    std::unique_ptr<Connection> Connection::Open(const QString& Path, const OpenOptions& Options, QString* Error)
    {
        if (!Runtime::Load()) {
            if (Error) *Error = QObject::tr("SQLite 라이브러리를 불러오지 못했습니다.");
            return {};
        }
        std::unique_ptr<Connection> Result(new Connection);
        Result->path_ = Path;
        Result->options_ = Options;
        int Flags = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
        QByteArray Target;
        if (Options.immutable_) {
            // 쓸 수 없는 폴더의 WAL 파일: -shm 을 만들 수 없어 보통으로는 열리지 않는다. 키는 URI 에 넣지 않는다.
            Target = QUrl::fromLocalFile(QDir::cleanPath(Path)).toEncoded() + "?mode=ro&immutable=1";
            Flags |= SQLITE_OPEN_READONLY | SQLITE_OPEN_URI;
        } else {
            Target = QDir::toNativeSeparators(Path).toUtf8();
            Flags |= Options.readOnly_ ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
            if (Options.create_ && !Options.readOnly_) Flags |= SQLITE_OPEN_CREATE;
        }
        const int Rc = sqlite3_open_v2(Target.constData(), &Result->db_, Flags, nullptr);
        if (Rc != SQLITE_OK) {
            if (Error) *Error = Result->db_ ? Message(Result->db_) : QString::fromUtf8(sqlite3_errstr(Rc));
            return {};
        }
        sqlite3_extended_result_codes(Result->db_, 1);
        sqlite3_busy_timeout(Result->db_, Options.busyTimeoutMs_);
        if (Options.readOnly_ || Options.immutable_) Result->Exec(QStringLiteral("PRAGMA query_only = 1"));
        return Result;
    }

    Connection::~Connection()
    {
        if (db_) sqlite3_close_v2(db_);
    }

    bool Connection::Exec(const QString& Sql, QString* Error)
    {
        char* Raw = nullptr;
        const int Rc = sqlite3_exec(db_, Sql.toUtf8().constData(), nullptr, nullptr, &Raw);
        if (Rc != SQLITE_OK && Error) *Error = Raw ? QString::fromUtf8(Raw) : Message(db_);
        if (Raw) sqlite3_free(Raw);
        return Rc == SQLITE_OK;
    }

    QString Connection::ErrorMessage() const { return Message(db_); }
    int Connection::ErrorOffset() const { return db_ ? sqlite3_error_offset(db_) : -1; }
    int Connection::ErrorCode() const { return db_ ? sqlite3_extended_errcode(db_) : SQLITE_MISUSE; }
    void Connection::Interrupt() { if (db_) sqlite3_interrupt(db_); }

    int Connection::Rekey(const QByteArray& Key, KeyFormat Format)
    {
        if (Runtime::See() != SeeState::Available) return SQLITE_MISUSE;
        if (Key.isEmpty()) return sqlite3_rekey_v2(db_, "main", nullptr, 0);
        switch (Format) {
        case KeyFormat::Raw:
            return sqlite3_rekey_v2(db_, "main", Key.constData(), int(Key.size()));
        case KeyFormat::Hex: {
            const auto Trimmed = Key.trimmed();
            if (Trimmed.isEmpty() || Trimmed.size() % 2 != 0) return SQLITE_MISUSE;
            for (const char Ch : Trimmed) if (!std::isxdigit(static_cast<unsigned char>(Ch))) return SQLITE_MISUSE;
            auto Bytes = QByteArray::fromHex(Trimmed);
            const int Rc = sqlite3_rekey_v2(db_, "main", Bytes.constData(), int(Bytes.size()));
            Bytes.fill('\0');
            return Rc;
        }
        case KeyFormat::Text: {
            auto Sql = QByteArray("PRAGMA main.textrekey = ") + QuoteLiteral(QString::fromUtf8(Key)).toUtf8();
            const int Rc = sqlite3_exec(db_, Sql.constData(), nullptr, nullptr, nullptr);
            Sql.fill('\0');
            return Rc;
        }
        }
        return SQLITE_MISUSE;
    }

    int Connection::ApplyKey(const QByteArray& Key, KeyFormat Format)
    {
        if (Runtime::See() != SeeState::Available) return SQLITE_MISUSE;
        switch (Format) {
        case KeyFormat::Raw:
            return sqlite3_key_v2(db_, "main", Key.constData(), int(Key.size()));
        case KeyFormat::Hex: {
            const auto Trimmed = Key.trimmed();
            if (Trimmed.isEmpty() || Trimmed.size() % 2 != 0) return SQLITE_MISUSE;
            for (const char Ch : Trimmed) if (!std::isxdigit(static_cast<unsigned char>(Ch))) return SQLITE_MISUSE;
            auto Bytes = QByteArray::fromHex(Trimmed);
            const int Rc = sqlite3_key_v2(db_, "main", Bytes.constData(), int(Bytes.size()));
            Bytes.fill('\0');
            return Rc;
        }
        case KeyFormat::Text: {
            // textkey 는 C API 가 없어 PRAGMA 로 넣는다. 이 SQL 은 기록 · 로그에 남기지 않는다.
            auto Sql = QByteArray("PRAGMA main.textkey = ") + QuoteLiteral(QString::fromUtf8(Key)).toUtf8();
            const int Rc = sqlite3_exec(db_, Sql.constData(), nullptr, nullptr, nullptr);
            Sql.fill('\0');
            return Rc;
        }
        }
        return SQLITE_MISUSE;
    }

    int Connection::Probe(QString* Error)
    {
        Statement Query(*this, QStringLiteral("SELECT count(*) FROM main.sqlite_schema"));
        if (!Query.Valid()) {
            if (Error) *Error = Query.Error();
            return ErrorCode();
        }
        const int Rc = Query.Step();
        if (Rc != SQLITE_ROW && Error) *Error = Query.Error();
        return Rc == SQLITE_ROW ? SQLITE_OK : Rc;
    }

    Statement::Statement(Connection& Owner, const QString& Sql)
        : owner_(Owner)
    {
        const auto Utf8 = Sql.toUtf8();
        const char* Tail = nullptr;
        const int Rc = sqlite3_prepare_v3(Owner.Handle(), Utf8.constData(), int(Utf8.size()), 0, &stmt_, &Tail);
        if (Rc != SQLITE_OK) {
            error_ = Owner.ErrorMessage();
            if (stmt_) { sqlite3_finalize(stmt_); stmt_ = nullptr; }
        }
        if (Tail && Tail < Utf8.constData() + Utf8.size()) tail_ = QString::fromUtf8(Tail, Utf8.constData() + Utf8.size() - Tail);
    }

    Statement::~Statement()
    {
        if (stmt_) sqlite3_finalize(stmt_);
    }

    bool Statement::Bind(int Index, const QVariant& Value)
    {
        if (!stmt_) return false;
        int Rc = SQLITE_OK;
        if (!Value.isValid() || Value.isNull()) Rc = sqlite3_bind_null(stmt_, Index);
        else switch (Value.typeId()) {
        case QMetaType::LongLong: case QMetaType::Int: case QMetaType::UInt: case QMetaType::Bool: case QMetaType::Long:
            Rc = sqlite3_bind_int64(stmt_, Index, Value.toLongLong()); break;
        case QMetaType::ULongLong: Rc = sqlite3_bind_int64(stmt_, Index, qint64(Value.toULongLong())); break;
        case QMetaType::Double: case QMetaType::Float: Rc = sqlite3_bind_double(stmt_, Index, Value.toDouble()); break;
        case QMetaType::QByteArray: {
            const auto Bytes = Value.toByteArray();
            Rc = sqlite3_bind_blob64(stmt_, Index, Bytes.constData(), sqlite3_uint64(Bytes.size()), SQLITE_TRANSIENT);
            break;
        }
        default: {
            const auto Text = Value.toString().toUtf8();
            Rc = sqlite3_bind_text64(stmt_, Index, Text.constData(), sqlite3_uint64(Text.size()), SQLITE_TRANSIENT, SQLITE_UTF8);
        }
        }
        if (Rc != SQLITE_OK) error_ = owner_.ErrorMessage();
        return Rc == SQLITE_OK;
    }

    int Statement::Step()
    {
        if (!stmt_) return SQLITE_MISUSE;
        const int Rc = sqlite3_step(stmt_);
        if (Rc != SQLITE_ROW && Rc != SQLITE_DONE) error_ = owner_.ErrorMessage();
        return Rc;
    }

    void Statement::Reset() { if (stmt_) { sqlite3_reset(stmt_); sqlite3_clear_bindings(stmt_); } }
    int Statement::ColumnCount() const { return stmt_ ? sqlite3_column_count(stmt_) : 0; }

    QString Statement::ColumnName(int Column) const
    {
        const char* Name = stmt_ ? sqlite3_column_name(stmt_, Column) : nullptr;
        return Name ? QString::fromUtf8(Name) : QString();
    }

    QString Statement::DeclaredType(int Column) const
    {
        const char* Type = stmt_ ? sqlite3_column_decltype(stmt_, Column) : nullptr;
        return Type ? QString::fromUtf8(Type) : QString();
    }

    QString Statement::TableName(int Column) const
    {
        const char* Name = stmt_ ? sqlite3_column_table_name(stmt_, Column) : nullptr;
        return Name ? QString::fromUtf8(Name) : QString();
    }

    QString Statement::OriginName(int Column) const
    {
        const char* Name = stmt_ ? sqlite3_column_origin_name(stmt_, Column) : nullptr;
        return Name ? QString::fromUtf8(Name) : QString();
    }

    int Statement::ColumnType(int Column) const { return stmt_ ? sqlite3_column_type(stmt_, Column) : SQLITE_NULL; }

    QVariant Statement::Value(int Column) const
    {
        switch (ColumnType(Column)) {
        case SQLITE_INTEGER: return QVariant(qint64(sqlite3_column_int64(stmt_, Column)));
        case SQLITE_FLOAT: return QVariant(sqlite3_column_double(stmt_, Column));
        case SQLITE_TEXT: {
            const auto* Text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, Column));
            return QVariant(QString::fromUtf8(Text, sqlite3_column_bytes(stmt_, Column)));
        }
        case SQLITE_BLOB: {
            const auto* Blob = static_cast<const char*>(sqlite3_column_blob(stmt_, Column));
            return QVariant(QByteArray(Blob, sqlite3_column_bytes(stmt_, Column)));
        }
        default: return {};
        }
    }

    bool Statement::ReadOnly() const { return stmt_ && sqlite3_stmt_readonly(stmt_) != 0; }

    QString QuoteIdentifier(const QString& Name)
    {
        auto Escaped = Name;
        Escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + Escaped + QLatin1Char('"');
    }

    QString Qualified(const QString& Schema, const QString& Name)
    {
        return Schema.isEmpty() ? QuoteIdentifier(Name) : QuoteIdentifier(Schema) + QLatin1Char('.') + QuoteIdentifier(Name);
    }

    QString IdentifierText(const QString& Name)
    {
        bool Plain = !Name.isEmpty() && !Name[0].isDigit();
        for (const auto Ch : Name) Plain &= (Ch >= QLatin1Char('a') && Ch <= QLatin1Char('z')) || (Ch >= QLatin1Char('A') && Ch <= QLatin1Char('Z'))
                                         || (Ch >= QLatin1Char('0') && Ch <= QLatin1Char('9')) || Ch == QLatin1Char('_');
        // 키워드인지 모르면(라이브러리 없음) 감싼다.
        if (Plain) {
            const auto Latin = Name.toLatin1();
            Plain = Runtime::Load() && sqlite3_keyword_check(Latin.constData(), int(Latin.size())) == 0;
        }
        return Plain ? Name : QuoteIdentifier(Name);
    }

    QString QuoteLiteral(const QString& Text)
    {
        auto Escaped = Text;
        Escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
        return QLatin1Char('\'') + Escaped + QLatin1Char('\'');
    }

    QString SqlLiteral(const QVariant& Value)
    {
        if (!Value.isValid() || Value.isNull()) return QStringLiteral("NULL");
        switch (Value.typeId()) {
        case QMetaType::LongLong: case QMetaType::Int: case QMetaType::ULongLong: return Value.toString();
        case QMetaType::Double: return QString::number(Value.toDouble(), 'g', 17);
        case QMetaType::QByteArray: return QStringLiteral("X'") + QString::fromLatin1(Value.toByteArray().toHex().toUpper()) + QLatin1Char('\'');
        default: return QuoteLiteral(Value.toString());
        }
    }
}
