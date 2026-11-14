#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <functional>
#include <QFont>
#include <QFrame>
#include <QIcon>
#include <QList>
#include <QPoint>
#include <QString>
#include <QVector>

class QKeyEvent;
class QLabel;
class QListWidget;

namespace hc::document {

class CompletionItemDelegate;

struct CompletionDisplayItem
{
    QString                             label;
    QString                             insertText;
    /// 오른쪽 흐린 열. reST 는 출처 · 경로, SQLite 는 형식(INTEGER · PK · FK · 행 수).
    QString                             detail;
    int                                 kind = 0;        ///< LSP CompletionItemKind
    QString                             filterText;      ///< 비어 있으면 label 로 거른다
    /// 퍼지 점수에 더할 가중치. 공급자가 "이게 더 그럴듯하다" 고 말하는 자리다.
    ///
    /// 팝업은 경로도 프로젝트도 모른다. 경로 후보에서 "지금 보고 있는
    /// 디렉터리" 가 "프로젝트 어딘가" 를 이기게 하려면 판단을 공급자가 해야 한다.
    int                                 scoreBias = 0;
    /// 비어 있지 않으면 종류 글자 배지 대신 그린다(SQLite: 테이블 · 뷰 · CTE · 열 · 기본 키 · 외래 키).
    QIcon                               icon;
    /// 목록 아래 설명 줄(setFooterVisible). 강조된 항목의 것을 보인다.
    QString                             description;
};

/// 편집기 위에 뜨는 자동완성 목록(reST 편집기 · SQLite 쿼리 편집기 공용, SQLite 설계 261113 5.2).
///
/// **포커스를 절대 가져가지 않는다.** 캐럿이 편집기에 남아 있어야 계속 타이핑할
/// 수 있기 때문이다. 방향키·Enter·Esc 는 편집기 쪽 이벤트 필터가 가로채
/// handleKeyPress() 로 넘긴다.
class CompletionPopupWidget final : public QFrame
{
    Q_OBJECT

public:
    /// 접두로 후보를 고르는 규칙. 점수가 높을수록 앞, MatchedPositions 는 굵게 · 밑줄 칠 글자.
    using Matcher = std::function< bool( const QString& Pattern, const QString& Candidate, int* Score, QVector< int >* MatchedPositions ) >;

    explicit CompletionPopupWidget( QWidget* parent = nullptr );

    void                                setItems( const QList< CompletionDisplayItem >& items );
    /// 이미 입력한 접두로 목록을 좁힌다. 남는 것이 없으면 스스로 숨는다.
    void                                updateFilter( const QString& prefix );
    /// 화면 밖으로 나가지 않도록 보정한 뒤 표시한다.
    void                                showAt( const QPoint& globalTopLeft );
    /// 이미 떠 있는 목록을 **그 자리에** 둔 채 크기만 다시 잡는다.
    ///
    /// showAt() 을 다시 부르면 그동안 움직인 캐럿을 따라 목록이 옆으로 튄다.
    /// LSP 응답은 200~500ms 뒤에 오는데 경로는 타이핑이 길어 매우 잘 보인다.
    void                                refreshGeometry();

    /// 거르기 규칙. 기본은 흩어진 글자 일치(completion::FuzzyMatch, reST). SQLite 는 접두어 · 머리 글자(InitialsMatch).
    void                                setMatcher( Matcher matcher );
    /// 오른쪽 열 글꼴. 기본은 목록 글꼴보다 1pt 작게. SQLite 는 형식을 고정폭으로 보인다.
    void                                setDetailFont( const QFont& font );
    /// 목록 아래 설명 줄(강조된 항목의 description). 기본은 끔(reST 는 옆의 상세 창을 쓴다).
    void                                setFooterVisible( bool visible );

    [[nodiscard]] bool                  isActive() const;
    [[nodiscard]] int                   visibleCount() const;
    /// 지금 강조된 항목. 목록이 비었으면 label 이 빈 항목.
    [[nodiscard]] CompletionDisplayItem currentItem() const;

    void                                selectNext();
    void                                selectPrevious();
    /// 한 화면씩. 목록 밖으로는 나가지 않는다 (한 칸 이동과 달리 순환하면
    /// 어디로 갔는지 알 수 없다).
    void                                selectNextPage();
    void                                selectPreviousPage();
    /// 선택된 항목을 확정한다. 확정할 것이 있었으면 true.
    bool                                acceptCurrent();
    /// 편집기가 받은 키를 팝업이 대신 처리한다. 처리했으면 true.
    bool                                handleKeyPress( QKeyEvent* event );

signals:
    void                                itemSelected( const QString& insertText );
    /// 강조된 항목이 바뀌었다. 상세 패널을 갱신하는 데 쓴다.
    void                                currentItemChanged( const hc::document::CompletionDisplayItem& item );
    /// 목록이 화면에서 사라졌다 (Esc, 항목 확정, 필터 결과 없음, 부모 창 파괴).
    ///
    /// 목록에 딸린 상세 패널은 **별도의 최상위 창**이라 목록이 숨어도 저절로
    /// 따라 사라지지 않는다. 숨는 경로가 여러 곳이므로 개별 경로마다 상세
    /// 패널을 닫는 대신 여기 한 곳으로 모은다.
    void                                popupHidden();

protected:
    void                                hideEvent( QHideEvent* event ) override;

private:
    void                                rebuild();
    /// 저장해 둔 앵커로 위치·크기를 다시 계산한다.
    void                                applyGeometry();
    /// 지금 강조된 항목이 마지막으로 알린 것과 다를 때만 currentItemChanged 를 낸다.
    void                                emitCurrentIfChanged();
    void                                refreshFooter();
    void                                resizeToRows();
    [[nodiscard]] QString               currentInsertText() const;
    bool                                selectByInsertText( const QString& insertText );
    void                                selectFirst();
    /// 한 화면에 보이는 행 수. 목록이 그보다 짧으면 목록 길이.
    [[nodiscard]] int                   pageStep() const;
    void                                selectRow( int row );

    QListWidget*                        list_ = nullptr;
    QLabel*                             footer_ = nullptr;
    CompletionItemDelegate*             delegate_ = nullptr;
    QList< CompletionDisplayItem >      allItems_;
    Matcher                             matcher_;
    QString                             prefix_;
    QPoint                              anchor_;
    bool                                hasAnchor_ = false;
    bool                                footerVisible_ = false;
    /// 중복 통지를 막는다. 목록을 다시 채우는 것만으로 상세 패널이 깜빡이면 안 된다.
    QString                             lastEmittedInsertText_;
};

}  // namespace hc::document
