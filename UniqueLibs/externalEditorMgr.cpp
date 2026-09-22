#include "stdafx.h"
#include "externalEditorMgr.hpp"

#include "UIs/dlgExternalConfigure.hpp"

#include "cmnTypeDefs_Opts.hpp"

////////////////////////////////////////////////////////////////////////////////
///

void CExternalEditorMgr::Refresh()
{
    vecExternalItems.clear();
    for (const auto& Value : hc::settings::AppSettings().Read(hc::settings::Section::Editors).toArray()) {
        const auto Record = Value.toObject(); TyExternalEditor Item;
        Item.Name = Record["name"].toString(); Item.FilePath = Record["program"].toString(); Item.IconPath = Record["icon"].toString();
        Item.Icon = QIcon(Item.IconPath);
        if (Item.Icon.isNull() && QFile::exists(Item.IconPath)) { QFileIconProvider Icons; Item.Icon = Icons.icon(QFileInfo(Item.IconPath)); }
        Item.CMDLine = Record["arguments"].toString(); Item.Detect = Record["detect"].toString();
        Item.VecExtensions = hc::settings::Strings(Record["extensions"]);
        if (Item.Name.isEmpty() || Item.FilePath.isEmpty() || (Item.Detect.isEmpty() && Item.VecExtensions.isEmpty())) continue;
        vecExternalItems.append(Item);
    }
}

QVector<TyExternalEditor> CExternalEditorMgr::RetrieveSettings() const
{
    return vecExternalItems;
}

void CExternalEditorMgr::ConstructExternalMenu( QMenu* Menu, const QString& FileFullPath )
{
    const auto& Ext = FileFullPath.section( '.', -1, -1,  QString::SectionFlag::SectionDefault ).toLower();

    int idx = 0;
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
        ac->setShortcut( Qt::ControlModifier | Qt::Key_1 | idx++ );
        if( idx >= 9 )
            idx = 0;
        ac->setShortcutContext( Qt::WidgetWithChildrenShortcut );

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
    QJsonArray Items;
    for (const auto& Item : vecExternalItems) Items.append(QJsonObject{{"name",Item.Name},{"program",Item.FilePath},{"icon",Item.IconPath},
        {"arguments",Item.CMDLine},{"detect",Item.Detect},{"extensions",QJsonArray::fromStringList(Item.VecExtensions)}});
    hc::settings::AppSettings().Put(hc::settings::Section::Editors, Items);
}
