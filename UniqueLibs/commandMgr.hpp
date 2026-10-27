#pragma once

#include "cmnConcurrent.hpp"

#include "commandMgr_Defs.hpp"
#include "commandUiBridge.hpp"

#include <memory>

namespace Qtitan
{
    class GridViewBase;
}

class CCommandMgr : public QObject
{
    Q_OBJECT
  public:
    CCommandMgr( QObject* parent = nullptr );

    void                  Refresh();
    void                  SaveSettings();
    TyMapShortcutToCMDStr Retrieve() const;
    TyMapShortcutToCMDStr RetrieveForCommand(const QString& Id) const;
    TyHC_COMMAND          GetDefaultCMD( const QString& Command );

    void SetShortcutWithCMD( QKeyCombination Key, const QString& CMD );
    void DelShortcut( QKeyCombination Key, const QString& Id = {} );

    QWidget*         GetMainUI() const;
    void             SetMainUI( QWidget* MainUI );
    CommandUiBridge* Bridge() const
    {
        return bridge_;
    }

    bool ProcessKeyPressEvent( QKeyEvent* KeyEvent, const QModelIndex& CursorIndex );
    bool InvokeCommand( const QString& Id, const QModelIndex& CursorIndex );
    void ProcessFavoriteDir( const QString& Command, const QString& Path );

  signals:
    void ShortcutsChanged();

  public slots:
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
    DECLARE_HC_COMMAND( FileNew );
    DECLARE_HC_COMMAND( ArchiveCreate );
    DECLARE_HC_COMMAND( ArchiveExtract );

    DECLARE_HC_COMMAND( ViewHidden );
    DECLARE_HC_COMMAND( NavigateRefresh );

    DECLARE_HC_COMMAND( NavigateFavorites );
    DECLARE_HC_COMMAND_EX( NavigateDrive );
    DECLARE_HC_COMMAND_EX( NavigatePath );

    DECLARE_HC_COMMAND( PanelSwitch );
    DECLARE_HC_COMMAND( FileContext );
    DECLARE_HC_COMMAND( FileEdit );

  private:
    using TySpMapKeyToCMDStr = std::shared_ptr<TyMapShortcutToCMDStr>;

    TySpMapKeyToCMDStr retrieve() const;

    TySpMapKeyToCMDStr SpMapKeyToCMDText_;

    QWidget*                  MainUI_ = nullptr;
    QPointer<CommandUiBridge> bridge_;
};

using TyStCommandMgr = nsCmn::nsConcurrent::TSingleton<CCommandMgr>;
