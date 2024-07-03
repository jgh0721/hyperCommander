#pragma once

#include "column_Defs.hpp"
#include "concurrent/cmnSingleton.hpp"

namespace nsHC
{
    class CFileSourceT;
}

class CColumnMgr
{
public:
    CColumnMgr();

    void                                Initialize();

    qsizetype                           GetCount() const;
    ColumnView                          GetColumnView( int Index );

    // true = 분석이 끝나지 않았음. 그대로 다시 호출해야 함. 
    static bool                         Parse( wchar_t*& Def, ColumnParseResult& Result, QString& Content );
    static void                         CreateColumnContent( const ColumnParseResult& Result, nsHC::CFileSourceT* Info, QString& Content );

private:

    static void                         builtInFsColumn( QStringView Name, QStringView Type, nsHC::CFileSourceT* Info, QString& Content );

    QVector< ColumnView >               VecColumnViews;
};

using TyStColumnMgr = nsCmn::nsConcurrent::TSingleton< CColumnMgr >;
