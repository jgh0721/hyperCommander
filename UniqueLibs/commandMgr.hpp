#pragma once

#include "cmnConcurrent.hpp"


#include "commandMgr_Defs.hpp"

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

    void                                Refresh();

    QWidget*                            GetMainUI() const;
    void                                SetMainUI( QWidget* MainUI );

    bool                                ProcessKeyPressEvent( QKeyEvent* KeyEvent, const QModelIndex& CursorIndex );

public slots:
    ////////////////////////////////////////////////////////////////////////////
    /// 명령 핸들러

    DECLARE_HC_COMMAND( cm_NewTab );
    DECLARE_HC_COMMAND( cm_PrevTab );
    DECLARE_HC_COMMAND( cm_NextTab );
    DECLARE_HC_COMMAND( cm_CloseTab );

    DECLARE_HC_COMMAND( cm_RenameSingleFile );
    DECLARE_HC_COMMAND( cm_List );
    DECLARE_HC_COMMAND( cm_Return );
    DECLARE_HC_COMMAND( cm_MkDir );

    DECLARE_HC_COMMAND( cm_SelInverse );

    DECLARE_HC_COMMAND( cm_MultiRenameFiles );

    DECLARE_HC_COMMAND( cm_SwitchHidSys );

    DECLARE_HC_COMMAND( cm_SwitchPanel );
    DECLARE_HC_COMMAND( cm_ContextMenu );
    DECLARE_HC_COMMAND( cm_ExternalEditorMenu );

private:
    using TySpMapKeyToCMDStr = std::shared_ptr< TyMapShortcutToCMDStr >;

    TySpMapKeyToCMDStr                  retrieve();

    TySpMapKeyToCMDStr                  SpMapKeyToCMDText;

    QWidget*                            MainUI_ = nullptr;
};

using TyStCommandMgr = nsCmn::nsConcurrent::TSingleton< CCommandMgr >;
