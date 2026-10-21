#include "stdafx.h"
#include "fileSetMgr.hpp"

void CFileSetMgr::Refresh()
{
    mapNameToFileSet.clear();
    for (const auto& Value : hc::settings::AppSettings().Read(hc::settings::Section::FileSets).toArray()) {
        const auto Record = Value.toObject(); TyFileSet Item;
        Item.Name = Record["name"].toString(); if (Item.Name.isEmpty()) continue;
        Item.Flags = static_cast<unsigned>(Record["flags"].toInteger());
        Item.Filters = hc::settings::Strings(Record["filters"]); Item.VecExtFilters = hc::settings::Strings(Record["extensions"]);
        Item.Attributes = static_cast<unsigned>(Record["attributes"].toInteger()); Item.Size = Record["size"].toInteger();
        mapNameToFileSet[Item.Name] = Item;
    }
}

void CFileSetMgr::SaveSettings()
{
    const auto Previous = hc::settings::AppSettings().Read(hc::settings::Section::FileSets).toArray();
    QJsonArray Items;
    for (const auto& Item : mapNameToFileSet) {
        QJsonObject Record;
        for (const auto& Value : Previous) if (Value.toObject()["name"] == Item.Name) { Record = Value.toObject(); break; }
        Record["name"] = Item.Name; Record["flags"] = qint64(Item.Flags);
        Record["filters"] = QJsonArray::fromStringList(Item.Filters); Record["extensions"] = QJsonArray::fromStringList(Item.VecExtFilters);
        Record["attributes"] = qint64(Item.Attributes); Record["size"] = qint64(Item.Size); Items.append(Record);
    }
    hc::settings::AppSettings().Put(hc::settings::Section::FileSets, Items);
}

QVector<QString> CFileSetMgr::GetNames() const
{
    return mapNameToFileSet.keys();
}

void CFileSetMgr::SetFileSet( const TyFileSet& FileSet )
{
    Q_ASSERT( FileSet.Name.isEmpty() == false );
    if( FileSet.Name.isEmpty() == true )
        return;

    mapNameToFileSet[ FileSet.Name ] = FileSet;
    SaveSettings();
}

TyFileSet CFileSetMgr::GetFileSet( const QString& FileSetName ) const
{
    if( mapNameToFileSet.contains( FileSetName ) == false )
        return {};

    return mapNameToFileSet[ FileSetName ];
}

void CFileSetMgr::RemoveFileSet( const QString& FileSetName )
{
    if( mapNameToFileSet.contains( FileSetName ) == false )
        return;

    mapNameToFileSet.remove( FileSetName );
    SaveSettings();
}
