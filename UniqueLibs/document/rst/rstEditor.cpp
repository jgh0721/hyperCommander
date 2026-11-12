#include "stdafx.h"
#include "rstEditor.hpp"

/// 모듈

/// UIs
#include "text/QTextView.h"
#include "text/RstStructure.h"
#include "text/ScintillaQtDirectBackend.h"

/// 기타 헤더파일

/// 고유 라이브러리
#include "core/ThemeManager.h"

/// 공통 라이브러리

/// 외부 라이브러리
#include <Scintilla.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFileInfo>
#include <QWidget>

#include <string_view>
#include <vector>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::rst {
namespace {

// 20–23 은 찾기, 24–25 는 괄호 강조, 26 은 EPUB 진단이 쓴다.
constexpr int ErrorIndicator = 27;
constexpr int WarningIndicator = 28;
constexpr int InfoIndicator = 29;
constexpr int HintIndicator = 30;
// EPUB 소스 편집기와 같은 표식 번호·여백을 쓴다. 두 보기는 한 편집기에 함께 붙지 않는다.
constexpr int WarningMarker = 10;
constexpr int ErrorMarker = 11;
constexpr int DiagnosticMargin = 3;

int indicatorFor( const int Severity )
{
    switch( Severity ) {
        case 1:  return ErrorIndicator;
        case 2:  return WarningIndicator;
        case 3:  return InfoIndicator;
        default: return HintIndicator;
    }
}

qintptr sciColor( const QColor& Color )
{
    return qintptr( Color.red() | ( Color.green() << 8 ) | ( Color.blue() << 16 ) );
}

}  // namespace

RstEditor::RstEditor( QTextView* View, QObject* Parent )
    : QObject( Parent ), view_( View )
{
    backend_ = View != nullptr ? qobject_cast< ScintillaQtDirectBackend* >( View->Backend() ) : nullptr;
    if( backend_.isNull() ) return;

    connect( backend_, &ScintillaQtDirectBackend::textChanged, this, &RstEditor::sigTextEdited );
    connect( backend_, &ScintillaQtDirectBackend::documentEdited, this, &RstEditor::sigDocumentEdited );
    connect( backend_, &ScintillaQtDirectBackend::charAdded, this, &RstEditor::sigCharAdded );
    connect( backend_, &ScintillaQtDirectBackend::viewportScrolled, this, &RstEditor::sigViewportScrolled );
    connect( backend_, &ScintillaQtDirectBackend::dwellStarted, this, &RstEditor::onDwellStarted );
    connect( backend_, &ScintillaQtDirectBackend::dwellEnded, this, &RstEditor::sigRoleHoverEnded );
    // 다른 본문으로 바뀌었다( 다시 읽기 ). 진단 표식은 옛 본문의 것이다.
    connect( backend_, &ScintillaQtDirectBackend::DocumentReset, this, [ this ] {
        setDiagnosticMarks( {} );
        emit sigFileReloadedFromDisk( path_ );
    } );
    // 테마가 바뀌면 밑줄 색을 다시 읽는다.
    connect( &ThemeManager::instance(), &ThemeManager::themeChanged, this, [ this ] {
        marksReady_ = false;
        setDiagnosticMarks( diagnostics_ );
    } );
}

RstEditor::~RstEditor()
{
    // 편집기는 이 보기보다 오래 산다( .rst 를 다른 확장자로 저장하면 일반 텍스트 보기로 돌아간다 ).
    if( !backend_.isNull() && marksReady_ ) setDiagnosticMarks( {} );
}

QTextView* RstEditor::view() const { return view_; }
QWidget* RstEditor::widget() const { return backend_.isNull() ? nullptr : backend_->widget(); }
QWidget* RstEditor::window() const { return view_.isNull() ? nullptr : view_->window(); }

bool RstEditor::isAncestorOf( const QWidget* Child ) const
{
    return !view_.isNull() && Child != nullptr && ( view_ == Child || view_->isAncestorOf( Child ) );
}

void RstEditor::setFilePath( const QString& Path )
{
    path_ = Path.isEmpty() ? QString{} : QFileInfo( Path ).absoluteFilePath();
}

QString RstEditor::currentFilePath() const { return path_; }

QString RstEditor::text() const { return backend_.isNull() ? QString{} : backend_->text(); }
bool RstEditor::isModified() const { return !backend_.isNull() && backend_->isModified(); }
QString RstEditor::lineText( const int Line ) const { return backend_.isNull() ? QString{} : backend_->lineText( Line - 1 ); }

QString RstEditor::textRange( const int StartPos, const int EndPos ) const
{
    return backend_.isNull() ? QString{} : QString::fromUtf8( backend_->textRangeUtf8( StartPos, EndPos ) );
}

int RstEditor::positionFromLineColumn( const int Line, const int Column ) const
{
    return backend_.isNull() ? 0 : backend_->positionFromLineColumn( qMax( 0, Line - 1 ), qMax( 0, Column - 1 ) );
}

