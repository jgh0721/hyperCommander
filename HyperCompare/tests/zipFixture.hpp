#pragma once
#include <QByteArray>
#include <QVector>
#include <QtEndian>
namespace hc::compare::test
{
    struct ZipEntry { QByteArray path_, data_; quint16 flags_ = 0x800; quint32 attributes_ = 0; };
    // Independent stored-ZIP fixture writer, including deliberately invalid names/flags.
    inline QByteArray StoredZip(const QVector<ZipEntry>& Entries, const QByteArray& Comment = {})
    {
        const auto crc = [](const QByteArray& Bytes) {
            quint32 value = 0xffffffffu;
            for (unsigned char byte : Bytes) { value ^= byte; for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u))); }
            return value ^ 0xffffffffu;
        };
        const auto u16 = [](QByteArray& Out, quint16 Value) { char bytes[2]; qToLittleEndian(Value, bytes); Out.append(bytes, 2); };
        const auto u32 = [](QByteArray& Out, quint32 Value) { char bytes[4]; qToLittleEndian(Value, bytes); Out.append(bytes, 4); };
        QByteArray output, central;
        for (const auto& entry : Entries)
        {
            const auto offset = quint32(output.size()), hash = crc(entry.data_), size = quint32(entry.data_.size());
            u32(output, 0x04034b50); u16(output, 20); u16(output, entry.flags_); u16(output, 0); u16(output, 0); u16(output, 0);
            u32(output, hash); u32(output, size); u32(output, size); u16(output, quint16(entry.path_.size())); u16(output, 0); output += entry.path_; output += entry.data_;
            u32(central, 0x02014b50); u16(central, 3 * 256 + 20); u16(central, 20); u16(central, entry.flags_); u16(central, 0); u16(central, 0); u16(central, 0);
            u32(central, hash); u32(central, size); u32(central, size); u16(central, quint16(entry.path_.size())); u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0);
            u32(central, entry.attributes_ | (entry.path_.endsWith('/') ? 0x10 : 0)); u32(central, offset); central += entry.path_;
        }
        const auto start = quint32(output.size()); output += central;
        u32(output, 0x06054b50); u16(output, 0); u16(output, 0); u16(output, quint16(Entries.size())); u16(output, quint16(Entries.size()));
        u32(output, quint32(central.size())); u32(output, start); u16(output, quint16(Comment.size())); output += Comment; return output;
    }
}
