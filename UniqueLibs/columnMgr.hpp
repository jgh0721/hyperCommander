#pragma once

#include "column_Defs.hpp"

class CColumnMgr
{
public:

    void                                Initialize();

    ColumnView                          GetColumnView( int Index );

    // true = 분석이 끝나지 않았음. 그대로 다시 호출해야 함. 
    static bool                         Parse( wchar_t*& Def, ColumnParseResult& Result, QString& Content );
    static void                         CreateColumnContent( const ColumnParseResult& Result, Node* Info, QString& Content );

private:

    static void                         builtInFsColumn( QStringView Name, QStringView Type, Node* Info, QString& Content );

    QVector< ColumnView >               VecColumnViews;
};