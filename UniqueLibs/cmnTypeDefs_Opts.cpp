#include "stdafx.h"

#include "cmnTypeDefs.hpp"
#include "cmnTypeDefs_Opts.hpp"


QString GetOPTValue( const TyOpts& Option )
{
    const auto Value = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject().value(Option.Section).toObject().value(Option.Key);
    return Value.isUndefined() ? Option.DefaultValue : Value.toVariant().toString();
}

void SetOPTValue( const TyOpts& Option, const QVariant& Variant )
{
    if (Option.Section.isEmpty() || Option.Key.isEmpty()) return;
    auto Options = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject();
    auto Category = Options[Option.Section].toObject(); Category[Option.Key] = QJsonValue::fromVariant(Variant);
    Options[Option.Section] = Category; hc::settings::AppSettings().Put(hc::settings::Section::Options, Options);
}
