#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <memory>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariant>

struct sqlite3;
struct sqlite3_stmt;

namespace hc::sqlite
{
    // 키 형식(설계 8.1). 같은 글자라도 형식이 다르면 다른 키다.
    enum class KeyFormat { Text, Raw, Hex };

    struct OpenOptions
    {
        bool readOnly_ = false;     // SQLITE_OPEN_READONLY + PRAGMA query_only = 1 (빠른 보기 · 읽기 전용으로 열기).
        bool immutable_ = false;    // 폴더에 쓸 수 없는 WAL 파일: immutable=1 로 연다(다른 프로그램의 변경을 못 봄).
        bool create_ = false;       // 없으면 만든다(새 데이터베이스 · 시험).
        int busyTimeoutMs_ = 2000;
    };

    // sqlite3 연결 하나. 한 번에 한 스레드만 쓴다(Interrupt 만 다른 스레드에서 불러도 된다).
    // 키는 URI 로 넘기지 않고 ApplyKey 로만 넣는다(로그 · 기록 유출 방지, 설계 8.3).
    class Connection final
    {
    public:
        static std::unique_ptr<Connection> Open(const QString& Path, const OpenOptions& Options, QString* Error);
        ~Connection();
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        sqlite3* Handle() const { return db_; }
        const QString& Path() const { return path_; }
        const OpenOptions& Options() const { return options_; }
        // 여러 문을 차례로 실행한다(결과 행은 버린다). 실패하면 Error 에 메시지.
        bool Exec(const QString& Sql, QString* Error = nullptr);
        QString ErrorMessage() const;
        // 마지막 오류의 SQL 안 바이트 위치(sqlite3_error_offset). 모르면 -1.
        int ErrorOffset() const;
        int ErrorCode() const;
        void Interrupt();
        // SEE 키를 넣는다. Runtime::See() 가 Available 일 때만 부른다. 성공해도 키가 맞는지는 첫 읽기에서 안다.
        int ApplyKey(const QByteArray& Key, KeyFormat Format);
        // 키 바꾸기(rekey). 빈 키는 암호화 해제, 평문 파일에 키를 주면 암호화(설계 8.1). 키는 SQL 기록에 남기지 않는다.
        int Rekey(const QByteArray& Key, KeyFormat Format);
        // 스키마를 한 번 읽어 키 · 파일이 맞는지 본다. SQLITE_NOTADB 면 키가 틀렸거나 SQLite 파일이 아니다.
        int Probe(QString* Error = nullptr);
    private:
        Connection() = default;
        sqlite3* db_ = nullptr;
        QString path_;
        OpenOptions options_;
    };

    // 준비된 문 하나(RAII). 값은 QVariant 로 내준다: NULL → 무효 QVariant, INTEGER → qint64, FLOAT → double,
    // TEXT → QString, BLOB → QByteArray.
    class Statement final
    {
    public:
        Statement(Connection& Owner, const QString& Sql);
        ~Statement();
        Statement(const Statement&) = delete;
        Statement& operator=(const Statement&) = delete;

        bool Valid() const { return stmt_ != nullptr; }
        const QString& Error() const { return error_; }
        // 준비한 SQL 중 이 문이 쓰지 않은 나머지(여러 문을 하나씩 나눌 때).
        const QString& Tail() const { return tail_; }
        sqlite3_stmt* Handle() const { return stmt_; }
        bool Bind(int Index, const QVariant& Value);
        // SQLITE_ROW · SQLITE_DONE · 오류 코드. 오류면 Error() 에 메시지.
        int Step();
        void Reset();
        int ColumnCount() const;
        QString ColumnName(int Column) const;
        QString DeclaredType(int Column) const;
        QString TableName(int Column) const;
        QString OriginName(int Column) const;
        int ColumnType(int Column) const;
        QVariant Value(int Column) const;
        bool ReadOnly() const;
    private:
        Connection& owner_;
        sqlite3_stmt* stmt_ = nullptr;
        QString error_, tail_;
    };

    // "이름" — 큰따옴표를 겹쳐 감싼다. 스키마 이름이 필요한 곳에는 Qualified 를 쓴다.
    QString QuoteIdentifier(const QString& Name);
    QString Qualified(const QString& Schema, const QString& Name);
    // 사람이 읽을 SQL(새 쿼리 탭에 SELECT, INSERT 문으로 복사)용: 영문 · 숫자 · _ 이름이고 키워드가 아니면 그대로, 아니면 "이름".
    QString IdentifierText(const QString& Name);
    // 'text' — 작은따옴표를 겹쳐 감싼다.
    QString QuoteLiteral(const QString& Text);
    // 표시 · 복사용 SQL 리터럴(NULL · 숫자 · 'text' · X'..').
    QString SqlLiteral(const QVariant& Value);
}
