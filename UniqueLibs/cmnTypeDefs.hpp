#pragma once

#include <QString>
#include <QSet>

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

struct TyColorScheme
{
    bool                IsDarkMode = false;

    // Main
    QFont               Menu_Font;

    QFont               Dialog_Font;

    // FileList
    QFont               FileList_Font;
    QColor              FileList_FGColor;
    QColor              FileList_BGColor;

    bool                IsInverseCursor = false;
    bool                IsInverseSelect = false;
};

Q_DECLARE_METATYPE( TyColorScheme );

struct TyExternalEditor
{
    QString stableId_;
    QString             Name;
    QIcon               Icon;
    QString             IconPath;

    QString             FilePath;
    QString             CMDLine;

    QString             Detect;             // force | ext= ...
    QVector<QString>    VecExtensions;      // 확장자 목록, 소문자
};

Q_DECLARE_METATYPE( TyExternalEditor );
