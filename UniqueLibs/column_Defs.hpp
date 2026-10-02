#pragma once
#include "commonLibs/columns/builtInColumns.hpp"

// Model indices belong only to this UI binding, never to persisted identities.
struct Column
{
    int Index = 0;
    hc::columns::ColumnDefinition definition_;
};

struct ColumnView
{
    hc::columns::ColumnProfile profile_;
    QVector<Column> VecColumns;
};
