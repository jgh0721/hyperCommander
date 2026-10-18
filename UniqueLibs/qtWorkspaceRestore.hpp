#pragma once
#include "qtWorkspaceState.hpp"
#include <QSharedPointer>
class CFSModel;
namespace nsHC { class CFileSystemT; }
// Captures approved configuration on the GUI thread; provider work is async.
void RestoreWorkspaceLocation(CFSModel* Model, const hc::workspace::TabState& Tab, QSharedPointer<nsHC::CFileSystemT> Native = {});
