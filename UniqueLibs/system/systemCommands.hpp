#pragma once

// 프로세스·서비스 명령(설계 7.5). 브리지가 QAction을 만들고, 실행·활성 상태는 여기서 정한다.
// 동작은 먼저 현재 권한으로 시도하고, 접근이 거부되면 관리자 도우미 세션으로 다시 보낸다.

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariant>
#include <QVector>

#include <functional>

class CmpPanel;
class CSystemView;
class CSystemDetailView;
class CommandUiBridge;
class QMainWindow;
struct ProcessRef;

namespace hc::sys {

class SystemCommands : public QObject
{
    Q_OBJECT
public:
    using PanelAccessor = std::function<CmpPanel*()>;
    SystemCommands( QMainWindow* Main, CommandUiBridge* Bridge, PanelAccessor Active );

    static bool Handles( const QString& Id );
    // Source가 없으면 활성 패널의 시스템 보기를 대상으로 한다. 처리했으면 true.
    bool Execute( const QString& Id, const QVariant& Argument = {}, QObject* Source = nullptr );
    // 활성 패널이 보이는 시스템 보기 종류(0 프로세스, 1 서비스). 없으면 -1.
    int  ActiveKind() const;
    // Ctrl+Q: 반대 패널의 상세 보기를 열거나 닫는다.
    void ToggleDetail();
    bool CloseDetail();
    bool DetailOpen() const { return !detail_.isNull(); }
    void RefreshState();

private:
    CSystemView* target( QObject* Source ) const;
    CmpPanel*    panelOf( const QObject* Object ) const;
    CmpPanel*    opposite( CmpPanel* Panel ) const;
    CmpPanel*    namedPanel( bool Left ) const;
    void         status( const QString& Text ) const;
    void         report( const QString& Title, const QStringList& Failures, const QString& Success ) const;

    void openView( int Kind );
    void openLocation( CSystemView* View, bool NewTab );
    void revealIn( CmpPanel* Panel, const QString& Path, bool NewTab, bool Focus );
    void selectLater( CSystemView* View, std::function<bool( CSystemView* )> Select, int Tries = 40 );

    void terminate( CSystemView* View, bool Tree, bool Restart );
    void suspend( CSystemView* View );
    void priority( CSystemView* View, const QVariant& Argument );
    void efficiency( CSystemView* View );
    void run( CSystemView* View );
    void copyProcess( CSystemView* View, const QVariant& Argument );
    void processProperties( CSystemView* View );

    void serviceControl( CSystemView* View, const QString& Control );
    void serviceStartType( CSystemView* View, const QVariant& Argument );
    void serviceProperties( CSystemView* View );
    void copyService( CSystemView* View, const QVariant& Argument );
    void startHelper();

    void setFollow( bool Follow, bool Announce = true );
    void followNow();

    QPointer<QMainWindow>       main_;
    CommandUiBridge*            bridge_ = nullptr;
    PanelAccessor               active_;
    QPointer<CSystemDetailView> detail_;
    QPointer<CmpPanel>          detailPanel_;
    bool                        follow_ = false;
    QPointer<CSystemView>       followSource_;
    QMetaObject::Connection     followConnection_;
    QTimer                      followTimer_;
    QString                     followShown_;
};

} // namespace hc::sys