int RstEditor::currentPosition() const { return backend_.isNull() ? 0 : backend_->currentPos(); }
int RstEditor::caretLine() const { return backend_.isNull() ? 1 : backend_->lineFromPosition( backend_->currentPos() ) + 1; }
int RstEditor::caretColumn() const { return backend_.isNull() ? 1 : backend_->columnFromPosition( backend_->currentPos() ) + 1; }
int RstEditor::firstVisibleLine() const { return backend_.isNull() ? 1 : backend_->firstVisibleLine() + 1; }

double RstEditor::fractionalLineAtViewportRatio( const double Ratio ) const
{
    return backend_.isNull() ? 1.0 : backend_->fractionalLineAtViewportRatio( Ratio );
}

void RstEditor::scrollFractionalLineToViewportRatio( const double FractionalLine, const double Ratio )
{
    if( !backend_.isNull() ) backend_->scrollFractionalLineToViewportRatio( FractionalLine, Ratio );
}

QPoint RstEditor::caretGlobalPos() const
{
    if( backend_.isNull() || backend_->widget() == nullptr ) return {};
    const int Caret = backend_->currentPos();
    const int LineHeight = qMax( 1, backend_->textHeight( backend_->lineFromPosition( Caret ) ) );
    return backend_->widget()->mapToGlobal( backend_->pointFromPosition( Caret ) + QPoint( 0, LineHeight ) );
}

void RstEditor::replaceRangeAtCursor( const int BackspaceCount, const QString& InsertText )
{
    if( backend_.isNull() ) return;
    const int Caret = backend_->currentPos();
    int Start = Caret;
    if( BackspaceCount > 0 ) {
        // 글자 수다. 바이트가 아니라 문서 위치로 바꿔야 한다.
        const int Line = backend_->lineFromPosition( Caret );
        Start = backend_->positionFromLineColumn( Line, qMax( 0, backend_->columnFromPosition( Caret ) - BackspaceCount ) );
    }
    backend_->setSearchTargetRange( Start, Caret );
    backend_->replaceInTarget( InsertText );
    const int NewCaret = backend_->targetEnd();
    backend_->setSelectionByPos( NewCaret, NewCaret );
}

void RstEditor::goToLine( const int Line, const int Column )
{
    if( view_.isNull() || backend_.isNull() ) return;
    const int Safe = qBound( 1, Line, qMax( 1, backend_->lineCount() ) );
    backend_->ensureLineVisible( Safe - 1 );
    view_->goToPosition( Safe, qMax( 1, Column ) );
}

void RstEditor::configureDiagnosticMarks()
{
    if( backend_.isNull() || marksReady_ ) return;
    const ThemeManager& Theme = ThemeManager::instance();
    const QColor Error = Theme.color( QStringLiteral( "text.diagnostic.error" ) );
    const QColor Warning = Theme.color( QStringLiteral( "text.diagnostic.warning" ) );
    const QColor Info = Theme.color( QStringLiteral( "text.diagnostic.info" ) );
    backend_->setIndicatorStyle( ErrorIndicator, INDIC_SQUIGGLE, Error );
    backend_->setIndicatorStyle( WarningIndicator, INDIC_SQUIGGLE, Warning );
    backend_->setIndicatorStyle( InfoIndicator, INDIC_SQUIGGLE, Info );
    backend_->setIndicatorStyle( HintIndicator, INDIC_DOTS, Info );
    for( const auto& [ Marker, Color ] : { std::pair{ WarningMarker, Warning }, std::pair{ ErrorMarker, Error } } ) {
        backend_->Send( SCI_MARKERDEFINE, Marker, SC_MARK_SMALLRECT );
        backend_->Send( SCI_MARKERSETBACK, Marker, sciColor( Color ) );
        backend_->Send( SCI_MARKERSETFORE, Marker, sciColor( Color ) );
    }
    backend_->Send( SCI_SETMARGINTYPEN, DiagnosticMargin, SC_MARGIN_SYMBOL );
    backend_->Send( SCI_SETMARGINMASKN, DiagnosticMargin, ( 1 << WarningMarker ) | ( 1 << ErrorMarker ) );
    marksReady_ = true;
}

