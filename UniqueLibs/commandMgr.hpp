#pragma once

#include "cmnConcurrent.hpp"


#include "commandMgr_Defs.hpp"

namespace Qtitan
{
    class GridViewBase;
}

class CCommandMgr : public QObject
{
    Q_OBJECT
public:
    void                                Refresh();

    QWidget*                            GetMainUI() const;
    void                                SetMainUI( QWidget* MainUI );

    bool                                ProcessKeyPressEvent( QKeyEvent* KeyEvent );

    void CMD_Return( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex );
    void CMD_Space( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex );
    void CMD_TabSwitch( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex );
    void CMD_HidSys( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex );
    void CMD_MultiRename( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex );

private:

    QHash< QKeyCombination, QString >   MapKeyToCMDText;

    QWidget*                            MainUI_ = nullptr;
};

using TyStCommandMgr = nsCmn::nsConcurrent::TSingleton< CCommandMgr >;
