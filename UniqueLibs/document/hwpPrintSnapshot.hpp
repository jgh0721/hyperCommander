#pragma once
#include "commonLibs/document/documentTypes.hpp"
#include <QByteArray>
#include <QSizeF>
#include <QMetaType>
namespace hc::document {
    struct HwpPrintPage { int index_ = 0; QSizeF paperPixels_; QByteArray png_; };
    struct HwpPrintFrames { DocumentOperationTicket ticket_; int totalPages_ = 0; std::vector<HwpPrintPage> pages_; };
    // Worker-side PDF snapshot of independently rendered print pages. No screen
    // screenshot, file destination, printer access or saved-baseline mutation.
    storage::Result<QByteArray> BuildHwpPdf(const DocumentOperationTicket& Expected,
        const std::vector<int>& RequestedPages, const HwpPrintFrames& Frames);
}

Q_DECLARE_METATYPE(hc::document::HwpPrintFrames)
