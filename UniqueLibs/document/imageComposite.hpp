#pragma once

/// 플랫폼/프레임워크 헤더파일
#include <QByteArrayView>
#include <QtEndian>
#include <optional>

namespace hc::document
{
    inline bool IsCompositeOnlyImage(QByteArrayView Bytes)
    {
        // Exporting one decoded image cannot preserve Photoshop layers or the DDS texture layout.
        return Bytes.startsWith("8BPS") || Bytes.startsWith("DDS ");
    }

    namespace image_detail
    {
        inline std::optional<QByteArrayView> readPhotoshopSection(QByteArrayView& Bytes, bool Wide = false)
        {
            const auto LengthBytes = Wide ? 8 : 4;
            if (Bytes.size() < LengthBytes) return {};
            const quint64 Length = Wide ? qFromBigEndian<quint64>(Bytes.data()) : qFromBigEndian<quint32>(Bytes.data());
            Bytes = Bytes.sliced(LengthBytes);
            if (Length > quint64(Bytes.size())) return {};
            const auto Section = Bytes.first(qsizetype(Length));
            Bytes = Bytes.sliced(qsizetype(Length));
            return Section;
        }
    }

    // Diagnose a failed decode from the PSD/PSB section lengths and Version Info
    // resource. Ordinary image loading does not walk these sections a second time.
    inline const char* PhotoshopCompositeError(QByteArrayView Bytes)
    {
        if (Bytes.size() < 26 || !Bytes.startsWith("8BPS")) return nullptr;
        const auto Version = qFromBigEndian<quint16>(Bytes.data() + 4);
        if (Version != 1 && Version != 2) return nullptr;
        Bytes = Bytes.sliced(26);
        if (!image_detail::readPhotoshopSection(Bytes)) return nullptr;
        const auto Resources = image_detail::readPhotoshopSection(Bytes);
        if (!Resources) return nullptr;
        auto Blocks = *Resources;
        while (Blocks.size() >= 12 && Blocks.startsWith("8BIM")) {
            const auto Id = qFromBigEndian<quint16>(Blocks.data() + 4);
            const auto NameLength = 1 + int(uchar(Blocks[6]));
            const auto NameBytes = NameLength + (NameLength % 2);
            if (Blocks.size() < 6 + NameBytes) return nullptr;
            Blocks = Blocks.sliced(6 + NameBytes);
            const auto Data = image_detail::readPhotoshopSection(Blocks);
            if (!Data) return nullptr;
            if (Id == 1057 && Data->size() >= 5 && (*Data)[4] == 0)
                return "photoshop-composite-absent";
            if (Data->size() % 2) {
                if (Blocks.isEmpty()) return nullptr;
                Blocks = Blocks.sliced(1);
            }
        }
        if (!image_detail::readPhotoshopSection(Bytes, Version == 2)) return nullptr;
        if (Bytes.isEmpty()) return "photoshop-composite-absent";
        if (Bytes.size() < 2) return nullptr;
        const auto Compression = qFromBigEndian<quint16>(Bytes.data());
        if (Compression == 2 || Compression == 3) return "photoshop-composite-zip";
        return nullptr;
    }
}
