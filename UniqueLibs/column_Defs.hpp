#pragma once
#include "commonLibs/columns/builtInColumns.hpp"

// Model indices belong only to this UI binding, never to persisted identities.
struct Column
{
    int Index = 0;
    hc::columns::ColumnDefinition definition_;
    hc::columns::CompiledTemplate compiled_;
};

struct ColumnView
{
    hc::columns::ColumnProfile profile_;
    QVector<Column> VecColumns;
};

inline ColumnView BindColumnProfile(const hc::columns::ColumnProfile& Profile)
{
    ColumnView View; View.profile_ = Profile;
    // Navigation/editing has one primary name binding at model column zero.
    // User order is visual and UUID based; it does not change file identity.
    for (const auto& Definition : Profile.columns_)
        if (Definition.id_ == Profile.primaryNameId_) View.VecColumns.append({0, Definition, hc::columns::CompileTemplate(Definition.template_)});
    for (const auto& Definition : Profile.columns_)
        if (Definition.id_ != Profile.primaryNameId_) View.VecColumns.append({int(View.VecColumns.size()), Definition, hc::columns::CompileTemplate(Definition.template_)});
    return View;
}
