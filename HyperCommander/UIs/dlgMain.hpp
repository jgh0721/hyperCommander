#pragma once

#include "UniqueLibs/commandMgr_Defs.hpp"
#include "UniqueLibs/qtWorkspaceState.hpp"
#include <QPointer>
#include <QAbstractNativeEventFilter>

#include "ui_dlgMain.h"

namespace fm::ui
{
    class CommandLine;
}
class QSHChangeNotify;
class QProcess;
namespace hc::update { class UpdateController; }

class QMainUI : public QMainWindow, public QAbstractNativeEventFilter
{
    Q_OBJECT
  public:
    QMainUI( QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags() );
    hc::workspace::State CaptureWorkspace() const;
    void                 RestoreWorkspace( const hc::workspace::State& State );
    bool                 SaveWorkspace();
    bool                 OpenStartupPaths( const QJsonArray& Paths );

  signals:
    void sigActivePanelChanged( CmpPanel* Panel );

  public slots:

    void OnDriveAdd( const QString& Root );
    void OnDriveRemoved( const QString& Root );
    void OnMediaInserted( const QString& ItemIDDisplayName );
    void OnMediaRemoved( const QString& Root );
    void OnColorSchemeChanged( const TyColorScheme& ColorScheme );

    ////////////////////////////////////////////////////////////////////////////
    /// 명령 핸들러

    DECLARE_HC_COMMAND( TabNew );
    DECLARE_HC_COMMAND( TabPrevious );
    DECLARE_HC_COMMAND( TabNext );
    DECLARE_HC_COMMAND( TabClose );

    DECLARE_HC_COMMAND( ViewerQuick );

    DECLARE_HC_COMMAND( FileCopy );
    DECLARE_HC_COMMAND( FileRename );
    DECLARE_HC_COMMAND( ViewerOpen );
    DECLARE_HC_COMMAND( NavigateOpen );
    DECLARE_HC_COMMAND( FileMkdir );
    DECLARE_HC_COMMAND( FileDelete );
    DECLARE_HC_COMMAND( FileNormalize );
    DECLARE_HC_COMMAND( FileAttributes );

    DECLARE_HC_COMMAND( SelectionInvert );

    DECLARE_HC_COMMAND( FileBatchRename );

    DECLARE_HC_COMMAND( ViewHidden );
    DECLARE_HC_COMMAND( NavigateRefresh );

    DECLARE_HC_COMMAND( NavigateFavorites );
    DECLARE_HC_COMMAND_EX( NavigateDrive );
    DECLARE_HC_COMMAND_EX( NavigatePath );

    DECLARE_HC_COMMAND( PanelSwitch );
    DECLARE_HC_COMMAND( FileContext );
    DECLARE_HC_COMMAND( FileEdit );

  protected slots:

    void OnAddCurrentDirToFavorite();

    void on_acShowMainOpts_triggered( bool checked = false );

    void oo_notifyCurrentDirectory( const QString& CurrentPath );
    void oo_notifyPanelActivated();

  protected:
    void changeEvent( QEvent* Event ) override;
    bool eventFilter( QObject* Object, QEvent* Event ) override;
    bool nativeEventFilter(const QByteArray& Type, void* Message, qintptr* Result) override;
    void closeEvent( QCloseEvent* Event ) override;

  private:
    void             updateWindowFrame();
    void             requestInputFocusRestore();
    void             restoreInputFocus();
    Q_INVOKABLE void initialize();
    void             setActivePanel( int Index );
    void             executeCommandLine( const QString& Command );

    CmpPanel* retrieveSrcPanel() const;
    CmpPanel* retrieveDstPanel() const;

    QSHChangeNotify* shlChangeNotify   = nullptr;
    int              currentPanelIndex = 0; // 0 = Left, 1 = Right

    fm::ui::CommandLine*  commandLine_    = nullptr;
    QProcess*             commandProcess_ = nullptr;
    QPointer<QWidget>     lastInputFocus_;
    bool                  inputFocusRestorePending_ = false;
    Ui::dlgMain           ui;
    hc::workspace::Loaded workspace_;
    bool                  workspaceLoaded_ = false;
    hc::update::UpdateController* update_ = nullptr;
    bool canRestartForUpdate() const;
};
