#pragma once

#include "esbonioLspClient.hpp"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

namespace hc::rst {

/// 개요 트리에 올라가는 항목 하나.
///
/// LSP documentSymbol 과 정규식 폴백이 같은 모양을 내놓아야 트리 쪽에서
/// 둘을 구분하지 않아도 된다.
struct OutlineSymbol
{
    QString                             name;
    QString                             detail;
    int                                 kind = 0;      ///< LSP SymbolKind
    int                                 line = 1;      ///< 1-based
    QString                             path;          ///< 절대경로. 비면 활성 문서
    QVector< OutlineSymbol >            children;
};

/// reStructuredText 섹션 개요를 텍스트만 보고 뽑는다.
///
/// Esbonio 는 Sphinx 환경이 데워지기 전에는 documentSymbol 에 빈 결과를
/// 돌려준다. 그 사이에도 개요 패널이 비어 있지 않게 하는 것이 목적이다.
///
/// docutils 와 같은 규칙을 따른다: 계층은 **처음 나온 순서** 로 정해진다.
/// 문서마다 어떤 밑줄 문자가 몇 단계인지 다르므로 고정 표를 쓰면 안 된다.
[[nodiscard]] QVector< OutlineSymbol > parseRstOutline( const QString& text,
                                                        const QString& path = {} );

/// Markdown 제목 개요를 텍스트만 보고 뽑는다.
///
/// 계층 규칙이 reST 와 다르다 — 해시 개수가 곧 단계이므로 "처음 나온 순서" 를
/// 셀 필요가 없다. setext 제목(`===`/`---`)도 1·2단계로 함께 잡는다.
///
/// 줄 번호는 setext 의 경우 밑줄이 아니라 **제목 글자가 있는 줄**을 가리킨다.
/// parseRstOutline 은 밑줄 줄을 가리키는데, 프리뷰가 제목 글자 줄에 앵커를
/// 붙이므로 여기서 갈라진다. 의도된 차이다.
[[nodiscard]] QVector< OutlineSymbol > parseMarkdownOutline( const QString& text,
                                                            const QString& path = {} );

/// 확장자로 파서를 고른다.
///
/// 마크업 판정을 한 곳에만 두려는 것이 목적이다. 호출 지점마다 각자 if 를 두면
/// 한쪽만 고쳐지는 순간 그 경로의 개요가 조용히 사라진다.
/// path 가 비어 있으면 reST 로 본다 — 활성 문서 경로는 항상 채워져 있다.
[[nodiscard]] QVector< OutlineSymbol > parseDocumentOutline( const QString& text,
                                                            const QString& path );

/// LSP documentSymbol 응답을 개요 항목으로 옮긴다.
[[nodiscard]] QVector< OutlineSymbol > toOutlineSymbols( const QList< LspDocumentSymbol >& symbols,
                                                         const QString& path );

/// 프로젝트 개요 트리의 파일 한 줄.
struct OutlineDocumentEntry
{
    QString                             label;     ///< 소스 루트 기준 상대경로
    QString                             path;      ///< 절대경로
    QVector< OutlineSymbol >            symbols;
};

/// 프로젝트 개요에 올릴 문서 목록. root_doc 이 맨 앞에 오고 나머지는 경로순.
///
/// limit 을 넘으면 잘라내고 totalFound 에 원래 개수를 담는다 — 조용히 자르면
/// "전부 보여 준 것" 처럼 읽힌다.
[[nodiscard]] QStringList collectProjectDocuments( const QString& sourceRoot, const QString& rootDoc,
                                                   int limit, int* totalFound = nullptr );

/// 문서 목록을 읽어 각각의 섹션 개요를 뽑는다. 디스크 I/O 가 있으므로
/// 작업 스레드에서 부른다.
[[nodiscard]] QVector< OutlineDocumentEntry > buildProjectOutline( const QString& sourceRoot,
                                                                   const QStringList& documents );

}  // namespace hc::rst

Q_DECLARE_METATYPE( hc::rst::OutlineSymbol )
Q_DECLARE_METATYPE( hc::rst::OutlineDocumentEntry )
