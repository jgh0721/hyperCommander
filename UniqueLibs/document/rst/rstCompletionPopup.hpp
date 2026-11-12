#pragma once

#include <QFrame>
#include <QPoint>
#include <QList>
#include <QString>
#include <QVector>

class QKeyEvent;
class QListWidget;

namespace hc::rst {

struct CompletionDisplayItem
{
    QString                             label;
    QString                             insertText;
    QString                             detail;
    int                                 kind = 0;        ///< LSP CompletionItemKind
    QString                             filterText;      ///< 비어 있으면 label 로 거른다
    /// 퍼지 점수에 더할 가중치. 공급자가 "이게 더 그럴듯하다" 고 말하는 자리다.
    ///
    /// 팝업은 경로도 프로젝트도 모른다. 경로 후보에서 "지금 보고 있는
    /// 디렉터리" 가 "프로젝트 어딘가" 를 이기게 하려면 판단을 공급자가 해야 한다.
    int                                 scoreBias = 0;
};

/// 편집기 위에 뜨는 자동완성 목록.
///
/// **포커스를 절대 가져가지 않는다.** 캐럿이 편집기에 남아 있어야 계속 타이핑할
/// 수 있기 때문이다. 방향키·Enter·Esc 는 편집기 쪽 이벤트 필터가 가로채
/// handleKeyPress() 로 넘긴다.
class CompletionPopupWidget final : public QFrame
{
    Q_OBJECT

public:
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
    void                                currentItemChanged( const CompletionDisplayItem& item );
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
    void                                resizeToRows();
    [[nodiscard]] QString               currentInsertText() const;
    bool                                selectByInsertText( const QString& insertText );
    void                                selectFirst();
    /// 한 화면에 보이는 행 수. 목록이 그보다 짧으면 목록 길이.
    [[nodiscard]] int                   pageStep() const;
    void                                selectRow( int row );

    QListWidget*                        list_ = nullptr;
    QList< CompletionDisplayItem >      allItems_;
    QString                             prefix_;
    QPoint                              anchor_;
    bool                                hasAnchor_ = false;
    /// 중복 통지를 막는다. 목록을 다시 채우는 것만으로 상세 패널이 깜빡이면 안 된다.
    QString                             lastEmittedInsertText_;
};

}  // namespace hc::rst
