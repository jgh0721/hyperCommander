#pragma once

#include "concurrent/cmnSingleton.hpp"
#include "cmnTypeDefs.hpp"

class CFavoriteDirMgr : public ISettings
{
    Q_OBJECT
public:

    struct TyFavoriteDir
    {
        QString                     Name;
        QString                     Command;
        QString                     Path;
    };

    enum TyEnDrivesOn { DRIVES_ON_NONE, DRIVES_ON_TOP, DRIVES_ON_BOTTOM };

public:

    qsizetype                       GetCount() const;
    QVector< QString >              GetNames() const;
    TyFavoriteDir                   GetItem( qsizetype Idx ) const;
    TyFavoriteDir                   GetItem( const QString& Name ) const;
    void                            SetItem( TyFavoriteDir Item );
    void                            RemoveItem( const QString& Name );
    TyEnDrivesOn                    GetDrivesOn() const;
    void                            SetDrivesOn( TyEnDrivesOn DrivesOn );

    void                            ConstructDirMenu( QMenu* Menu );

public slots:
    void                            OnConfigure( bool checked = false );

signals:
    void                            NotifyAppendCurrentPath();

protected:
    bool                            doRefresh() override;
    bool                            doSaveSettings() override;

private:
    void                            createDrives( QMenu* Menu );

    TyEnDrivesOn                    drivesOn_ = DRIVES_ON_NONE;
    QVector< TyFavoriteDir >        vecItems_;
};

using TyStFavoriteDirMgr = nsCmn::nsConcurrent::TSingleton< CFavoriteDirMgr >;
