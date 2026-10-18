#include "stdafx.h"
#include "updateJson.hpp"

#include <limits>

namespace hc::update {
const Json& Json::At(std::string_view Key, Kind Expected) const {
    const auto It = object_.find(Key);
    if (kind_ != Kind::Object || It == object_.end() || It->second.kind_ != Expected) throw std::runtime_error("update-json-field");
    return It->second;
}
namespace {
class Parser {
public:
    explicit Parser(std::string_view Bytes) : bytes_(Bytes) {}
    Json Parse() {
        if (bytes_.empty() || bytes_.size() > 1048576 ||
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes_.data(), int(bytes_.size()), nullptr, 0)) Fail();
        auto Value = Read(0); Space(); if (position_ != bytes_.size()) Fail(); return Value;
    }
private:
    [[noreturn]] static void Fail() { throw std::runtime_error("update-json-invalid"); }
    void Space() { while (position_ < bytes_.size() && (bytes_[position_] == ' ' || bytes_[position_] == '\r' || bytes_[position_] == '\n' || bytes_[position_] == '\t')) ++position_; }
    bool Take(char Value) { Space(); if (position_ < bytes_.size() && bytes_[position_] == Value) { ++position_; return true; } return false; }
    unsigned Hex() {
        if (position_ + 4 > bytes_.size()) Fail();
        unsigned Value = 0;
        for (unsigned Index = 0; Index < 4; ++Index) {
            const auto Ch = bytes_[position_++]; unsigned Digit = 0;
            if (Ch >= '0' && Ch <= '9') Digit = Ch - '0'; else if (Ch >= 'a' && Ch <= 'f') Digit = Ch - 'a' + 10;
            else if (Ch >= 'A' && Ch <= 'F') Digit = Ch - 'A' + 10; else Fail();
            Value = Value * 16 + Digit;
        }
        return Value;
    }
    static void Utf8(std::string& Result, unsigned Code) {
        if (Code < 0x80) Result += char(Code);
        else if (Code < 0x800) { Result += char(0xc0 | (Code >> 6)); Result += char(0x80 | (Code & 63)); }
        else if (Code < 0x10000) { Result += char(0xe0 | (Code >> 12)); Result += char(0x80 | ((Code >> 6) & 63)); Result += char(0x80 | (Code & 63)); }
        else { Result += char(0xf0 | (Code >> 18)); Result += char(0x80 | ((Code >> 12) & 63)); Result += char(0x80 | ((Code >> 6) & 63)); Result += char(0x80 | (Code & 63)); }
    }
    std::string String() {
        if (!Take('"')) Fail(); std::string Result;
        while (position_ < bytes_.size()) {
            auto Ch = bytes_[position_++];
            if (Ch == '"') return Result;
            if (static_cast<unsigned char>(Ch) < 32) Fail();
            if (Ch != '\\') { Result += Ch; continue; }
            if (position_ == bytes_.size()) Fail(); Ch = bytes_[position_++];
            switch (Ch) {
            case '"': case '\\': case '/': Result += Ch; break;
            case 'b': Result += '\b'; break; case 'f': Result += '\f'; break;
            case 'n': Result += '\n'; break; case 'r': Result += '\r'; break; case 't': Result += '\t'; break;
            case 'u': {
                auto Code = Hex();
                if (Code >= 0xd800 && Code <= 0xdbff) {
                    if (position_ + 2 > bytes_.size() || bytes_.substr(position_, 2) != "\\u") Fail();
                    position_ += 2; const auto Low = Hex(); if (Low < 0xdc00 || Low > 0xdfff) Fail();
                    Code = 0x10000 + ((Code - 0xd800) << 10) + Low - 0xdc00;
                } else if (Code >= 0xdc00 && Code <= 0xdfff) Fail();
                Utf8(Result, Code); break;
            }
            default: Fail();
            }
        }
        Fail();
    }
    Json Read(unsigned Depth) {
        Space(); if (Depth > 24 || position_ == bytes_.size() || ++nodes_ > 40000) Fail();
        Json Result; const char Ch = bytes_[position_];
        if (Ch == '{') {
            ++position_; Result.kind_ = Json::Kind::Object; if (Take('}')) return Result;
            do { const auto Key = String(); if (!Take(':')) Fail(); auto Value = Read(Depth + 1); if (!Result.object_.emplace(Key, std::move(Value)).second) Fail(); } while (Take(','));
            if (!Take('}')) Fail(); return Result;
        }
        if (Ch == '[') {
            ++position_; Result.kind_ = Json::Kind::Array; if (Take(']')) return Result;
            do { Result.array_.push_back(Read(Depth + 1)); } while (Take(',')); if (!Take(']')) Fail(); return Result;
        }
        if (Ch == '"') { Result.kind_ = Json::Kind::String; Result.text_ = String(); return Result; }
        for (const auto Word : {std::string_view("true"), std::string_view("false"), std::string_view("null")}) {
            if (bytes_.substr(position_, Word.size()) == Word) {
                position_ += Word.size(); Result.kind_ = Word == "null" ? Json::Kind::Null : Json::Kind::Boolean; Result.boolean_ = Word == "true"; return Result;
            }
        }
        if (Ch < '0' || Ch > '9') Fail();
        Result.kind_ = Json::Kind::Number;
        do {
            const auto Digit = unsigned(bytes_[position_] - '0');
            if (Result.number_ > (std::numeric_limits<std::uint64_t>::max() - Digit) / 10) Fail();
            Result.number_ = Result.number_ * 10 + Digit; ++position_;
            if (Ch == '0' && position_ < bytes_.size() && bytes_[position_] >= '0' && bytes_[position_] <= '9') Fail();
        } while (position_ < bytes_.size() && bytes_[position_] >= '0' && bytes_[position_] <= '9');
        return Result;
    }
    std::string_view bytes_; std::size_t position_ = 0, nodes_ = 0;
};
}
Json ParseJson(std::string_view Bytes) { return Parser(Bytes).Parse(); }
std::string QuoteJson(std::string_view Text) {
    constexpr char Digits[] = "0123456789abcdef"; std::string Result = "\"";
    for (const auto Ch : Text) {
        if (Ch == '"' || Ch == '\\') { Result += '\\'; Result += Ch; }
        else if (static_cast<unsigned char>(Ch) < 32) { Result += "\\u00"; Result += Digits[(static_cast<unsigned char>(Ch) >> 4) & 15]; Result += Digits[Ch & 15]; }
        else Result += Ch;
    }
    return Result + '"';
}
}
