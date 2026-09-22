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

void CFavoriteDirMgr::RemoveItem( const QString& Name )
{
    for( int idx = 0; idx < vecItems_.size(); ++idx )
    {
        const auto& Item = vecItems_[ idx ];
        if( Item.Name != Name )
            continue;

        vecItems_.removeAt( idx );
        break;
    }

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
        const auto Ac = Menu->addAction( Dir.Name );
        connect( Ac, &QAction::triggered, [Dir]() {
            TyStCommandMgr::GetInstance()->ProcessFavoriteDir( Dir.Command, Dir.Path );
        } );
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
    const auto Favorites = hc::settings::AppSettings().Read(hc::settings::Section::Favorites).toObject();
    const auto Position = Favorites["drivesOn"].toString();
    drivesOn_ = Position == "top" ? DRIVES_ON_TOP : Position == "bottom" ? DRIVES_ON_BOTTOM : static_cast<TyEnDrivesOn>(0);
    vecItems_.clear();
    for (const auto& Value : Favorites["items"].toArray()) {
        const auto Record = Value.toObject();
        if (!Record["name"].isString() || !Record["command"].isString()) continue;
        TyFavoriteDir Item; Item.Name = Record["name"].toString(); Item.Command = Record["command"].toString(); Item.Path = Record["path"].toString();
        vecItems_.append(Item);
    }
    return true;
}

bool CFavoriteDirMgr::doSaveSettings()
{
    QJsonArray Items;
    for (const auto& Item : vecItems_) Items.append(QJsonObject{{"name",Item.Name},{"command",Item.Command},{"path",Item.Path}});
    return hc::settings::AppSettings().Put(hc::settings::Section::Favorites, QJsonObject{{"items",Items},
        {"drivesOn",drivesOn_ == DRIVES_ON_TOP ? "top" : drivesOn_ == DRIVES_ON_BOTTOM ? "bottom" : "none"}});
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
