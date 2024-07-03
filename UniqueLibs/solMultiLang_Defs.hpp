#pragma once

#include <QCoreApplication>

#define DECLARE_LANG_ITEM1( Var, Text )             const auto Var = QCoreApplication::translate( "Global", Text )
#define DECLARE_LANG_ITEM2( Var, Context, Text )    const auto Var = QCoreApplication::translate( Context, Text )


DECLARE_LANG_ITEM2( LANG_MNU_HELP, "Menu", "도움말(&H)" );

DECLARE_LANG_ITEM2( LANG_OPT_DUPLICATE_SCHEME, "Opts", "해당 이름을 가진 구성표가 존재합니다." );
