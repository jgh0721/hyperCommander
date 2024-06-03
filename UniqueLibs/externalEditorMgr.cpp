#include "stdafx.h"
#include "externalEditorMgr.hpp"

#include "UIs/dlgExternalConfigure.hpp"

#include "cmnTypeDefs_Opts.hpp"

////////////////////////////////////////////////////////////////////////////////
///

void CExternalEditorMgr::Refresh()
{
    StSettings->beginGroup( OPT_SEC_EDITOR );
    const auto Count = StSettings->value( "Count" ).toUInt();

    QVector< TyExternalEditor > VecItems;

    for( uint idx = 1; idx <= Count; ++idx )
    {
        const auto Name     = QString( "Name%1" ).arg( idx );
        const auto Icon     = QString( "Icon%1" ).arg( idx );
        const auto Program  = QString( "Program%1" ).arg( idx );
        const auto CMDLine  = QString( "CMDLine%1" ).arg( idx );
        const auto Detect   = QString( "Detect%1" ).arg( idx );
        const auto Exts     = QString( "Exts%1" ).arg( idx );

        if( StSettings->contains( Name ) == false )
            continue;

        if( StSettings->contains( Program ) == false )
            continue;

        if( StSettings->contains( Detect ) == false && StSettings->contains( Exts ) == false )
            continue;

        TyExternalEditor Item;

        Item.Name       = StSettings->value( QString( "Name%1" ).arg( idx ) ).toString();
        Item.FilePath   = StSettings->value( QString( "Program%1" ).arg( idx ) ).toString();
        Item.IconPath   = StSettings->value( QString( "Icon%1" ).arg( idx ) ).toString();
        Item.Icon       = QIcon( Item.IconPath );

        if( Item.Icon.isNull() == true )
        {
            if( QFile::exists( Item.FilePath ) == true )
                Item.Icon = QIcon( Item.FilePath );
        }

        Item.CMDLine    = StSettings->value( QString( "CMDLine%1" ).arg( idx ) ).toString();
        Item.Detect     = StSettings->value( QString( "Detect%1" ).arg( idx ) ).toString();
        for( const auto& Ext : StSettings->value( QString( "Exts%1" ).arg( idx ) ).toString().split( "|", Qt::SkipEmptyParts ).toList() )
            Item.VecExtensions.push_back( Ext );

        if( ( Item.Name.isEmpty() == true || Item.FilePath.isEmpty() == true ) || 
            ( Item.Detect.isEmpty() == true && Item.VecExtensions.isEmpty() == true ) )
            continue;

        VecItems.push_back( Item );
    }

    StSettings->endGroup();

    qSwap( vecExternalItems, VecItems );
}

QVector<TyExternalEditor> CExternalEditorMgr::RetrieveSettings() const
{
    return vecExternalItems;
}

void CExternalEditorMgr::ConstructExternalMenu( QMenu* Menu, const QString& FileFullPath )
{
    const auto& Ext = FileFullPath.section( '.', -1, -1,  QString::SectionFlag::SectionDefault ).toLower();

    for( const auto& Item : vecExternalItems )
    {
        bool IsContains = false;

        for( const auto& ProgExt : Item.VecExtensions )
        {
            if( ProgExt.contains( "*" ) == true )
            {
                IsContains = QRegularExpression::fromWildcard( ProgExt ).match( Ext ).hasMatch();
            }
            else
            {
                IsContains = ProgExt.compare( Ext, Qt::CaseInsensitive ) == 0;
            }

            if( IsContains == true )
                break;
        }

        if( IsContains == false )
        {
            // TODO: Definition 적용
        }

        if( IsContains == false )
            continue;

        const auto ac = new QAction;
        ac->setText( Item.Name );
        ac->setIcon( Item.Icon );
        ac->setData( QVariant::fromValue( Item ) );
        
        Menu->addAction( ac );
    }

    Menu->addSeparator();
    /// 환경설정 항목 추가

    const auto acConfigure = new QAction;
    acConfigure->setText( tr( "환경설정" ) );
    connect( acConfigure, &QAction::triggered, this, &CExternalEditorMgr::ac_configure );

    Menu->addAction( acConfigure );

}

void CExternalEditorMgr::AddItem( const TyExternalEditor& Item )
{
    vecExternalItems.push_back( Item );

    saveSettings();
    Refresh();
}

void CExternalEditorMgr::RemoveItem( int Index )
{
    if( Index < 0 || Index >= vecExternalItems.size() )
        return;

    vecExternalItems.remove( Index );

    saveSettings();
    Refresh();
}

void CExternalEditorMgr::ModifyItem( int Index, const TyExternalEditor & Item )
{
    if( Index < 0 || Index >= vecExternalItems.size() )
        return;

    vecExternalItems[ Index ] = Item;

    saveSettings();
    Refresh();
}

void CExternalEditorMgr::ac_configure()
{
    CExternalConfigure Configure;
    const auto Result = Configure.exec();
    if( Result == QDialog::Rejected )
        return;

}

void CExternalEditorMgr::saveSettings()
{
    const auto VecItems = RetrieveSettings();
    const auto Count = VecItems.size();

    StSettings->beginGroup( OPT_SEC_EDITOR );

    StSettings->setValue( "Count", Count );

    for( uint idx = 1; idx <= Count; ++idx )
    {
        const auto Name     = QString( "Name%1" ).arg( idx );
        const auto Icon     = QString( "Icon%1" ).arg( idx );
        const auto Program  = QString( "Program%1" ).arg( idx );
        const auto CMDLine  = QString( "CMDLine%1" ).arg( idx );
        const auto Detect   = QString( "Detect%1" ).arg( idx );
        const auto Exts     = QString( "Exts%1" ).arg( idx );

        const auto Item = VecItems.at( idx - 1 );

        StSettings->setValue( Name,     Item.Name );
        StSettings->setValue( Icon,     Item.IconPath );
        StSettings->setValue( Program,  Item.FilePath );
        StSettings->setValue( CMDLine,  Item.CMDLine );
        StSettings->setValue( Detect,   Item.Detect );
        StSettings->setValue( Exts,     Item.VecExtensions.toList().join( '|' ) );
    }

    StSettings->endGroup();
}
