#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>
#include <limits>

namespace hc::document::epub {
// A source-scroll request crosses from the editor to the sandboxed preview helper; both ends accept
// exactly chapter, line (>= 1), ratio (0..1), end and serial, plus optionally the editor row's screen
// height below the preview frame (viewOffset) and the anchor character's source-map position
// (anchor: element path, text child, offset), with the same bounds sourcePositions.js enforces.
inline bool ValidSourceScrollRequest(const QJsonObject& Value) {
    const double Line = Value["line"].toDouble(-1), Ratio = Value["ratio"].toDouble(-1);
    if (Value["chapter"].toInt(-1) < 0 || !std::isfinite(Line) || Line < 1 || Ratio < 0 || Ratio > 1
        || !Value["end"].isBool() || !Value["serial"].isDouble())
        return false;
    const auto Whole = [](const QJsonValue& Number, double Low, double High) {
        const double At = Number.toDouble(std::numeric_limits<double>::quiet_NaN());
        return Number.isDouble() && std::isfinite(At) && At == std::floor(At) && At >= Low && At <= High;
    };
    qsizetype Expected = 5;
    if (Value.contains("viewOffset")) {
        const double Offset = Value["viewOffset"].toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!Value["viewOffset"].isDouble() || !std::isfinite(Offset) || std::abs(Offset) > 100000) return false;
        ++Expected;
    }
    if (Value.contains("anchor")) {
        if (!Value["anchor"].isObject()) return false;
        const auto Anchor = Value["anchor"].toObject();
        if (Anchor.size() != 3 || !Anchor["path"].isArray() || Anchor["path"].toArray().size() > 128
            || !Whole(Anchor["child"], -1, 65535) || !Whole(Anchor["offset"], 0, 2 * 1024 * 1024))
            return false;
        for (const auto& Index : Anchor["path"].toArray())
            if (!Whole(Index, 0, 32767)) return false;
        ++Expected;
    }
    return Value.size() == Expected;
}
}
