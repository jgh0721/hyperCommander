#pragma once

#include "cmnConcurrent.hpp"

#include "cmnTypeDefs.hpp"

class CInternalViewerMgr : public QObject
{
    Q_OBJECT
public:
    void                                Refresh();

    /*!
     * @brief 
     * @param Menu 
     * @param FileFullPath 
     * @return 해당 파일에 적용되는 뷰어 수량 
     */
    quint32                             ConstructInternalMenu( QMenu* Menu, const QString& FileFullPath );

    ////////////////////////////////////////////////////////////////////////////
    /// 
    
    static void                         searchViewerFuncs( const QString& PlugIn, TyInternalViewer_TC* TC );
    static void                         cleanupViewerFuncs( TyInternalViewer_TC* TC );

private:

    QVector< TyInternalViewer >         vecInternalItems;
};

using TyStInternalViewerMgr = nsCmn::nsConcurrent::TSingleton<CInternalViewerMgr>;
