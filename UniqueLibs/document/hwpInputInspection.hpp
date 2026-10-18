#pragma once
#include <QByteArray>
#include <QString>
namespace hc::document {
    struct HwpInputInspection {
        bool accepted_ = false, restricted_ = false;
        QString format_, error_;
    };
    // Metadata/container gate, not a guarantee of rendering or round-trip fidelity.
    // Run on a worker; no extraction, filesystem writes or renderer callbacks.
    HwpInputInspection InspectHwpInput(const QByteArray& Bytes, const QString& Format);
}
