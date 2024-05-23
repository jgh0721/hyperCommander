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
    void Refresh();

    void CMD_Return( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex );
    void CMD_Space( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex );
    void CMD_TabSwitch( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex );
    void CMD_HidSys( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex );
    void CMD_MultiRename( Qtitan::GridViewBase* View, const QPoint& GlobalPos, const QModelIndex& SrcIndex );

private:
};

using TyStCommandMgr = nsCmn::nsConcurrent::TSingleton< CCommandMgr >;
