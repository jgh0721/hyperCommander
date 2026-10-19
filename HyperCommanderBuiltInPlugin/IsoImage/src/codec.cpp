// SPDX-License-Identifier: LGPL-2.1-only
// Copyright (C) 2015-2022 Chris Lalancette; C++ modifications (C) 2026 HyperCommander contributors.
// Ported from PyCdlib utils.py, dates.py and udf.py; bounds are explicit in C++.
#include "image.hpp"
#include <chrono>

namespace hc::iso {
uint64_t Get(std::span<const uint8_t> Data, size_t Offset, size_t Width, bool Big)
{
    Require(Width <= 8 && Offset <= Data.size() && Width <= Data.size() - Offset, "Truncated field");
    uint64_t Value = 0;
    for (size_t I = 0; I < Width; ++I) Value |= uint64_t(Data[Offset + (Big ? Width - 1 - I : I)]) << (8 * I);
    return Value;
}
void Put(Bytes& Data, size_t Offset, uint64_t Value, size_t Width, bool Big)
{
    Require(Width <= 8 && Offset <= Data.size() && Width <= Data.size() - Offset, "Writer field overflow");
    Require(Width == 8 || Value < (uint64_t(1) << (Width * 8)), "Field value overflow");
    for (size_t I = 0; I < Width; ++I) Data[Offset + (Big ? Width - 1 - I : I)] = uint8_t(Value >> (8 * I));
}
void Both(Bytes& Data, size_t Offset, uint32_t Value, size_t Width)
{ Put(Data, Offset, Value, Width); Put(Data, Offset + Width, Value, Width, true); }
void Text(Bytes& Data, size_t Offset, std::string_view Value, size_t Width, uint8_t Pad)
{
    Require(Offset <= Data.size() && Width <= Data.size() - Offset && Value.size() <= Width, "Text field overflow");
    std::fill_n(Data.begin() + Offset, Width, Pad);
    std::copy(Value.begin(), Value.end(), Data.begin() + Offset);
}
std::u16string DecodeUtf8(std::string_view Value)
{
    std::u16string Result;
    for (size_t I = 0; I < Value.size();) {
        uint32_t C = uint8_t(Value[I++]);
        if (C >= 128) {
            const int Extra = C >= 0xf0 ? 3 : C >= 0xe0 ? 2 : C >= 0xc2 ? 1 : 0;
            Require(Extra != 0 && C <= 0xf4 && I + Extra <= Value.size(), "Invalid UTF-8");
            C &= (1u << (6 - Extra)) - 1;
            for (int J = 0; J < Extra; ++J) {
                const uint8_t B = uint8_t(Value[I++]);
                Require((B & 0xc0) == 0x80, "Invalid UTF-8 continuation"); C = (C << 6) | (B & 63);
            }
            Require(C >= (Extra == 1 ? 128u : Extra == 2 ? 2048u : 65536u), "Overlong UTF-8");
        }
        Require(C != 0 && C <= 0x10ffff && (C < 0xd800 || C > 0xdfff), "Invalid Unicode scalar");
        if (C >= 0x10000) { C -= 0x10000; Result += char16_t(0xd800 + (C >> 10)); Result += char16_t(0xdc00 + (C & 1023)); }
        else Result += char16_t(C);
    }
    return Result;
}
std::string EncodeUtf8(std::u16string_view Value)
{
    std::string Result;
    for (size_t I = 0; I < Value.size(); ++I) {
        uint32_t C = Value[I];
        if (C >= 0xd800 && C <= 0xdbff) {
            Require(++I < Value.size() && Value[I] >= 0xdc00 && Value[I] <= 0xdfff, "Invalid UTF-16 surrogate");
            C = 0x10000 + ((C - 0xd800) << 10) + (Value[I] - 0xdc00);
        } else Require(C < 0xdc00 || C > 0xdfff, "Invalid UTF-16 surrogate");
        Require(C != 0, "Embedded NUL in name");
        if (C < 128) Result += char(C);
        else if (C < 2048) { Result += char(0xc0 | (C >> 6)); Result += char(0x80 | (C & 63)); }
        else {
            if (C >= 65536) { Result += char(0xf0 | (C >> 18)); Result += char(0x80 | ((C >> 12) & 63)); }
            else Result += char(0xe0 | (C >> 12));
            Result += char(0x80 | ((C >> 6) & 63)); Result += char(0x80 | (C & 63));
        }
    }
    return Result;
}
Bytes Unicode(std::string_view Value, bool Osta)
{
    const auto Units = DecodeUtf8(Value);
    const bool Eight = Osta && std::all_of(Units.begin(), Units.end(), [](auto C) { return C < 256; });
    Bytes Result;
    if (Osta) Result.push_back(Eight ? 8 : 16);
    for (const auto C : Units) { if (!Eight) Result.push_back(uint8_t(C >> 8)); Result.push_back(uint8_t(C)); }
    return Result;
}
std::string DecodeUnicode(std::span<const uint8_t> Value, bool Osta)
{
    bool Eight = false;
    if (Osta) { Require(!Value.empty() && (Value[0] == 8 || Value[0] == 16), "Unsupported OSTA compression", HC_ISO_UNSUPPORTED); Eight = Value[0] == 8; Value = Value.subspan(1); }
    Require(Eight || Value.size() % 2 == 0, "Odd UTF-16 length");
    std::u16string Units;
    for (size_t I = 0; I < Value.size(); I += Eight ? 1 : 2) Units += char16_t(Eight ? Value[I] : Get(Value, I, 2, true));
    return EncodeUtf8(Units);
}
std::string CleanPath(std::string_view Value)
{
    Require(!Value.empty() && Value.size() <= 4096 && Value.front() != '/' && Value.back() != '/', "Invalid image path");
    DecodeUtf8(Value);
    size_t Start = 0;
    for (size_t I = 0; I <= Value.size(); ++I) {
        if (I != Value.size()) Require(uint8_t(Value[I]) >= 32 && Value[I] != '\\' && Value[I] != ':', "Unsafe image path");
        if (I == Value.size() || Value[I] == '/') {
            const auto Part = Value.substr(Start, I - Start);
            Require(!Part.empty() && Part != "." && Part != "..", "Unsafe image component"); Start = I + 1;
        }
    }
    return std::string(Value);
}
fs::path DiskPath(const char* Value)
{ Require(Value && *Value, "Missing disk path"); DecodeUtf8(Value); return fs::u8path(Value); }
uint16_t Crc16(std::span<const uint8_t> Value)
{
    uint16_t Crc = 0;
    for (auto B : Value) { Crc ^= uint16_t(B) << 8; for (int I = 0; I < 8; ++I) Crc = uint16_t((Crc << 1) ^ ((Crc & 0x8000) ? 0x1021 : 0)); }
    return Crc;
}
void Tag(Bytes& Data, uint16_t Id, uint32_t Location, size_t Length)
{
    Require(Length >= 16 && Length <= Data.size() && Length - 16 <= 65535, "Invalid UDF tag length");
    Put(Data, 0, Id, 2); Put(Data, 2, 2, 2); Put(Data, 6, 1, 2);
    Put(Data, 8, Crc16(std::span(Data).subspan(16, Length - 16)), 2);
    Put(Data, 10, Length - 16, 2); Put(Data, 12, Location, 4);
    Data[4] = 0; for (size_t I = 0; I < 16; ++I) if (I != 4) Data[4] += Data[I];
}
void CheckTag(std::span<const uint8_t> Data, uint32_t Location, uint16_t Id)
{
    Require(Data.size() >= 16 && (!Id || Get(Data, 0, 2) == Id), "Unexpected UDF descriptor");
    Require((Get(Data, 2, 2) == 2 || Get(Data, 2, 2) == 3) && Get(Data, 12, 4) == Location, "Invalid UDF tag location/version");
    uint8_t Sum = 0; for (size_t I = 0; I < 16; ++I) if (I != 4) Sum += Data[I];
    const auto Length = Get(Data, 10, 2);
    Require(Sum == Data[4] && Length <= Data.size() - 16, "Invalid UDF tag checksum/length");
    const auto Kind = Get(Data, 0, 2);
    const size_t Minimum = Kind == 261 ? 160 : Kind == 266 ? 200 : Kind == 257 ? 22 : Kind == 6 ? 424 : Kind == 7 ? 8 : Kind == 9 ? 64 : 496;
    Require(Length >= Minimum, "UDF CRC does not cover required descriptor fields");
    Require(Crc16(Data.subspan(16, size_t(Length))) == Get(Data, 8, 2), "Invalid UDF descriptor CRC");
}
void Timestamp(Bytes& Data, size_t Offset, int64_t Epoch, bool Udf, bool Explicit)
{
    using namespace std::chrono;
    const sys_seconds Time{seconds(Epoch || Explicit ? Epoch : 946684800)};
    const auto Day = floor<days>(Time); const year_month_day Date{Day}; const hh_mm_ss Clock{Time - Day};
    const int Year = int(Date.year()); Require(Date.ok() && Year >= 1900 && Year <= 2155, "Timestamp out of range");
    if (Udf) { Put(Data, Offset, 0x1000, 2); Put(Data, Offset + 2, Year, 2); Offset += 4; }
    else Data[Offset++] = uint8_t(Year - 1900);
    Data[Offset++] = uint8_t(unsigned(Date.month())); Data[Offset++] = uint8_t(unsigned(Date.day()));
    Data[Offset++] = uint8_t(Clock.hours().count()); Data[Offset++] = uint8_t(Clock.minutes().count()); Data[Offset++] = uint8_t(Clock.seconds().count());
    Data[Offset] = 0;
}
void Pulse(const HcIsoOptions& Options, uint64_t Done, uint64_t Total)
{ if (Options.progress_ && !Options.progress_(Options.context_, Done, Total)) throw Error(HC_ISO_CANCELLED, "ISO operation cancelled"); }
}
