#pragma once

#include <QString>

#include "cmnTypeDefs_Name.hpp"

constexpr long long KILO_BYTE = 1000;
constexpr long long MEGA_BYTE = KILO_BYTE * KILO_BYTE;
constexpr long long GIGA_BYTE = MEGA_BYTE * KILO_BYTE;
constexpr long long TERA_BYTE = GIGA_BYTE * KILO_BYTE;

constexpr long long KIBI_BYTE = 1024;
constexpr long long MEBI_BYTE = KIBI_BYTE * KIBI_BYTE;
constexpr long long GIBI_BYTE = MEBI_BYTE * KIBI_BYTE;
constexpr long long TEBI_BYTE = GIBI_BYTE * KIBI_BYTE;

#include "settings/appSettings.hpp"

////////////////////////////////////////////////////////////////////////////////



////////////////////////////////////////////////////////////////////////////////

class ISettings : public QObject
{
    Q_OBJECT
public:
    bool                            Refresh() { return doRefresh(); }
    bool                            SaveSettings() { return doSaveSettings(); }

protected:
    virtual bool                    doRefresh() { return false; }
    virtual bool                    doSaveSettings() { return false; }

private:

};

struct TyConfiguration
{
    int SizeInHeader = 0;
    int SizeInFooter = 0;
};

Q_DECLARE_METATYPE( TyConfiguration );

////////////////////////////////////////////////////////////////////////////////

struct Node
{
    QString                             Name;
    BOOL                                IsNormalizedByNFD = FALSE;
    QString                             Ext;

    DWORD                               Attiributes = 0;
    DWORD                               Reserved0 = 0;
    int64_t                             Size = 0;

    QDateTime                           Created;
    QDateTime                           Modified;

    ///////////////////////////////////////////////////////////////////////////

    QPixmap                             Icon;
    QVector< QVariant >                 VecContent;
};

Q_DECLARE_METATYPE( Node );

constexpr int FILE_SET_NOR_FILTERS      = 0x1;
constexpr int FILE_SET_NOR_EXT_FILTERS  = 0x2;

constexpr int FILE_SET_EXP_ATTRIBUTE    = 0x100;

constexpr int FILE_SET_EXP_SIZE_EQUAL   = 0x1000;
constexpr int FILE_SET_EXP_SIZE_LESS    = 0x2000;
constexpr int FILE_SET_EXP_SIZE_MORE    = 0x4000;

struct TyFileSet
{
    QString                             Name;
    uint32_t                            Flags = 0;

    // 일반
    QStringList                         Filters;
    QVector<QString>                    VecExtFilters;          // 모두 소문자

    // 확장
    uint32_t                            Attributes = 0;
    int64_t                             Size = 0;
};

Q_DECLARE_METATYPE( TyFileSet );

using TyPrFGWithBG = QPair< QColor, QColor >;
Q_DECLARE_METATYPE( TyPrFGWithBG );

struct TyColorScheme
{
    QString             Name;
    bool                IsDarkMode = false;

    // Main
    QFont               Menu_Font;
    QColor              Menu_FGColor;
    QColor              Menu_BGColor;

    QFont               Dialog_Font;
    QColor              Dialog_FGColor;
    QColor              Dialog_BGColor;

    // FileList
    QFont               FileList_Font;
    QColor              FileList_FGColor;
    QColor              FileList_BGColor;
    QColor              FileList_Selected;
    QColor              FileList_Cursor;

    bool                IsInverseCursor = false;
    bool                IsInverseSelect = false;

    // key = FileSet's Name, value.first = FG, value.second = BG
    QMap< QString, TyPrFGWithBG > MapNameToColors;
};

Q_DECLARE_METATYPE( TyColorScheme );

struct TyExternalEditor
{
    QString             Name;
    QIcon               Icon;
    QString             IconPath;

    QString             FilePath;
    QString             CMDLine;

    QString             Detect;             // force | ext= ...
    QVector<QString>    VecExtensions;      // 확장자 목록, 소문자
};

Q_DECLARE_METATYPE( TyExternalEditor );
