#pragma once

#include <QLineEdit>

namespace hc::ui
{
inline void SelectRenameText(QLineEdit* Input, bool IncludeExtension)
{
    const auto Dot = Input->text().lastIndexOf('.');
    if (IncludeExtension || Dot <= 0) Input->selectAll();
    else Input->setSelection(0, static_cast<int>(Dot));
}
}
