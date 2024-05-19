#include "stdafx.h"

#include "cmnTypeDefs.hpp"
#include "cmnTypeDefs_Opts.hpp"


QString GetOPTValue( const TyOpts& Option )
{
    if( Option.Section.isEmpty() == true )
        return Option.DefaultValue;

    StSettings->beginGroup( Option.Section );
    const auto Ret = StSettings->value( Option.Key ).toString();
    StSettings->endGroup();

    return Ret.isEmpty() == true ? Option.DefaultValue : Ret;
}

void SetOPTValue( const TyOpts& Option, const QVariant& Variant )
{
    if( Option.Section.isEmpty() == true || Option.Key.isEmpty() == true )
        return;

    StSettings->beginGroup( Option.Section );
    StSettings->setValue( Option.Key, Variant );
    StSettings->endGroup();
}
