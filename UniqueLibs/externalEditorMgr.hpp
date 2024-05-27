#pragma once


#include "cmnConcurrent.hpp"

class CExternalEditorMgr
{
public:
    void                                Refresh();



};

using TyStExternalEditorMgr = nsCmn::nsConcurrent::TSingleton<CExternalEditorMgr>;
