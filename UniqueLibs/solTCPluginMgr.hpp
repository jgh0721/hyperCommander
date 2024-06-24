#pragma once

#include "cmnConcurrent.hpp"

#include "cmnTypeDefs.hpp"

class CTCPlugInWLX
{
public:
    ~CTCPlugInWLX();

    bool                                LoadWLX( const QString& FilePath );
    void                                SetWinId( HWND ListWin );
    void                                CloseWLX();

    static void                         RetrieveWLXFuncs( const QString& PlugIn, TyPlugInWLXFuncs* TC );
    TyPlugInWLXFuncs                    Funcs() const;

private:

    HWND                                wnd_ = nullptr;
    TyPlugInWLX                         wlx_;
};

Q_DECLARE_METATYPE( CTCPlugInWLX );

using TySpWLX = QSharedPointer< CTCPlugInWLX >;

//Q_DECLARE_METATYPE( TySpWLX );

class CTCPlugInMgr : public QObject
{
    Q_OBJECT
public:
    CTCPlugInMgr( QObject* Parent = nullptr );

    void                                Refresh();
    void                                SaveSettings();

    quint32                             GetWLXCount() const;
    QString                             GetWLXFilePath( quint32 Idx ) const;
    QString                             GetWLXDetect( quint32 Idx ) const;
    void                                SetWLXOpts( quint32 Idx, const TyPlugInWLX& WLX );
    void                                DelWLXOpts( quint32 Idx );

    /*!
     * @brief
     * @param Menu
     * @param FileFullPath
     * @return 해당 파일에 적용되는 뷰어 수량
     */
    quint32                             ConstructWLXMenu( QMenu* Menu, const QString& FileFullPath );

private:

    void                                Save_WLX();


    QVector< TyPlugInWLX >              vecWLX;
};

using TyStPlugInMgr = nsCmn::nsConcurrent::TSingleton<CTCPlugInMgr>;
