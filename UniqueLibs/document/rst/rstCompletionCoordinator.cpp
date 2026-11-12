#include "stdafx.h"
#include "rstCompletionCoordinator.hpp"

/// 모듈
#include "rstEditor.hpp"
#include "text/RstContainerLexer.h"
#include "rstFileKinds.hpp"
#include "rstLineUtils.hpp"
#include "rstGlossaryIndex.hpp"
#include "rstPathIndex.hpp"
#include "rstSettings.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QScopedValueRollback>
#include <QTimer>

DECLARE_CMNLIBSV2_NAMESPACE
namespace hc::rst {
namespace {

/// 파이썬 원본과 같은 감각. 너무 짧으면 한 글자마다 요청이 날아가고,
/// 너무 길면 팝업이 뒤늦게 뜬다.
constexpr int kDebounceMs = 150;

/// 빠른 파일 열기의 인덱스는 워크스페이스 전체를 보존하지만 기존 reST 경로
/// 자동완성은 GUI 스레드에서 동기적으로 후보를 만든다. 여기까지 무제한으로
/// 순회하면 큰 워크스페이스에서 키 입력이 멈추므로 종전의 응답시간 경계를
/// 자동완성 소비자에만 유지한다.
constexpr qsizetype kPathCompletionScanLimit = 20'000;

/// reST 완성기는 모든 텍스트 편집기에 연결되지만 Markdown 문법까지 해석해서는
/// 안 된다. 확장자 없는 새 문서와 사용자 정의 Sphinx 소스 확장자는 기존처럼
/// 허용하고, 앱이 Markdown 으로 확정한 확장자만 명시적으로 제외한다.
bool isMarkdownEditor( const RstEditor* view )
{
    return view != nullptr
           && filekinds::hasExtension( view->currentFilePath(), filekinds::markdownExtensions() );
}

/// 이 문자를 입력했을 때만 완성을 시작한다. 그 외 글자는 이미 떠 있는 팝업의
/// 필터만 갱신한다. 평범한 산문을 치는 동안 팝업이 튀어나오면 안 된다.
bool isTriggerCharacter( const int character )
{
    switch( character )
    {
        case '.':
        case ':':
        case '`':
        case '/':
        case '\\':
        case '<':
        case '>':
        case ' ':
        case '|':
            return true;
        default:
            return false;
    }
}

CompletionDisplayItem toDisplay( const rstcomplete::Item& item )
{
    return { item.label, item.insertText, item.detail, item.kind, item.label };
}

/// 치환 후보의 순서. 팝업은 프로젝트도 conf.py 도 모르므로 "이게 더 그럴듯하다"
/// 는 판단은 여기서 해서 점수로 넘긴다.
///
/// docutils 의 치환은 문서 단위다. 다른 문서에서 정의한 것은 그 문서를
/// include 하지 않았다면 못 쓰므로 맨 뒤에 둔다 — 빼지는 않는다. 공용 조각
/// 파일 하나를 모든 문서에 include 하는 형태가 실사용에서 흔하기 때문이다.
int substitutionScoreBias( const SubstitutionOrigin origin )
{
    switch( origin )
    {
        case SubstitutionOrigin::Document: return 40;
        case SubstitutionOrigin::Conf:     return 30;
        case SubstitutionOrigin::Builtin:  return 20;
        case SubstitutionOrigin::Project:  return 0;
    }
    return 0;
}

/// 상세 패널 아래에 붙는 출처 줄.
QString substitutionSource( const SubstitutionEntry& entry )
{
    const QString where =
        entry.path.isEmpty()
            ? QString{}
            : QStringLiteral( "%1:%2" ).arg( QFileInfo( entry.path ).fileName() ).arg( entry.line );

    switch( entry.origin )
    {
        case SubstitutionOrigin::Document:
            return QObject::tr( "이 문서" );
        case SubstitutionOrigin::Builtin:
            return where.isEmpty() ? QObject::tr( "Sphinx 기본 치환" )
                                   : QObject::tr( "Sphinx 기본 치환 · %1" ).arg( where );
        case SubstitutionOrigin::Conf:
        case SubstitutionOrigin::Project:
            break;
    }
    return where;
}

rstcomplete::Item fromLsp( const LspCompletionItem& item )
{
    return { item.label, item.insertText.isEmpty() ? item.label : item.insertText, item.detail,
            item.kind };
}

QList< CompletionDisplayItem > toDisplayList( const QVector< rstcomplete::Item >& items )
{
    QList< CompletionDisplayItem > display;
    display.reserve( items.size() );
    for( const rstcomplete::Item& item : items )
        display.push_back( toDisplay( item ) );
    return display;
}

}  // namespace

CompletionCoordinator::CompletionCoordinator( QObject* parent )
    : QObject( parent )
    , popup_( new CompletionPopupWidget( nullptr ) )
    , detail_( new CompletionDetailPopup( nullptr ) )
    , debounce_( new QTimer( this ) )
    , previewLoader_( new FilePreviewLoader( this ) )
{
    connect( previewLoader_, &FilePreviewLoader::previewReady, this,
            [ this ]( const FilePreviewLoader::Result& result ) {
                if( !detail_.isNull() )
                    detail_->applyPreview( result.token, result.pixmap, result.hasAlpha,
                                          result.metaLine, result.note );
            } );
    debounce_->setSingleShot( true );
    debounce_->setInterval( kDebounceMs );
    connect( debounce_, &QTimer::timeout, this, &CompletionCoordinator::flushTrigger );

    connect( popup_, &CompletionPopupWidget::itemSelected, this,
            &CompletionCoordinator::insertCompletion );
    connect( popup_, &CompletionPopupWidget::currentItemChanged, this,
            &CompletionCoordinator::refreshDetailPopup );
    // 목록은 Esc 키, 항목 확정, 필터 결과 없음 등 여러 경로로 스스로 숨는다.
    // 그중 Esc 는 여기까지 알려 오는 길이 없어서 오른쪽 상세 패널만 화면에
    // 남아 있었다. 숨김 자체를 신호로 받아 한 곳에서 정리한다.
    connect( popup_, &CompletionPopupWidget::popupHidden, this,
            &CompletionCoordinator::hidePopup );
}

CompletionCoordinator::~CompletionCoordinator()
{
    // 앱 객체가 먼저 사라졌을 수 있다( 앱의 자식으로 지워지는 경우 ). 필터는 Qt 가 이 객체와 함께 걷어 낸다.
    if( auto* app = QCoreApplication::instance() )
        app->removeEventFilter( this );
    delete detail_.data();
    delete popup_.data();
}

void CompletionCoordinator::setPathIndex( PathIndex* index )
{
    pathIndex_ = index;
}

void CompletionCoordinator::notifyPathIndexReady( const QString& root )
{
    if( !isPopupVisible() || popup_.isNull() )
        return;
    if( rstpath::slotFor( shownContext_ ) == nullptr )
        return;   // 사용자가 이미 딴 데로 갔다
    if( root != workspaceRoot_ && !workspaceRoot_.isEmpty() )
        return;

    // 인덱스가 늦게 도착했다. 지금 목록에 전역 후보를 보태 준다.
    recollectPathItems();
}

void CompletionCoordinator::setGlossaryIndex( GlossaryIndex* glossary )
{
    glossary_ = glossary;
}

void CompletionCoordinator::setSubstitutionIndex( SubstitutionIndex* substitutions )
{
    substitutions_ = substitutions;
}

void CompletionCoordinator::showPopupAtCaret()
{
    if( popup_.isNull() || activeView_.isNull() || popup_->visibleCount() <= 0 )
        return;

    // 부모 없는 Tool 창은 Windows 에서 메인 창 뒤로 숨는다. 창이 바뀔 때마다
    // 붙여 준다 (탭이 다른 창으로 떨어져 나갈 수 있다).
    QWidget* window = activeView_->window();
    if( popup_->parentWidget() != window )
        popup_->setParent( window, popup_->windowFlags() );
    if( !detail_.isNull() && detail_->parentWidget() != window )
        detail_->setParent( window, detail_->windowFlags() );

    popup_->showAt( activeView_->caretGlobalPos() );
    qApp->installEventFilter( this );
    sessionArmed_ = true;
    sessionLine_ = activeView_->caretLine();

    // 목록이 자리를 잡은 뒤에 옆에 붙인다.
    refreshDetailPopup( popup_->currentItem() );
}

void CompletionCoordinator::showOrRefreshPopup()
{
    if( popup_.isNull() || activeView_.isNull() || popup_->visibleCount() <= 0 )
        return;

    if( !popup_->isVisible() )
    {
        showPopupAtCaret();
        return;
    }

    // 이미 떠 있는 목록을 showAt() 으로 다시 띄우면 그동안 움직인 캐럿을 따라
    // 옆으로 튄다. LSP 응답은 200~500ms 뒤에 오는데, 그 사이에 "../images/" 를
    // 치면 그만큼 점프한다. 경로 완성은 타이핑이 길어 매우 잘 보인다.
    popup_->refreshGeometry();
    refreshDetailPopup( popup_->currentItem() );
}

void CompletionCoordinator::refreshDetailPopup( const CompletionDisplayItem& item )
{
    if( detail_.isNull() )
        return;
    if( popup_.isNull() || !popup_->isVisible() )
    {
        detail_->hide();
        return;
    }

    if( !showPathDetail( item ) )
    {
        QString title;
        QString body;
        QString source;
        if( !buildDetailContent( item, &title, &body, &source )
            || !detail_->setContent( title, body, source ) )
        {
            detail_->hide();
            return;
        }
    }

    detail_->showBesideAnchor( popup_->geometry() );
}

bool CompletionCoordinator::showPathDetail( const CompletionDisplayItem& item )
{
    const auto found = pathCandidates_.constFind( item.insertText );
    if( found == pathCandidates_.constEnd() || detail_.isNull() )
        return false;

    const rstpath::Candidate& candidate = found.value();

    CompletionFileDetail detail;
    detail.fileName = candidate.label;
    // 문서에 실제로 들어갈 경로를 보여 준다. 절대 경로보다 이것이
    // 지금 고르는 후보를 판단하는 데 쓸모 있다.
    const rstpath::TypedPath split = rstpath::splitTypedPath( candidate.insertText );
    detail.directoryText = split.directory.isEmpty()
                               ? QStringLiteral( "./" )
                               : split.directory + QLatin1Char( '/' );
    detail.showPreviewBox = imagePreview_ && !candidate.isDirectory;

    detailToken_ = detail_->setFileContent( detail );

    if( imagePreview_ && previewLoader_ != nullptr && !candidate.absolutePath.isEmpty() )
    {
        // 고DPI 에서 흐려지지 않게 dpr 만큼 크게 디코딩한다.
        previewLoader_->request( detailToken_, candidate.absolutePath,
                                detail_->previewBoxSize(), detail_->devicePixelRatioF(),
                                candidate.isDirectory );
    }
    return true;
}

bool CompletionCoordinator::buildDetailContent( const CompletionDisplayItem& item, QString* title,
                                                QString* body, QString* source ) const
{
    if( item.label.isEmpty() )
        return false;

    *title = item.label;
    *body = item.detail;
    source->clear();

    // 치환 후보면 정의 전문과 정의한 자리를 보여 준다. 무엇으로 펼쳐지는지
    // 모르면 `|br|` 과 `|pb|` 중 어느 쪽인지 이름만으로는 고를 수 없다.
    if( const auto found = substitutionCandidates_.constFind( item.insertText );
        found != substitutionCandidates_.constEnd() )
    {
        const SubstitutionEntry& entry = found.value();
        *title = QStringLiteral( "|%1|" ).arg( entry.name );
        *body = substitutionDetail( entry );
        *source = substitutionSource( entry );
        return !body->isEmpty() || !source->isEmpty();
    }

    // 용어집 항목이면 정의 전문과 출처를 붙인다. 롤 대상 컨텍스트가 아니어도
    // 이름이 용어와 같으면 보여 준다 (LSP 가 준 후보도 마찬가지로 걸린다).
    const bool termContext = shownContext_.kind == rstcomplete::ContextKind::RoleTarget
                             && shownContext_.directiveName == QLatin1String( "term" );
    if( glossary_ != nullptr && ( termContext || shownContext_.kind == rstcomplete::ContextKind::None ) )
    {
        if( const GlossaryEntry* entry = glossary_->lookup( item.label ); entry != nullptr )
        {
            *body = entry->definition;
            if( !entry->path.isEmpty() )
            {
                *source = QStringLiteral( "%1:%2" )
                              .arg( QFileInfo( entry->path ).fileName() )
                              .arg( entry->line );
            }
        }
    }

    return !title->isEmpty() && ( !body->isEmpty() || !source->isEmpty() );
}

QList< CompletionDisplayItem >
CompletionCoordinator::localCandidatesFor( const rstcomplete::Context& context )
{
    // 경로 후보는 우리가 직접 만든다. Esbonio 는 4종 directive 만, 확장자
    // 필터 없이, 그것도 내부 Sphinx 빌드가 끝난 뒤에야 답한다.
    if( rstpath::slotFor( context ) != nullptr )
    {
        substitutionCandidates_.clear();
        return pathCandidatesFor( context );
    }

    pathCandidates_.clear();

    // 치환 후보도 우리 몫이다. Esbonio 는 `|` 자리에 아무것도 주지 않는다.
    if( context.kind == rstcomplete::ContextKind::Substitution )
        return substitutionCandidates();

    substitutionCandidates_.clear();

    QList< CompletionDisplayItem > items;
    if( glossary_ == nullptr || context.kind != rstcomplete::ContextKind::RoleTarget )
        return items;
    if( context.directiveName != QLatin1String( "term" ) )
        return items;

    // Esbonio 는 프로젝트 빌드가 끝나야 대상 후보를 만든다. 그 사이에도
    // 용어집만큼은 우리가 직접 훑어 둔 것이 있으므로 바로 쓸 수 있다.
    const QVector< GlossaryEntry > entries = glossary_->match( QString{} );
    items.reserve( static_cast< int >( entries.size() ) );
    for( const GlossaryEntry& entry : entries )
    {
        CompletionDisplayItem item;
        item.label = entry.term;
        item.insertText = entry.term;
        item.detail = QObject::tr( "용어집" );
        item.kind = 21;   // LSP CompletionItemKind::Constant — 다른 롤 대상과 구분만 되면 된다
        item.filterText = entry.term;
        items.push_back( item );
    }
    return items;
}

QList< CompletionDisplayItem > CompletionCoordinator::substitutionCandidates()
{
    substitutionCandidates_.clear();

    // 지금 편집 중인 버퍼가 먼저다. 방금 위쪽에 정의한 `.. |x| replace::` 는
    // 저장하기 전까지 디스크에도 인덱스에도 없는데, 그것이 바로 지금 쓰려는
    // 후보다. 문서 하나를 훑는 비용은 목록을 한 번 여는 동안 한 번뿐이다.
    QVector< SubstitutionEntry > entries;
    if( !activeView_.isNull() )
    {
        entries = parseSubstitutions( activeView_->text(), editorPath(),
                                     SubstitutionOrigin::Document );
    }
    if( substitutions_ != nullptr )
        entries += substitutions_->entries();
    // 인덱스가 아직 훑기 전이어도 |version| 셋은 낸다. 인덱스 쪽에 conf.py 값이
    // 실린 같은 이름이 이미 있으면 아래 중복 제거에서 이쪽이 밀린다.
    entries += builtinSubstitutions();

    // 닫는 `|` 를 이미 쳐 두었으면 하나 더 넣지 않는다 (`|logo||` 가 된다).
    bool closingBarTyped = false;
    if( !activeView_.isNull() )
    {
        const QString line = activeView_->lineText( activeView_->caretLine() );
        const int     caret = activeView_->caretColumn() - 1;
        closingBarTyped = caret >= 0 && caret < line.length()
                       && line.at( caret ) == QLatin1Char( '|' );
    }

    QList< CompletionDisplayItem > items;
    items.reserve( static_cast< qsizetype >( entries.size() ) );

    QSet< QString > seen;
    seen.reserve( static_cast< int >( entries.size() ) );
    for( const SubstitutionEntry& entry : std::as_const( entries ) )
    {
        // 같은 이름이 여럿이면 **먼저 온 출처**가 이긴다. 버퍼 → conf.py →
        // Sphinx 기본 → 다른 문서 순으로 쌓아 두었다.
        const QString key = entry.name.toCaseFolded();
        if( seen.contains( key ) )
            continue;
        seen.insert( key );

        CompletionDisplayItem item;
        item.label = entry.name;
        item.insertText = closingBarTyped ? entry.name : entry.name + QLatin1Char( '|' );
        item.detail = substitutionSummary( entry );
        item.kind = rstcomplete::kKindSubstitution;
        item.filterText = entry.name;
        item.scoreBias = substitutionScoreBias( entry.origin );
        items.push_back( item );

        substitutionCandidates_.insert( item.insertText, entry );
    }
    return items;
}

void CompletionCoordinator::notifySubstitutionsReady( const QString& projectId )
{
    if( projectId != activeProjectId_ || !isPopupVisible() || popup_.isNull() )
        return;
    if( shownContext_.kind != rstcomplete::ContextKind::Substitution )
        return;

    const QList< CompletionDisplayItem > local = substitutionCandidates();
    if( local.isEmpty() )
        return;

    offlineItems_ = local;
    popup_->setItems( offlineItems_ );
    popup_->updateFilter( shownContext_.filterPrefix );
    showOrRefreshPopup();
}

rstpath::Query CompletionCoordinator::pathQueryFor( const rstcomplete::Context& context ) const
{
    rstpath::Query query;
    query.context = context;
    const QString documentPath = editorPath();
    if( !documentPath.isEmpty() )
        query.documentDirectory = QFileInfo( documentPath ).absolutePath();
    query.sourceRoot = sourceRoot_;
    query.workspaceRoot = workspaceRoot_;
    return query;
}

QList< CompletionDisplayItem >
CompletionCoordinator::pathCandidatesFor( const rstcomplete::Context& context )
{
    QList< CompletionDisplayItem > items;
    pathCandidates_.clear();

    const rstpath::Query query = pathQueryFor( context );
    if( query.documentDirectory.isEmpty() )
        return items;

    QVector< rstpath::Candidate > found =
        rstpath::oneLevelCandidates( query, rstpath::diskLister() );

    // 워크스페이스 전역 인덱스는 여기서 처음 필요해진다. 프로젝트를 열 때가
    // 아니라 지금 시작하는 것이 지연 시작의 요점이다.
    if( pathIndex_ != nullptr && !workspaceRoot_.isEmpty() )
    {
        pathIndex_->ensure( workspaceRoot_ );
        if( pathIndex_->isReadyFor( workspaceRoot_ ) )
        {
            found = rstpath::mergeCandidates(
                std::move( found ),
                rstpath::fuzzyCandidates( query, pathIndex_->indexedRoot(),
                                         pathIndex_->paths(), 50,
                                         kPathCompletionScanLimit ) );
        }
    }

    items.reserve( static_cast< qsizetype >( found.size() ) );
    for( const rstpath::Candidate& candidate : found )
    {
        // 라벨은 파일 이름뿐이어야 한다. 팝업의 퍼지 필터가 마지막 조각만 받기
        // 때문이고, 그래야 강조 위치도 라벨 기준으로 맞는다.
        CompletionDisplayItem item;
        item.label = candidate.label;
        item.insertText = candidate.insertText;
        item.detail = candidate.detail;
        item.kind = candidate.kind;
        item.filterText = candidate.label;
        item.scoreBias = candidate.scoreBias;
        items.push_back( item );

        pathCandidates_.insert( candidate.insertText, candidate );
    }
    return items;
}

void CompletionCoordinator::recollectPathItems()
{
    if( popup_.isNull() )
        return;

    offlineItems_ = pathCandidatesFor( shownContext_ );
    popup_->setItems( offlineItems_ );
    popup_->updateFilter( shownContext_.filterPrefix );
    if( popup_->visibleCount() <= 0 )
    {
        hidePopup();
        return;
    }
    showOrRefreshPopup();
}

void CompletionCoordinator::notifyGlossaryReady( const QString& projectId )
{
    if( projectId != activeProjectId_ || !isPopupVisible() || popup_.isNull() )
        return;
    if( shownContext_.kind != rstcomplete::ContextKind::RoleTarget
        || shownContext_.directiveName != QLatin1String( "term" ) )
    {
        return;
    }

    const QList< CompletionDisplayItem > local = localCandidatesFor( shownContext_ );
    if( local.isEmpty() )
        return;

    offlineItems_ = local;
    popup_->setItems( offlineItems_ );
    popup_->updateFilter( shownContext_.filterPrefix );
    showOrRefreshPopup();
}

void CompletionCoordinator::showHoverDetail( const QString& role, const QString& target,
                                             const QPoint& globalPos )
{
    if( detail_.isNull() || glossary_ == nullptr || !termHover_ )
        return;
    // 자동완성이 떠 있으면 그쪽이 우선이다. 두 팝업이 겹치면 읽을 수 없다.
    if( isPopupVisible() )
        return;
    if( role != QLatin1String( "term" ) || target.trimmed().isEmpty() )
    {
        detail_->hide();
        return;
    }

    const GlossaryEntry* entry = glossary_->lookup( target );
    if( entry == nullptr )
    {
        detail_->hide();
        return;
    }

    if( !activeView_.isNull() )
    {
        QWidget* window = activeView_->window();
        if( detail_->parentWidget() != window )
            detail_->setParent( window, detail_->windowFlags() );
    }

    const QString source = entry->path.isEmpty()
                               ? QString{}
                               : QStringLiteral( "%1:%2" )
                                     .arg( QFileInfo( entry->path ).fileName() )
                                     .arg( entry->line );
    if( !detail_->setContent( entry->term, entry->definition, source ) )
    {
        detail_->hide();
        return;
    }

    detail_->showNearPoint( globalPos );
}

void CompletionCoordinator::hideHoverDetail()
{
    if( !detail_.isNull() && !isPopupVisible() )
        detail_->hide();
}

void CompletionCoordinator::reloadSettings()
{
    const Settings settings;
    autoTrigger_ = settings.value( QStringLiteral( "completion/autoTrigger" ), true ).toBool();
    imagePreview_ = settings.value( QStringLiteral( "completion/imagePreview" ), true ).toBool();
    termHover_ = settings.value( QStringLiteral( "completion/termHover" ), true ).toBool();
    if( !termHover_ )
        hideHoverDetail();
}

void CompletionCoordinator::attachEditor( RstEditor* view )
{
    if( view == nullptr )
        return;

    connect( view, &RstEditor::sigCharAdded, this, &CompletionCoordinator::onCharAdded,
            Qt::UniqueConnection );
    // 삽입만이 아니라 **삭제**도 받아야 한다. sigCharAdded 는 넣은 글자만
    // 알려 주므로, 그것만 보면 지웠을 때 목록이 옛 접두에 좁혀진 채 남는다.
    connect( view, &RstEditor::sigTextEdited, this, &CompletionCoordinator::onTextEdited,
            Qt::UniqueConnection );
    // 캐럿이 다른 곳으로 뛰면 지금 띄운 후보는 무의미하다.
    connect( view, &RstEditor::sigViewportScrolled, this, &CompletionCoordinator::hidePopup,
            Qt::UniqueConnection );
}

void CompletionCoordinator::detachEditor( RstEditor* view )
{
    if( view == nullptr )
        return;

    disconnect( view, nullptr, this, nullptr );
    if( activeView_ == view )
    {
        hidePopup();
        activeView_ = nullptr;
    }
}

void CompletionCoordinator::setActiveEditor( RstEditor* view )
{
    if( activeView_ == view )
        return;

    hidePopup();
    activeView_ = view;
}

void CompletionCoordinator::setActiveProject( const QString& projectId,
                                           const QString& sourceRoot,
                                           const QString& workspaceRoot )
{
    if( activeProjectId_ == projectId && sourceRoot_ == sourceRoot
        && workspaceRoot_ == workspaceRoot )
    {
        return;
    }

    activeProjectId_ = projectId;
    sourceRoot_ = sourceRoot;
    workspaceRoot_ = workspaceRoot;
    hidePopup();
}

bool CompletionCoordinator::isPopupVisible() const
{
    return popup_ != nullptr && popup_->isActive();
}

void CompletionCoordinator::hidePopup()
{
    // popup_->hide() 는 popupHidden 을 거쳐 이 함수로 되돌아온다. 한 번만 돈다.
    if( hidingPopup_ )
        return;
    const QScopedValueRollback< bool > guard( hidingPopup_, true );

    debounce_->stop();
    pendingTrigger_.clear();
    pendingExplicit_ = false;
    sessionArmed_ = false;
    inFlight_ = {};

    if( previewLoader_ != nullptr )
        previewLoader_->cancel();
    if( !detail_.isNull() && detail_->isVisible() )
        detail_->hide();

    if( popup_ != nullptr && popup_->isVisible() )
        popup_->hide();

    // 팝업이 없는 동안 모든 키 이벤트를 훑을 이유가 없다.
    qApp->removeEventFilter( this );
}

void CompletionCoordinator::requestExplicit()
{
    if( isMarkdownEditor( activeView_ ) )
    {
        hidePopup();
        return;
    }

    // 사용자가 직접 부른 것이다. Esc 로 눌러 둔 것을 푼다.
    dismissedPathPrefix_ = QString{};
    debounce_->stop();
    trigger( QString{}, true );
}

// ── 트리거 ────────────────────────────────────────────────

void CompletionCoordinator::onCharAdded( const int character )
{
    if( activeView_.isNull() || sender() != activeView_ )
        return;
    if( isMarkdownEditor( activeView_ ) )
    {
        hidePopup();
        return;
    }

    // 이미 떠 있으면 여기서는 아무것도 하지 않는다. 갱신은 onTextEdited 가
    // 한 곳에서 맡는다 — 삭제도 그쪽으로 오므로 규칙이 하나로 남는다.
    if( isPopupVisible() )
        return;

    // 자동 제안을 껐으면 글자 입력으로는 열지 않는다. 명시 호출( requestExplicit )은 그대로 된다.
    if( !autoTrigger_ )
        return;

    if( !isTriggerCharacter( character ) && !shouldRearmForPath() )
        return;

    pendingTrigger_ = QString( QChar( character ) );
    pendingExplicit_ = false;
    debounce_->start();
}

bool CompletionCoordinator::shouldRearmForPath()
{
    if( activeView_.isNull() )
        return false;

    const rstcomplete::Context context = contextAtCaret();
    if( rstpath::slotFor( context ) == nullptr )
    {
        dismissedPathPrefix_ = QString{};
        return false;
    }

    // Esc 로 닫았으면 같은 자리에서 계속 치는 동안에는 다시 열지 않는다.
    // 지우거나 다른 줄로 가면 다시 열린다.
    if( !dismissedPathPrefix_.isNull() && dismissedPathLine_ == activeView_->caretLine()
        && context.prefix.startsWith( dismissedPathPrefix_ ) )
    {
        return false;
    }

    dismissedPathPrefix_ = QString{};
    return true;
}

void CompletionCoordinator::onTextEdited()
{
    if( activeView_.isNull() || sender() != activeView_ )
        return;
    if( isMarkdownEditor( activeView_ ) )
    {
        hidePopup();
        return;
    }
    if( !isPopupVisible() && !sessionArmed_ )
        return;
    if( refreshQueued_ )
        return;

    // Scintilla 는 텍스트를 고친 통지를 캐럿이 옮겨지기 전에 낼 수 있다.
    // 한 바퀴 돌린 뒤에 읽으면 삽입이든 삭제든 자리가 확정돼 있다.
    refreshQueued_ = true;
    QTimer::singleShot( 0, this, &CompletionCoordinator::refreshShownContext );
}

void CompletionCoordinator::refreshShownContext()
{
    refreshQueued_ = false;
    if( popup_.isNull() || activeView_.isNull() )
        return;

    const bool visible = isPopupVisible();
    if( !visible && !sessionArmed_ )
        return;

    const int line = activeView_->caretLine();
    if( !visible && line != sessionLine_ )
    {
        sessionArmed_ = false;   // 다른 줄로 갔다. 이 완성 세션은 끝났다
        return;
    }

    const rstcomplete::Context context = contextAtCaret();
    if( context.kind == rstcomplete::ContextKind::None )
    {
        // ".. image:" 처럼 지우는 도중의 한 순간은 아무 컨텍스트도 아니다.
        // 여기서 세션까지 끝내면 그다음 삭제로 ".. image" 가 돼도 다시
        // 열리지 않는다 — 삽입만 통지를 내기 때문이다. 목록만 감추고
        // 같은 줄을 고치는 동안에는 자리를 지킨다.
        hidePopup();
        sessionArmed_ = true;
        sessionLine_ = line;
        return;
    }

    // 같은 자리를 계속 치고 있는가. 종류나 소유 directive 가 바뀌었으면
    // 후보 집합 자체가 다르므로 필터만 좁혀서는 안 된다.
    const bool sameSlot = visible && context.kind == shownContext_.kind
                       && context.pathSite == shownContext_.pathSite
                       && context.directiveName == shownContext_.directiveName
                       && context.optionName == shownContext_.optionName;
    shownContext_ = context;

    if( !visible )
    {
        // 잠깐 감춰 두었던 세션이 되살아났다. 후보를 새로 만들어 다시 띄운다.
        trigger( QString{}, true );
        return;
    }

    // 경로는 '/' 하나로 디렉터리가 바뀌어 후보 집합이 달라진다. 필터만
    // 좁히면 슬래시를 친 순간 목록이 통째로 죽는다. 디렉터리 나열은
    // 캐시를 타므로 글자마다 다시 만들어도 싸다.
    if( rstpath::slotFor( context ) != nullptr )
    {
        recollectPathItems();
        return;
    }

    if( !sameSlot )
    {
        // 예: ".. image:: " 를 ".. image" 로 지웠다. 경로 목록을 directive
        // 목록으로 갈아야 한다. 종류가 바뀔 때만이므로 LSP 왕복도 한 번뿐이다.
        trigger( QString{}, true );
        return;
    }

    // 같은 자리. 매 글자 LSP 왕복은 낭비이므로 필터만 좁힌다.
    popup_->updateFilter( context.filterPrefix );
}

void CompletionCoordinator::flushTrigger()
{
    const QString triggerCharacter = pendingTrigger_;
    const bool explicitInvoke = pendingExplicit_;
    pendingTrigger_.clear();
    pendingExplicit_ = false;

    trigger( triggerCharacter, explicitInvoke );
}

void CompletionCoordinator::trigger( const QString& triggerCharacter, const bool explicitInvoke )
{
    if( activeView_.isNull() || popup_.isNull() || isMarkdownEditor( activeView_ ) )
    {
        hidePopup();
        return;
    }

    const rstcomplete::Context context = contextAtCaret();
    if( context.kind == rstcomplete::ContextKind::None )
    {
        // Ctrl+Space 를 눌렀는데 완성할 것이 없으면 조용히 넘어간다.
        hidePopup();
        return;
    }

    shownContext_ = context;

    // 접두를 뗀 전체 후보를 팝업에 주고 걸러내기는 팝업의 퍼지 매칭에 맡긴다.
    // 그래야 "cb" 로 "code-block" 을 고를 수 있다.
    rstcomplete::Context broad = context;
    broad.prefix.clear();
    const QVector< rstcomplete::Item > offline =
        rstcomplete::finalizeItems( rstcomplete::candidatesFor( broad ) );
    offlineItems_ = toDisplayList( offline );
    // 오프라인 표가 비워 두는 롤 대상(:term: 등)은 워크스페이스 인덱스가 채운다.
    offlineItems_ += localCandidatesFor( context );

    if( !offlineItems_.isEmpty() )
    {
        popup_->setItems( offlineItems_ );
        popup_->updateFilter( context.filterPrefix );
        showPopupAtCaret();
    }
    else if( !explicitInvoke )
    {
        // 오프라인 후보가 없는 컨텍스트(:ref: 대상, 경로)는 LSP 응답을 기다린다.
        popup_->hide();
    }

    // 치환은 묻지 않는다. Esbonio 에는 `|` 자리를 아는 기능이 없어서, 물으면
    // 그 자리와 무관한 목록(대개 directive 전체)이 돌아와 **우리 후보 위에
    // 앉는다** — applyLspItems 는 LSP 를 우선으로 놓기 때문이다.
    if( context.kind == rstcomplete::ContextKind::Substitution )
    {
        inFlight_ = {};   // 앞선 요청의 늦은 응답을 버린다
        return;
    }

    askLsp( triggerCharacter );
}

void CompletionCoordinator::askLsp( const QString& triggerCharacter )
{
    const QString path = editorPath();
    if( activeProjectId_.isEmpty() || path.isEmpty() || activeView_.isNull() )
        return;

    inFlight_ = {};
    inFlight_.projectId = activeProjectId_;
    inFlight_.path = path;
    inFlight_.line = activeView_->caretLine();
    inFlight_.column = activeView_->caretColumn();
    // **언제나 Invoked 로 보낸다.** Esbonio 의 _trigger_characters_match()
    // (server/feature.py:207) 는 triggerKind 가 TriggerCharacter 면 그 문자를
    // **자기 트리거에 등록한** 기능만 돌리고, Invoked 면 전부 돌린다.
    // 그래서 ".. " 를 쳐서 저절로 뜬 목록과 같은 자리에서 Ctrl+Space 를 눌러
    // 부른 목록이 서로 달랐다. 등록 문자 집합은 기능마다 다르므로 클라이언트가
    // 맞힐 수 있는 값이 아니다.
    Q_UNUSED( triggerCharacter );
    inFlight_.triggerCharacter = QString{};

    // 받는 쪽이 registerRequestId() 로 id 를 돌려준다 (직접 연결이라 동기적이다).
    emit lspCompletionRequested( inFlight_.path, inFlight_.line, inFlight_.column,
                                inFlight_.triggerCharacter );
}

void CompletionCoordinator::registerRequestId( const int requestId )
{
    if( requestId <= 0 )
    {
        inFlight_ = {};
        return;
    }
    inFlight_.requestId = requestId;
}

// ── LSP 응답 ──────────────────────────────────────────────

void CompletionCoordinator::applyLspItems( const QString& projectId, const int requestId,
                                           const QList< LspCompletionItem >& items )
{
    if( requestId <= 0 || requestId != inFlight_.requestId || projectId != inFlight_.projectId )
        return;   // 이미 지나간 요청의 응답
    if( popup_.isNull() )
        return;

    if( items.isEmpty() )
    {
        // Esbonio 는 내부 Sphinx 빌드 전에는 후보를 만들지 못한다. 빌드가
        // 끝나면 한 번만 다시 묻는다. 오프라인 팝업은 그대로 둔다.
        if( !warmProjects_.contains( projectId ) && !inFlight_.retried )
        {
            retryAfterBuild_.insert( projectId );
            emit logMessage( tr( "LSP 완성 결과가 비어 있습니다 [%1]. 빌드 후 다시 시도합니다." )
                                .arg( projectId ) );
        }
        return;
    }

    warmProjects_.insert( projectId );
    retryAfterBuild_.remove( projectId );

    QVector< rstcomplete::Item > converted;
    converted.reserve( items.size() );
    for( const LspCompletionItem& item : items )
        converted.push_back( fromLsp( item ) );

    // 경로 컨텍스트에서는 파일 이름을 directive/role 어휘로 먹이지 않는다.
    // 렉서의 3-state 캐시가 파일 이름을 아는 directive 로 착각한다.
    const rstpath::Slot* pathSlot = rstpath::slotFor( shownContext_ );
    if( pathSlot == nullptr )
        harvestVocabulary( items );

    const QString lineText = activeView_.isNull() ? QString{}
                                                  : activeView_->lineText( inFlight_.line );
    converted = rstcomplete::normalizeLspItems( std::move( converted ), lineText, inFlight_.column );

    if( pathSlot != nullptr )
    {
        // 경로에서는 **우리 후보가 우선**이다. Esbonio 는 4종 directive 만,
        // 확장자 필터 없이, 내부 Sphinx 빌드가 끝난 뒤에야 답한다. 그리고
        // 그 항목은 마지막 세그먼트뿐이라 우리 형태로 고쳐야 디렉터리가
        // 날아가지 않는다. 그렇게 맞춰 놓아야 중복 제거도 옳게 된다.
        QList< CompletionDisplayItem > display = offlineItems_;
        QSet< QString > seen;
        seen.reserve( display.size() + converted.size() );
        for( const CompletionDisplayItem& item : std::as_const( display ) )
            seen.insert( item.insertText );

        const QVector< rstcomplete::Item > rebased = rstpath::rebaseLspPathItems(
            std::move( converted ), pathQueryFor( shownContext_ ) );
        for( const rstcomplete::Item& item : rebased )
        {
            if( item.insertText.isEmpty() || seen.contains( item.insertText ) )
                continue;
            seen.insert( item.insertText );
            display.push_back(
                { item.label, item.insertText, item.detail, item.kind, item.label, 0 } );
        }

        popup_->setItems( display );
        popup_->updateFilter( shownContext_.filterPrefix );
        showOrRefreshPopup();
        return;
    }

    // LSP 가 우선, 오프라인 표는 빈 자리만 메운다.
    QVector< rstcomplete::Item > offline;
    offline.reserve( offlineItems_.size() );
    for( const CompletionDisplayItem& item : offlineItems_ )
        offline.push_back( { item.label, item.insertText, item.detail, item.kind } );

    const QVector< rstcomplete::Item > merged =
        rstcomplete::finalizeItems( rstcomplete::mergeItems( std::move( converted ), offline ) );

    popup_->setItems( toDisplayList( merged ) );
    popup_->updateFilter( shownContext_.filterPrefix );
    showOrRefreshPopup();
}

void CompletionCoordinator::notifyBuildComplete( const QString& projectId )
{
    if( !retryAfterBuild_.remove( projectId ) )
        return;
    if( projectId != activeProjectId_ || !isPopupVisible() )
        return;   // 사용자가 이미 딴 데로 갔다. 갑자기 팝업을 띄우지 않는다.

    emit logMessage( tr( "빌드 완료 후 LSP 완성을 다시 요청합니다 [%1]." ).arg( projectId ) );
    QTimer::singleShot( 250, this,
                       [ this, projectId ]
                       {
                           if( projectId != activeProjectId_ || !isPopupVisible() )
                               return;
                           askLsp( QString{} );
                           inFlight_.retried = true;
                       } );
}

void CompletionCoordinator::harvestVocabulary( const QList< LspCompletionItem >& items )
{
    // 분류 규칙은 렉서의 캐시가 이미 갖고 있다. 여기서 다시 구현하면
    // 두 곳이 조용히 어긋난다. 스크래치 캐시에 먹여 결과만 꺼낸다.
    std::vector< syntax::CompletionEntry > entries;
    entries.reserve( items.size() );
    for( const LspCompletionItem& item : items )
    {
        entries.push_back( { item.label.toStdString(), item.insertText.toStdString(),
                            item.detail.toStdString() } );
    }

    syntax::RstMetadataCache scratch;
    scratch.updateFromCompletion( entries );
    if( !scratch.directivesPopulated && !scratch.rolesPopulated )
        return;

    QStringList directives;
    directives.reserve( static_cast< qsizetype >( scratch.directives.size() ) );
    for( const std::string& name : scratch.directives )
        directives << QString::fromStdString( name );

    QStringList roles;
    roles.reserve( static_cast< qsizetype >( scratch.roles.size() ) );
    for( const std::string& name : scratch.roles )
        roles << QString::fromStdString( name );

    emit vocabularyHarvested( directives, roles );
}

// ── 삽입 ──────────────────────────────────────────────────

void CompletionCoordinator::insertCompletion( const QString& insertText )
{
    if( activeView_.isNull() || insertText.isEmpty() )
        return;

    // 팝업을 띄운 뒤로 글자를 더 쳤을 수 있으므로 지울 길이를 지금 다시 잰다.
    const rstcomplete::Context now = contextAtCaret();
    const int replaceLength = now.kind == shownContext_.kind ? now.replaceLength
                                                             : shownContext_.replaceLength;

    activeView_->replaceRangeAtCursor( qMax( 0, replaceLength ), insertText );

    // 디렉터리를 골랐으면 그 안을 바로 보여 준다 (C++ include 자동완성의 감각).
    // 프로그램이 넣은 글자는 sigCharAdded 를 내지 않으므로 저절로는 안 열린다.
    const bool steppedIntoDirectory =
        now.kind == rstcomplete::ContextKind::Path && insertText.endsWith( QLatin1Char( '/' ) );
    hidePopup();
    if( steppedIntoDirectory )
        QTimer::singleShot( 0, this, [ this ] { requestExplicit(); } );
}

// ── 컨텍스트 ──────────────────────────────────────────────

rstcomplete::Context CompletionCoordinator::contextAtCaret() const
{
    if( activeView_.isNull() )
        return {};

    const int line = activeView_->caretLine();
    return rstcomplete::detectContext( activeView_->lineText( line ), activeView_->caretColumn(),
                                      previousLinesAtCaret() );
}

QStringList CompletionCoordinator::previousLinesAtCaret() const
{
    if( activeView_.isNull() )
        return {};

    const int current = activeView_->caretLine();
    const int caretIndent = rstline::leadingSpaceCount( activeView_->lineText( current ) );

    // 창 크기를 여기서 정하지 않는다. 블록 경계까지 거슬러 올라가는 규칙은
    // rstcomplete 가 갖고 있고 단위 테스트도 그쪽에 붙는다.
    return rstcomplete::collectPreviousLines(
        current, caretIndent,
        [ this ]( const int line ) -> std::optional< QString > {
            if( activeView_.isNull() || line < 1 )
                return std::nullopt;
            return activeView_->lineText( line );
        } );
}

QString CompletionCoordinator::editorPath() const
{
    return activeView_.isNull() ? QString{} : activeView_->currentFilePath();
}

// ── 키 입력 ───────────────────────────────────────────────

bool CompletionCoordinator::eventFilter( QObject* watched, QEvent* event )
{
    if( !isPopupVisible() )
        return QObject::eventFilter( watched, event );

    if( event->type() == QEvent::KeyPress )
    {
        // 편집기(또는 그 안의 Scintilla 위젯)가 받은 키만 가로챈다.
        // 팝업은 포커스를 갖지 않으므로 키는 언제나 편집기로 간다.
        auto* widget = qobject_cast< QWidget* >( watched );
        const bool insideEditor = widget != nullptr && !activeView_.isNull() && activeView_->isAncestorOf( widget );
        auto* keyEvent = static_cast< QKeyEvent* >( event );
        if( insideEditor && keyEvent->key() == Qt::Key_Escape
            && rstpath::slotFor( shownContext_ ) != nullptr )
        {
            // 같은 자리에서 계속 치는 동안 다시 열리지 않게 눌러 둔다.
            dismissedPathPrefix_ = shownContext_.prefix.isNull() ? QString( QLatin1String( "" ) )
                                                                : shownContext_.prefix;
            dismissedPathLine_ = activeView_.isNull() ? 0 : activeView_->caretLine();
        }
        if( insideEditor && popup_->handleKeyPress( keyEvent ) )
            return true;
    }
    else if( event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate )
    {
        auto* widget = qobject_cast< QWidget* >( watched );
        if( widget != nullptr && !activeView_.isNull() && activeView_->isAncestorOf( widget ) )
        {
            hidePopup();
        }
    }
    else if( event->type() == QEvent::Move || event->type() == QEvent::Resize )
    {
        // 창이 움직이면 팝업만 제자리에 남아 떠다닌다.
        if( !activeView_.isNull() && watched == activeView_->window() )
            hidePopup();
    }

    return QObject::eventFilter( watched, event );
}

}  // namespace hc::rst
