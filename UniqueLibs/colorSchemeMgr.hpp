#pragma once

#include "cmnConcurrent.hpp"
#include "cmnTypeDefs.hpp"

class CColorSchemeMgr : public QObject
{
    Q_OBJECT
public:
    void Refresh();
    const TyColorScheme& Current() const { return current_; }
    TyColorScheme ForMode(bool Dark) const;

signals:
    void sigColorSchemeChanged(const TyColorScheme& ColorScheme);

private:
    TyColorScheme current_;
};

using TyStColorSchemeMgr = nsCmn::nsConcurrent::TSingleton<CColorSchemeMgr>;
