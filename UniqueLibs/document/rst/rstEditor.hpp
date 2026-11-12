#pragma once

/// 모듈
#include "sphinxDiagnostics.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

class QTextView;
class QWidget;
class ScintillaQtDirectBackend;

namespace hc::rst {

/// reStructuredText 서비스( 언어 서버 동기화, 자동완성, 미리보기 스크롤 동기화, 진단 표시 )가 보는 편집기.
///
/// HyperEditor 의 텍스트 편집기( QTextView + ScintillaQtDirectBackend )를 감싼다. 서비스 계층은
/// MultiRoot reST Editor 에서 옮겨 왔고 그쪽 편집기의 접근자와 신호를 쓴다. 그 모양을 여기서 맞춰 주어
/// 서비스 코드를 고치지 않는다.
///
/// 밖에서 쓰는 줄·열은 **1 부터** 센다( 열은 UTF-16 단위 ). 위치는 UTF-8 바이트 오프셋이다.
///
/// 문서 경로는 편집기가 아니라 문서 창이 안다( 새 문서는 임시 파일이고, 저장하면 바뀐다 ).
/// setFilePath() 로 알려 준다.
class RstEditor final : public QObject
{
    Q_OBJECT

public:
    explicit RstEditor( QTextView* View, QObject* Parent = nullptr );
    ~RstEditor() override;

    [[nodiscard]] QTextView*            view() const;
    /// 글자가 그려지는 위젯( Scintilla ). 팝업 위치와 키 입력 가로채기의 기준이다.
    [[nodiscard]] QWidget*              widget() const;
    [[nodiscard]] QWidget*              window() const;
    [[nodiscard]] bool                  isAncestorOf( const QWidget* Child ) const;

    void                                setFilePath( const QString& Path );
    [[nodiscard]] QString               currentFilePath() const;

    [[nodiscard]] QString               text() const;
    [[nodiscard]] bool                  isModified() const;
    [[nodiscard]] QString               lineText( int Line ) const;
    [[nodiscard]] QString               textRange( int StartPos, int EndPos ) const;
    [[nodiscard]] int                   positionFromLineColumn( int Line, int Column ) const;
    [[nodiscard]] int                   currentPosition() const;
    /// 캐시를 거치지 않고 Scintilla 에 직접 묻는다. SCN_CHARADDED 가 SCN_UPDATEUI 보다 먼저 오므로,
    /// 글자를 넣은 직후에는 캐시된 캐럿이 한 글자 뒤처져 있다.
    [[nodiscard]] int                   caretLine() const;
    [[nodiscard]] int                   caretColumn() const;
    [[nodiscard]] int                   firstVisibleLine() const;
    /// 창 세로 Ratio 위치에 보이는 줄( 소수 ). 12.5 는 12번 줄의 세로 중간이다.
    [[nodiscard]] double                fractionalLineAtViewportRatio( double Ratio ) const;
    void                                scrollFractionalLineToViewportRatio( double FractionalLine, double Ratio );
    /// 캐럿 바로 아래의 화면 좌표. 자동완성 목록의 기준점이다.
    [[nodiscard]] QPoint                caretGlobalPos() const;
    /// 캐럿 앞 BackspaceCount 글자를 지우고 InsertText 를 넣는다.
    void                                replaceRangeAtCursor( int BackspaceCount, const QString& InsertText );
    /// 줄로 가서 캐럿을 놓는다. 접힌 블록 안이면 펼친다.
    void                                goToLine( int Line, int Column = 1 );

    /// 진단 밑줄과 여백 표식, 미니맵 문제 표식을 다시 그린다. 빈 목록이면 모두 지운다.
    void                                setDiagnosticMarks( const QVector< DiagnosticEntry >& Entries );
    [[nodiscard]] const QVector< DiagnosticEntry >& diagnostics() const { return diagnostics_; }
    /// 이 위치의 줄에 걸린 진단. 없으면 빈 목록.
    [[nodiscard]] QVector< DiagnosticEntry > diagnosticsAtLine( int Line ) const;
    /// 언어 서버 자동완성에서 얻은 지시어·롤 이름을 렉서에 알린다. 목록이 채워진 뒤에야 목록에 없는 이름이
    /// "알 수 없음" 으로 칠해진다.
    void                                feedRstCompletionVocabulary( const QStringList& Directives, const QStringList& Roles );
    /// Pos( 문서 위치 )에 있는 `:role:`target``. 없으면 둘 다 빈 문자열.
    struct RoleAt { QString role, target; };
    [[nodiscard]] RoleAt                roleAtPosition( int Position ) const;

signals:
    void                                sigFileOpened( const QString& FilePath );
    void                                sigFileReloadedFromDisk( const QString& FilePath );
    void                                sigTextEdited();
    /// LSP 좌표: 0 부터 세는 줄과 줄머리로부터의 UTF-8 바이트 수.
    void                                sigDocumentEdited( int StartLine, int StartColumn, int OldEndLine, int OldEndColumn,
                                                           const QByteArray& NewText );
    void                                sigCharAdded( int Ch );
    void                                sigViewportScrolled();
    void                                sigRoleHovered( const QString& Role, const QString& Target, const QPoint& GlobalPos );
    void                                sigRoleHoverEnded();

private:
    void                                configureDiagnosticMarks();
    void                                onDwellStarted( int Position, const QPoint& ViewportPos );

    QPointer< QTextView >               view_;
    QPointer< ScintillaQtDirectBackend > backend_;
    QString                             path_;
    QVector< DiagnosticEntry >          diagnostics_;
    bool                                marksReady_ = false;
};

}  // namespace hc::rst
