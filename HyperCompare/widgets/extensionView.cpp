#include "stdafx.h"
#include "extensionView.hpp"
#include "imageCompareView.hpp"
#include "archiveCompareView.hpp"
#include "core/compare.hpp"

namespace hc::compare
{
    ExtensionView* CreateExtensionView(const QJsonObject& Comparison, const QStringList& Paths, QWidget* Parent)
    {
        const auto schema = Comparison["payloadSchema"].toString();
        if (schema == "image.rgba.v1") return new ImageCompareView(Comparison, Paths, Parent);
        if (schema == "archive.entries.v1") return new ArchiveCompareView(Comparison, Paths, Parent);
        throw Failure("ExtensionViewUnsupported");
    }
}
