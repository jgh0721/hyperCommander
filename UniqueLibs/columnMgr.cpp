#include "stdafx.h"
#include "columnMgr.hpp"

void CColumnMgr::Initialize()
{
    VecColumnViews.push_back( ColumnView { QObject::tr( "자세히" ), QVector< Column >() 
                              << Column {
                                    0, 0, QObject::tr( "이름" ), Qt::AlignLeft, "[=HC.name]"
                                  }
                              << Column {
                                    1, 0, QObject::tr( "크기" ), Qt::AlignCenter, "[=HC.size]"
                                }
                              << Column {
                                    2, 0, QObject::tr( "만든 날짜" ), Qt::AlignCenter, "[=HC.created]"
                                }
                              << Column {
                                    3, 0, QObject::tr( "속성" ), Qt::AlignCenter, "[=HC.attribText]"
                                }
                              } );
}

ColumnView CColumnMgr::GetColumnView( int Index )
{
    if( Index < 0 || Index >= VecColumnViews.size() )
        return {};

    return VecColumnViews[ Index ];
}

/*!



    File Filter => Label, 



*/

constexpr int FILE_SET_NOR_FILTERS      = 0x1;
constexpr int FILE_SET_NOR_EXT_FILTERS  = 0x2;

constexpr int FILE_SET_EXP_ATTRIBUTE    = 0x100;
constexpr int FILE_SET_EXP_SIZE         = 0x200;

struct FileSet
{
    QString                             Name;
    uint32_t                            Flags = 0;

    // 일반
    QStringList                         Filters;
    QVector<QString>                    VecExtFilters;

    // 확장
    DWORD                               Attributes = 0;
    int64_t                             Size = 0;

    QColor                              Color;
};

struct Node;

class CFileSetMgr
{
public:

    QColor Judge( const Node* Item );


private:

    QVector< FileSet >                  VecFileSet;
};

struct ColorScheme
{
    QString                             Name;

    // Main
    QString                             FontFaceMain;

    // List

    QString                             FontFace;

    QColor                              Background;
    QColor                              Foreground;
    QColor                              Selected;
    QColor                              Cursor;

    bool IsInverseCursor;
    bool IsInverseSelect;

};