void RstEditor::setDiagnosticMarks( const QVector< DiagnosticEntry >& Entries )
{
    if( backend_.isNull() ) return;
    configureDiagnosticMarks();
    for( const int Indicator : { ErrorIndicator, WarningIndicator, InfoIndicator, HintIndicator } )
        backend_->clearAllIndicator( Indicator );
    backend_->Send( SCI_MARKERDELETEALL, WarningMarker );
    backend_->Send( SCI_MARKERDELETEALL, ErrorMarker );

    diagnostics_ = Entries;
    QVector< QPair< int, bool > > ProblemLines;
    const int DocumentEnd = backend_->documentLength();
    const int LineCount = backend_->lineCount();
    for( const DiagnosticEntry& Entry : Entries ) {
        const int Line = qMax( 0, Entry.line - 1 );
        if( Line >= LineCount ) continue;
        const int Start = backend_->positionFromLineColumn( Line, qMax( 0, Entry.character - 1 ) );
        // 끝이 없거나 시작보다 앞이면 그 줄 끝까지 칠한다.
        int End = DocumentEnd;
        if( Entry.endLine >= Entry.line )
            End = backend_->positionFromLineColumn( qMax( 0, Entry.endLine - 1 ), qMax( 0, Entry.endCharacter - 1 ) );
        if( End <= Start ) End = backend_->lineEndPosition( Line );
        // 빈 줄이어도 한 글자는 칠해야 보인다.
        const int Length = qMax( 1, End - Start );
        if( Start < DocumentEnd )
            backend_->applyIndicator( indicatorFor( Entry.severity ), Start, qMin( Length, DocumentEnd - Start ) );
        if( Entry.severity == 1 || Entry.severity == 2 ) {
            backend_->Send( SCI_MARKERADD, Line, Entry.severity == 1 ? ErrorMarker : WarningMarker );
            ProblemLines.append( { Line, Entry.severity == 1 } );
        }
    }
    backend_->Send( SCI_SETMARGINWIDTHN, DiagnosticMargin, ProblemLines.isEmpty() ? 0 : 10 );
    if( !view_.isNull() ) view_->SetProblemLines( ProblemLines );
}

QVector< DiagnosticEntry > RstEditor::diagnosticsAtLine( const int Line ) const
{
    QVector< DiagnosticEntry > Found;
    for( const DiagnosticEntry& Entry : diagnostics_ )
        if( Entry.line == Line ) Found.append( Entry );
    return Found;
}

void RstEditor::feedRstCompletionVocabulary( const QStringList& Directives, const QStringList& Roles )
{
    if( backend_.isNull() ) return;
    syntax::RstMetadataCache* Cache = backend_->rstMetadataCache();
    if( Cache == nullptr ) return;   // 언어가 reStructuredText 가 아니다.

    bool Changed = false;
    for( const QString& Name : Directives ) Changed |= Cache->directives.insert( Name.toStdString() ).second;
    for( const QString& Name : Roles ) Changed |= Cache->roles.insert( Name.toStdString() ).second;
    // 비어 있는 동안은 "아직 모름" 이라 아무것도 오류 색이 아니다. 처음 채워지는 순간부터 목록에 없는 이름이
    // "알 수 없음" 이 된다.
    if( !Directives.isEmpty() && !Cache->directivesPopulated ) { Cache->directivesPopulated = true; Changed = true; }
    if( !Roles.isEmpty() && !Cache->rolesPopulated ) { Cache->rolesPopulated = true; Changed = true; }
    if( Changed ) backend_->invalidateStyling();
}

RstEditor::RoleAt RstEditor::roleAtPosition( const int Position ) const
{
    if( backend_.isNull() || Position < 0 ) return {};
    // 롤 판정은 렉서와 같은 스캐너가 한다. Scintilla 위치도 스캐너의 답도 UTF-8 바이트라 환산이 없다.
    const int Line = backend_->lineFromPosition( Position );
    const QByteArray LineUtf8 = backend_->lineText( Line ).toUtf8();
    const int ByteOffset = qMax( 0, Position - backend_->positionFromLine( Line ) );

    std::vector< syntax::InlineToken > Tokens;
    syntax::scanInline( std::string_view( LineUtf8.constData(), static_cast< std::size_t >( LineUtf8.size() ) ), Tokens );
    for( const syntax::InlineToken& Token : Tokens ) {
        if( Token.kind != syntax::InlineKind::Role ) continue;
        if( ByteOffset < static_cast< int >( Token.start ) || ByteOffset >= static_cast< int >( Token.end ) ) continue;
        // `:name:` + 백틱 + 대상 + 백틱. 대상은 이름 뒤 두 바이트부터 끝 하나 앞까지다.
        const auto NameLength = static_cast< qsizetype >( Token.nameEnd - Token.nameStart );
        const auto TargetStart = static_cast< qsizetype >( Token.nameEnd ) + 2;
        const auto TargetLength = static_cast< qsizetype >( Token.end ) - 1 - TargetStart;
        return { QString::fromUtf8( LineUtf8.constData() + Token.nameStart, NameLength ),
                 QString::fromUtf8( LineUtf8.constData() + TargetStart, qMax( qsizetype( 0 ), TargetLength ) ) };
    }
    return {};
}

void RstEditor::onDwellStarted( const int Position, const QPoint& ViewportPos )
{
    const RoleAt Found = roleAtPosition( Position );
    if( Found.role.isEmpty() || backend_.isNull() || backend_->widget() == nullptr ) {
        emit sigRoleHoverEnded();
        return;
    }
    emit sigRoleHovered( Found.role, Found.target, backend_->widget()->mapToGlobal( ViewportPos ) );
}

}  // namespace hc::rst
