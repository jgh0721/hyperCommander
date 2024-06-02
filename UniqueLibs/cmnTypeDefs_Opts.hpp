#pragma once

#include <QObject>
#include <QString>

// VERSION = X.Y.Z.Build

struct TyOpts
{
    // UI
    QString                             Cate;
    QString                             Desc;
    QString                             Tooltip;

    // Settings
    QString                             Section;        // QSettings => Group
    QString                             Key;
    QString                             ValueType;
    QString                             DefaultValue;

    // Etc
    unsigned int                        Build = 0;
};

Q_DECLARE_METATYPE( TyOpts );

// 변수명, OPT_카테고리_키

const TyOpts OPT_COLORS_LISTFONT  = { QObject::tr( "색상" ), QObject::tr( "글꼴" ), QObject::tr( "파일 목록의 글꼴" ),
                                      "Colors", "FileListFontName", "FontName", "맑은 고딕",
                                      1 };

const TyOpts OPT_COLORS_LISTFONTSIZE = { QObject::tr( "색상" ), QObject::tr( "글꼴 크기" ), QObject::tr( "파일 목록의 글꼴 크기" ),
                                      "Colors", "FileListFontSize", "int", "14",
                                      1 };

const TyOpts OPT_COLORS_FORECOLOR = { QObject::tr( "색상" ), QObject::tr( "글꼴 색상" ), QObject::tr( "파일 목록의 글꼴 색상" ),
                                      "Colors", "FileListForeColor", "Color(RGB)", "#DFDFDF",
                                      1 };
const TyOpts OPT_COLORS_BACKCOLOR = { QObject::tr( "색상" ), QObject::tr( "배경 색상" ), QObject::tr( "파일 목록의 배경 색상" ),
                                      "Colors", "FileListBackColor", "Color(RGB)", "#1F1F1F", 
                                      1 };

constexpr auto OPT_SEC_WINDOW = QLatin1String( "Window" );
constexpr auto OPT_SEC_EDITOR = QLatin1String( "Editor" );

QString                                 GetOPTValue( const TyOpts& Option );
void                                    SetOPTValue( const TyOpts& Option, const QVariant& Variant );
