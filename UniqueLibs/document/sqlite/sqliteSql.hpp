#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace hc::sqlite
{
    // 편집기 글(UTF-8) 안의 문 하나. 위치는 Scintilla 위치와 같은 UTF-8 바이트다.
    struct StatementRange
    {
        int start_ = 0, end_ = 0;           // [start, end) — 끝의 ; 까지.
        int bodyStart_ = 0, bodyEnd_ = 0;   // 앞뒤 공백 · 주석을 뺀 범위(거터 막대 · 실행 범위).
        bool Empty() const { return bodyEnd_ <= bodyStart_; }
    };

    // 문 경계(설계 5.3). 따옴표 · 대괄호 · 주석 밖의 ; 에서 sqlite3_complete 로 끝난 문인지 보고 자른다
    // (CREATE TRIGGER … BEGIN … ; … END; 도 한 문). 빈 문(공백 · 주석만)은 빼고, 마지막 ; 없는 문도 넣는다.
    QVector<StatementRange> SplitStatements(const QByteArray& Utf8);
    // 위치가 든 문(문 사이 공백이면 바로 앞 문). 없으면 -1.
    int StatementAt(const QVector<StatementRange>& Ranges, int Position);
    // WHERE 없는 UPDATE · DELETE 인가(실행 전 확인, 설계 6.3).
    bool IsUnboundedWrite(const QString& Statement);
    // 실행 중인 SQLite 의 키워드(소문자, LexSQL 키워드 목록 0번).
    QStringList Keywords();
    // 문 앞의 주석 · 공백을 뺀 첫 줄(메시지 · 기록 표시).
    QString Summary(const QString& Statement, int Length = 80);
}
