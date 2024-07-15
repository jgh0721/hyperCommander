#include "stdafx.h"
#include "solFavoriteDirMgr.hpp"

#include "commandMgr.hpp"
#include "UIs/dlgFavoriteDir.hpp"

DECLARE_CMNLIBSV2_NAMESPACE

///////////////////////////////////////////////////////////////////////////////
///

qsizetype CFavoriteDirMgr::GetCount() const
{
    return vecItems_.count();
}

QVector<QString> CFavoriteDirMgr::GetNames() const
{
    QVector< QString > VecNames;
    for( const auto& Item : vecItems_ )
        VecNames.push_back( Item.Name );
    return VecNames;
}

CFavoriteDirMgr::TyFavoriteDir CFavoriteDirMgr::GetItem( qsizetype Idx ) const
{
    Q_ASSERT( Idx >= 0 && Idx < GetCount() );
    return vecItems_[ Idx ];
}

CFavoriteDirMgr::TyFavoriteDir CFavoriteDirMgr::GetItem( const QString& Name ) const
{
    for( const auto& Item : vecItems_ )
    {
        if( Item.Name.compare( Name, Qt::CaseInsensitive ) != 0 )
            continue;

        return Item;
    }

    return {};
}

void CFavoriteDirMgr::SetItem( TyFavoriteDir Item )
{
    qsizetype Search = -1;
    const auto Names = GetNames();
    for( qsizetype Idx = 0; Idx < Names.count(); ++Idx )
    {
        if( Names[ Idx ].compare( Item.Name, Qt::CaseInsensitive ) != 0 )
            continue;

        Search = Idx;
        break;
    }

    if( Search < 0 )
        vecItems_.push_back( Item );
    else
        vecItems_[ Search ] = Item;

    SaveSettings();
}

CFavoriteDirMgr::TyEnDrivesOn CFavoriteDirMgr::GetDrivesOn() const
{
    return drivesOn_;
}

void CFavoriteDirMgr::SetDrivesOn( TyEnDrivesOn DrivesOn )
{
    drivesOn_ = DrivesOn;
    SaveSettings();
}

void CFavoriteDirMgr::ConstructDirMenu( QMenu* Menu )
{
    Q_ASSERT( Menu != nullptr );
    if( Menu == nullptr )
        return;

    if( drivesOn_ == DRIVES_ON_TOP )
        createDrives( Menu );

    for( const auto& Dir : vecItems_ )
    {
        
    }

    if( drivesOn_ == DRIVES_ON_BOTTOM )
        createDrives( Menu );

    QAction* Ac = nullptr;

    Menu->addSeparator();

    Ac = Menu->addAction( QObject::tr( "현재 폴더 추가" ) );
    connect( Ac, &QAction::triggered, this, &CFavoriteDirMgr::NotifyAppendCurrentPath );
    Ac = Menu->addAction( QObject::tr( "환경설정(&C)" ) );
    connect( Ac, &QAction::triggered, this, &CFavoriteDirMgr::OnConfigure );
}

void CFavoriteDirMgr::OnConfigure( bool checked )
{
    QFavoriteDirConfigure Configure;
    Configure.exec();
    Refresh();
}

bool CFavoriteDirMgr::doRefresh()
{
    QVector< TyFavoriteDir > VecItems;

    StSettings->beginGroup( "Favorites" );
    const auto Count = StSettings->value( "Count" ).toInt();
    auto DrivesOn = static_cast< TyEnDrivesOn >( StSettings->value( "DrivesOn" ).toInt() );
    for( int Idx = 0; Idx < Count; ++Idx )
    {
        TyFavoriteDir Item;
        Item.Name       = StSettings->value( QString( "Menu%1" ).arg( Idx ) ).toString();
        Item.Command    = StSettings->value( QString( "Cmd%1" ).arg( Idx ) ).toString();
        Item.Path       = StSettings->value( QString( "Path%1" ).arg( Idx ) ).toString();
        VecItems.push_back( Item );
    }
    StSettings->endGroup();

    qSwap( VecItems, vecItems_ );
    qSwap( DrivesOn, drivesOn_ );

    return true;
}

bool CFavoriteDirMgr::doSaveSettings()
{
    StSettings->beginGroup( "Favorites" );
    StSettings->setValue( "Count", vecItems_.count() );
    for( qsizetype Idx = 0; Idx < vecItems_.size(); ++Idx )
    {
        const auto& Item = vecItems_[ Idx ];
        StSettings->setValue( QString( "Menu%1" ).arg( Idx ), Item.Name );
        StSettings->setValue( QString( "Cmd%1" ).arg( Idx ), Item.Command );
        StSettings->setValue( QString( "Path%1" ).arg( Idx ), Item.Path );
    }
    StSettings->setValue( "DrivesOn", ( int )drivesOn_ );
    StSettings->endGroup();

    return true;
}

void CFavoriteDirMgr::createDrives( QMenu* Menu )
{
    // TODO: 향후 VolumeListModel 을 통해 획득한다.
    WCHAR wszDrive[ 3 ] = L" :";
    WCHAR wszBuffer[ MAX_PATH ] = { 0, };
    WCHAR* wszC = wszBuffer;

    if( GetLogicalDriveStringsW( MAX_PATH, wszBuffer ) != 0 )
    {
        const auto Parent = Menu->addMenu( tr( "드라이브" ) );

        do
        {
            *wszDrive = *wszC;

            const auto Ac = Parent->addAction( QString( "[%1]" ).arg( QString::fromWCharArray( wszDrive ) ) );
            connect( Ac, &QAction::triggered, [wszDrive]() {
                TyStCommandMgr::GetInstance()->cm_GotoDrive( QModelIndex(), QString::fromWCharArray( wszDrive ) );
            } );

            while( *wszC++ );

        } while( *wszC );
    }
}
