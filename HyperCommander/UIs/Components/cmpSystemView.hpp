#pragma once

// 패널 탭을 덮는 시스템 보기(프로세스 트리·서비스 목록)와 반대 패널 상세 보기.
// 탭의 파일 보기는 숨긴 채 유지하고 이 보기가 페이지를 대신한다(빠른 보기와 같은 방식).

#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <optional>

#include "cmpView.hpp"
#include "UniqueLibs/commandUiConfig.hpp"
#include "UniqueLibs/system/processModel.hpp"
#include "UniqueLibs/system/serviceModel.hpp"

class QLabel;
class QComboBox;
class QCheckBox;
class QToolButton;
class QStackedWidget;
class QTabWidget;
class QFormLayout;
class QTreeWidget;
class QPushButton;
namespace fm::ui { class SegmentedControl; }
namespace Qtitan { class TreeGrid; class GridTreeView; }
namespace hc::system { class ServiceMonitor; class ProcessCollector; }

struct ProcessRef
{
    hc::system::ProcessKey key;
    QString                name, image, commandLine;
    quint32                parentPid = 0;
    bool                   suspended = false, critical = false, opened = false;
    quint32                priorityClass = 0;
};

class CSystemView : public CViewT
{
    Q_OBJECT
public:
    enum class Kind { Processes = 0, Services = 1 };
    explicit CSystemView( Kind Value, QWidget* Parent = nullptr );
    ~CSystemView() override;

    Kind                ViewKind() const { return kind_; }
    hc::ui::InputScope  Scope() const { return kind_ == Kind::Processes ? hc::ui::InputScope::ProcessView : hc::ui::InputScope::ServiceView; }
    static QString      KindKey( Kind Value ) { return Value == Kind::Processes ? QStringLiteral( "processes" ) : QStringLiteral( "services" ); }

    void ApplyColorScheme( const TyColorScheme& ColorScheme ) override;
    void EnsureKeyboardFocusOnView() const override;
    void CloseView() override;
    void SetPanelActive( bool Active );
    // 탭이 보이지 않으면 수집을 멈춘다(설정의 pauseWhenHidden).
    void SetShown( bool Shown );
    void ApplySettings();

    void    SetSearch( const QString& Text );
    QString Search() const;

    QJsonObject CaptureState() const;
    void        RestoreState( const QJsonObject& State );

    QString PathText() const;
    QString SummaryText() const;
    QString StatusLeft() const;
    QString StatusRight() const;

    std::optional<ProcessRef> CurrentProcess() const;
    QVector<ProcessRef>       SelectedProcesses() const;
    QVector<ProcessRef>       TreeOf( const hc::system::ProcessKey& Root ) const;
    QString                   CurrentService() const;
    QStringList               SelectedServices() const;
    const hc::sys::ServiceRow* ServiceRow( const QString& Name ) const;

    const hc::sys::ProcessTreeModel* Processes() const { return processes_; }
    const hc::sys::ServiceListModel* Services() const { return services_; }

    void SelectProcess( quint32 Pid );
    void SelectService( const QString& Name );
    void SelectServiceByPid( quint32 Pid );
    void Refresh();
    void TogglePause();
    bool Paused() const { return paused_; }
    void SetFlat( bool Flat );
    bool Flat() const;
    void SetIncludeDrivers( bool Include );
    bool IncludeDrivers() const { return includeDrivers_; }
    // 바로 가기 메뉴. Global이 비어 있으면 커서 행 근처에 연다.
    void ShowContextMenu( QPoint Global = {} );
    // 커서 행의 화면 위치(하위 메뉴를 띄울 때 쓴다)
    QPoint CursorPoint() const;
    bool   HasDenied() const;
    // 시험용: 표에 보이는 행 수, 커서 행이 펼쳐졌는지
    int    VisibleRowCount() const;
    bool   CursorExpanded() const;

signals:
    void sigCursorChanged();
    void sigSummaryChanged();

protected:
    bool eventFilter( QObject* Watched, QEvent* Event ) override;

private:
    void buildProcessBar( QWidget* Bar );
    void buildServiceBar( QWidget* Bar );
    void configureGrid();
    void applyCursorColors();
    void sample();
    void startMonitor();
    void updateBar();
    void updateNotice();
    bool handleKey( QKeyEvent* Event );
    QModelIndex sourceIndex( const QModelIndex& ViewIndex ) const;
    QModelIndex focusedSource() const;
    ProcessRef  refOf( const hc::sys::ProcessNode& Node ) const;

    Kind                               kind_;
    Qtitan::TreeGrid*                  grid_ = nullptr;
    Qtitan::GridTreeView*              view_ = nullptr;
    hc::sys::ProcessTreeModel*         processes_ = nullptr;
    hc::sys::ProcessFilterProxy*       processProxy_ = nullptr;
    hc::sys::ServiceListModel*         services_ = nullptr;
    hc::sys::ServiceFilterProxy*       serviceProxy_ = nullptr;
    std::shared_ptr<hc::system::ProcessCollector> collector_;
    std::unique_ptr<hc::system::ServiceMonitor>   monitor_;
    QTimer                             timer_;
    QTimer                             servicesTimer_;
    bool                               sampling_ = false;
    // 수집기는 표본 작업자만 만진다. 기준점 초기화는 다음 표본을 시작할 때 GUI 스레드에서 한다.
    bool                               resetPending_ = false;
    bool                               paused_ = false;
    bool                               shown_ = true;
    bool                               panelActive_ = false;
    bool                               includeDrivers_ = false;
    bool                               expandedOnce_ = false;
    hc::sys::Settings                  settings_;
    QLabel*                            summary_ = nullptr;
    QLabel*                            notice_ = nullptr;
    QWidget*                           noticeRow_ = nullptr;
    QWidget*                           searchRow_ = nullptr;
    QLabel*                            searchStatus_ = nullptr;
    QComboBox*                         searchScope_ = nullptr;
    QCheckBox*                         keepAncestors_ = nullptr;
    fm::ui::SegmentedControl*          mode_ = nullptr;
    fm::ui::SegmentedControl*          stateFilter_ = nullptr;
    QComboBox*                         interval_ = nullptr;
    QToolButton*                       pause_ = nullptr;
    QCheckBox*                         drivers_ = nullptr;
    std::shared_ptr<std::atomic_bool>  alive_;
    class TreeBranchStyle*             branchStyle_ = nullptr;
    QStringList                        stateLabels_;
};

// 반대 패널의 상세 보기. 시스템 보기의 커서 행을 따라간다.
class CSystemDetailView : public CViewT
{
    Q_OBJECT
public:
    explicit CSystemDetailView( QWidget* Parent = nullptr );
    void SetSource( CSystemView* Source );
    CSystemView* Source() const { return source_; }
    void Update();
    void CloseView() override;

private:
    void showProcess( const hc::sys::ProcessNode& Node );
    void showService( const QString& Name );
    void clear( const QString& Message );

    QPointer<CSystemView> source_;
    QLabel*               icon_ = nullptr;
    QLabel*               title_ = nullptr;
    QLabel*               subtitle_ = nullptr;
    QLabel*               badges_ = nullptr;
    QTabWidget*           tabs_ = nullptr;
    QFormLayout*          form_ = nullptr;
    QWidget*              graphs_ = nullptr;
    QTreeWidget*          dependencies_ = nullptr;
    QWidget*              actions_ = nullptr;
    QString               shownKey_;
    QString               signature_;
    QString               signatureFor_;
    hc::system::ServiceConfig config_;
    QString               configFor_;
    std::vector<hc::system::ServiceStatusInfo> dependents_;
};
