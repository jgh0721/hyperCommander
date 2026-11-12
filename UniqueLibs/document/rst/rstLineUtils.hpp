#pragma once

// reStructuredText 를 QString 위에서 훑는 인덱서들이 함께 쓰는 줄 단위 도우미.
//
// 판정 규칙 자체는 src/editor/RstStructure.hpp 한 곳에 있다. 여기 있는 것은 그중
// 줄 나누기·들여쓰기·빈 줄 판정을 **QString 그대로** 다루는 얇은 갈래일 뿐이다.
// 용어집·치환 인덱서는 워크스페이스의 문서 수천 개를 훑으므로 줄마다 UTF-8 로
// 옮기는 값을 치를 이유가 없다.
//
// 예전에는 이 세 함수가 두 파일에 바이트 단위로 같은 사본으로 있었다. 한쪽만
// 고치는 날 조용히 갈라지는 부류라서 한 곳으로 모은다.

#include <QChar>
#include <QString>
#include <QStringList>

namespace hc::rst::rstline {

/// 디스크에서 읽은 UTF-8 바이트를 글로 바꾼다. 앞의 BOM 은 벗긴다. conf.py · .rst · pyproject.toml ·
/// requirements 는 BOM 이 있든 없든 같게 읽혀야 한다( Sphinx · Python 도 utf-8-sig 로 읽는다 ).
/// QString::fromUtf8 은 BOM 을 첫 글자 U+FEFF 로 남겨, 첫 줄의 제목 · 대입 · [project] 를 가린다.
[[nodiscard]] inline QString fromUtf8File( const QByteArray& bytes )
{
    const bool bom = bytes.startsWith( "\xEF\xBB\xBF" );
    return QString::fromUtf8( bytes.constData() + ( bom ? 3 : 0 ), bytes.size() - ( bom ? 3 : 0 ) );
}

/// 줄 앞 공백의 시각적 폭. 탭은 docutils 와 같이 8칸으로 편다.
/// syntax::indentWidth 와 같은 규칙이다.
[[nodiscard]] inline int indentWidth( const QString& line )
{
    int width = 0;
    for( const QChar ch : line )
    {
        if( ch == QLatin1Char( ' ' ) )
            ++width;
        else if( ch == QLatin1Char( '\t' ) )
            width += 8 - ( width % 8 );
        else
            break;
    }
    return width;
}

/// 줄 앞 공백 **문자 수**. indentWidth 와 달리 탭을 8칸으로 펴지 않는다.
///
/// 자동완성 문맥 판정이 정규식의 `captured(1).length()` 와 견주기 때문에 그쪽은
/// 문자 수라야 한다. 한쪽만 폭으로 바꾸면 탭이 섞인 문서에서 조용히 어긋난다.
[[nodiscard]] inline int leadingSpaceCount( const QString& line )
{
    int index = 0;
    while( index < line.length() && line.at( index ).isSpace() )
        ++index;
    return index;
}

[[nodiscard]] inline bool isBlank( const QString& line )
{
    return line.trimmed().isEmpty();
}

/// 줄 끝 문자가 무엇이든 같은 결과가 나오게 나눈다.
///
/// 정규식으로 나누지 않는다 — 100KB 문서에서 그 차이가 그대로 보인다.
[[nodiscard]] inline QStringList splitLines( const QString& text )
{
    QStringList lines = text.split( QLatin1Char( '\n' ) );
    for( QString& line : lines )
    {
        if( line.endsWith( QLatin1Char( '\r' ) ) )
            line.chop( 1 );
    }
    return lines;
}

}   // namespace hc::rst::rstline
