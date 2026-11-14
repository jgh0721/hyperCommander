#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QByteArray>
#include <QString>

namespace hc::sqlite
{
    // 파일 머리 100바이트로 SQLite 파일인지, SEE 로 암호화된 것으로 보이는지 가린다(설계 8.2).
    enum class FileKind
    {
        Plain,          // 0–15 바이트가 "SQLite format 3\0".
        Empty,          // 0 바이트 — SQLite 는 빈 데이터베이스로 연다.
        SeeEncrypted,   // 0–15 는 암호화되어 있고, 평문으로 남는 16–23 바이트가 SQLite 규칙에 맞는다.
        NotSqlite,
        Unreadable      // 열거나 읽지 못했다(권한 · 잠금 · 없음).
    };

    struct HeaderInfo
    {
        FileKind kind_ = FileKind::NotSqlite;
        int pageSize_ = 0;          // 16–17 바이트(1 이면 65536).
        int reservedBytes_ = 0;     // 20 바이트. SEE 는 여기에 nonce 를 둔다.
        bool wal_ = false;          // 18 · 19 바이트가 2 — WAL 모드 파일.
        qint64 fileSize_ = 0;
        QString error_;             // Unreadable 일 때 이유.
    };

    // Header 는 파일 앞부분(100바이트면 충분). FileSize 는 페이지 크기와 맞는지 보는 데 쓴다(모르면 -1).
    HeaderInfo InspectHeader(const QByteArray& Header, qint64 FileSize = -1);
    HeaderInfo InspectFile(const QString& Path);
    // 확장자가 없는 파일을 SQLite 문서로 열지 정할 때 쓴다. 평문 머리만 본다(암호화 추정은 확장자가 있을 때만).
    // 설정 › SQLite › 열기 ‘확장자가 달라도 머리말이 SQLite 이면 SQLite 로 열기’(프로세스 전체, 기본 켬).
    void SetHeaderSniffing(bool Enabled);
    bool HeaderSniffing();
    bool HasPlainHeader(const QByteArray& Header);
}
