#pragma once
#include "column_Defs.hpp"
#include "concurrent/cmnSingleton.hpp"

namespace nsHC { class CFileSourceT; }

class CColumnMgr
{
public:
    CColumnMgr();
    void Initialize();
    qsizetype GetCount() const;
    ColumnView GetColumnView(int Index);
    static hc::columns::CellValue GetValue(const hc::columns::ColumnDefinition& Column, const nsHC::CFileSourceT* Info);
    static QString FormatValue(const hc::columns::ColumnDefinition& Column, const hc::columns::CellValue& Value, const nsHC::CFileSourceT* Info);
private:
    QVector<ColumnView> columnViews_;
};

using TyStColumnMgr = nsCmn::nsConcurrent::TSingleton<CColumnMgr>;
