#include "wlxDetection.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <charconv>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        // TC's detect grammar is a bounded data expression, not an executable
        // script: https://ghisler.github.io/WLX-SDK/listgetdetectstring.htm
        enum class Op { Number, String, Extension, Size, Force, Multimedia, Byte, Find, FindI, Not, Equal, Unequal, Less, Greater, And, Or };
        struct Node { Op op_; std::uint64_t number_ = 0; QByteArray text_; };
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"wlx"}}; }
        QByteArray Upper(QByteArray Bytes)
        {
            // Preserve embedded NUL and byte offsets. QString decoding would
            // replace arbitrary binary data or change its length.
            if (!Bytes.isEmpty()) CharUpperBuffA(Bytes.data(), static_cast<DWORD>(Bytes.size()));
            return Bytes;
        }
        class Parser
        {
        public:
            explicit Parser(const QByteArray& Source) : source_(Source) {}
            Result<std::vector<Node>> Parse()
            {
                if (source_.size() > 2047 || source_.contains('\0')) return Invalid("wlx-detect-length");
                skip();
                // An absent/empty detection expression supplies no automatic
                // match. Manual plugin selection remains available elsewhere.
                if (at_ == source_.size()) return std::vector<Node>{{Op::Number}};
                expression(0); skip();
                if (failed_ || at_ != source_.size()) return Invalid("wlx-detect-syntax");
                return std::move(nodes_);
            }
        private:
            void skip() { while (at_ < source_.size() && (source_[at_] == ' ' || source_[at_] == '\t' || source_[at_] == '\r' || source_[at_] == '\n')) ++at_; }
            bool take(char Token) { skip(); if (at_ < source_.size() && source_[at_] == Token) { ++at_; return true; } return false; }
            void require(char Token) { if (!take(Token)) failed_ = true; }
            void append(Node Value)
            {
                if (nodes_.size() >= 1024) failed_ = true;
                else nodes_.push_back(std::move(Value));
            }
            std::uint64_t number()
            {
                skip(); const auto Start = at_;
                while (at_ < source_.size() && source_[at_] >= '0' && source_[at_] <= '9') ++at_;
                std::uint64_t Value = 0;
                if (Start == at_ || std::from_chars(source_.constData() + Start, source_.constData() + at_, Value).ec != std::errc{}) failed_ = true;
                return Value;
            }
            QByteArray string()
            {
                require('"'); const auto Start = at_;
                while (at_ < source_.size() && source_[at_] != '"') ++at_;
                const auto Value = source_.mid(Start, at_ - Start); require('"'); return Value;
            }
            void expression(unsigned Depth)
            {
                conjunction(Depth);
                while (!failed_ && take('|')) { conjunction(Depth); append({Op::Or}); }
            }
            void conjunction(unsigned Depth)
            {
                comparison(Depth);
                while (!failed_ && take('&')) { comparison(Depth); append({Op::And}); }
            }
            void comparison(unsigned Depth)
            {
                atom(Depth); if (failed_) return;
                std::optional<Op> Operator;
                if (take('=')) Operator = Op::Equal;
                else if (take('!')) { require('='); Operator = Op::Unequal; }
                else if (take('<')) Operator = Op::Less;
                else if (take('>')) Operator = Op::Greater;
                if (Operator) { atom(Depth); append({*Operator}); }
            }
            void atom(unsigned Depth)
            {
                if (failed_) return;
                if (Depth >= 64) { failed_ = true; return; }
                skip(); if (at_ == source_.size()) { failed_ = true; return; }
                if (take('(')) { expression(Depth + 1); require(')'); return; }
                if (take('!')) { require('('); expression(Depth + 1); require(')'); append({Op::Not}); return; }
                if (take('['))
                {
                    const auto Offset = number(); require(']');
                    if (Offset >= 8192) failed_ = true;
                    append({Op::Byte, Offset}); return;
                }
                if (source_[at_] == '"') { append({Op::String, 0, string()}); return; }
                if (source_[at_] >= '0' && source_[at_] <= '9') { append({Op::Number, number()}); return; }
                const auto Start = at_;
                while (at_ < source_.size() && source_[at_] >= 'A' && source_[at_] <= 'Z') ++at_;
                const auto Name = source_.mid(Start, at_ - Start);
                if (Name == "EXT") append({Op::Extension});
                else if (Name == "SIZE") append({Op::Size});
                else if (Name == "FORCE") append({Op::Force});
                else if (Name == "MULTIMEDIA") append({Op::Multimedia});
                else if (Name == "FIND" || Name == "FINDI")
                {
                    require('('); const auto Text = string(); require(')');
                    append({Name == "FIND" ? Op::Find : Op::FindI, 0, Text});
                }
                else failed_ = true;
            }
            const QByteArray& source_;
            qsizetype at_ = 0;
            bool failed_ = false;
            std::vector<Node> nodes_;
        };
        using Value = std::variant<std::monostate, std::uint64_t, QByteArray>;
        std::optional<bool> Truth(const Value& Item)
        {
            if (const auto* Number = std::get_if<std::uint64_t>(&Item)) return *Number != 0;
            return {};
        }
        Value Boolean(std::optional<bool> Item)
        { return Item ? Value{std::uint64_t(*Item)} : Value{}; }
        Value Compare(Value Left, Value Right, Op Operator)
        {
            if (std::holds_alternative<std::monostate>(Left) || std::holds_alternative<std::monostate>(Right)) return {};
            if (Left.index() != Right.index())
            {
                auto& TextValue = std::holds_alternative<QByteArray>(Left) ? Left : Right;
                const auto& Text = std::get<QByteArray>(TextValue);
                if (Text.size() != 1) return std::uint64_t(0);
                TextValue = std::uint64_t(static_cast<unsigned char>(Text[0]));
            }
            int Order = 0;
            if (const auto* Number = std::get_if<std::uint64_t>(&Left))
            { const auto Other = std::get<std::uint64_t>(Right); Order = *Number < Other ? -1 : (*Number > Other ? 1 : 0); }
            else Order = std::get<QByteArray>(Left).compare(std::get<QByteArray>(Right));
            return std::uint64_t(Operator == Op::Equal ? Order == 0 : Operator == Op::Unequal ? Order != 0 : Operator == Op::Less ? Order < 0 : Order > 0);
        }
    }
    struct WlxDetectionExpression::Program
    {
        std::vector<Node> nodes_;
        bool needsPrefix_ = false, needsExtension_ = false, multimedia_ = false;
    };
    Result<WlxDetectionExpression> WlxDetectionExpression::Parse(const QByteArray& Expression)
    {
        auto Parsed = Parser(Expression).Parse(); if (!Parsed.HasValue()) return Parsed.Failure();
        auto Compiled = std::make_shared<Program>(); Compiled->nodes_ = std::move(Parsed.Value());
        for (const auto& Item : Compiled->nodes_)
        {
            Compiled->needsPrefix_ |= Item.op_ == Op::Byte || Item.op_ == Op::Find || Item.op_ == Op::FindI;
            Compiled->needsExtension_ |= Item.op_ == Op::Extension;
            Compiled->multimedia_ |= Item.op_ == Op::Multimedia;
        }
        return WlxDetectionExpression(std::move(Compiled));
    }
    bool WlxDetectionExpression::NeedsPrefix() const { return program_->needsPrefix_; }
    bool WlxDetectionExpression::NeedsExtension() const { return program_->needsExtension_; }
    Result<WlxDetectionMatch> WlxDetectionExpression::Evaluate(const WlxDetectionInput& Input) const
    {
        if (Input.extension_.size() > 32768 || Input.extension_.contains('\0')) return Invalid("wlx-detect-extension");
        const auto Required = static_cast<qsizetype>(std::min<std::uint64_t>(Input.size_, 8192));
        if (program_->needsPrefix_ && Input.prefix_.size() < Required) return Invalid("wlx-detect-prefix-incomplete");
        const auto Prefix = Input.prefix_.first(std::min(Input.prefix_.size(), Required));
        const auto Extension = Upper(Input.extension_); std::optional<QByteArray> Folded;
        std::vector<Value> Stack; Stack.reserve(program_->nodes_.size());
        for (const auto& Item : program_->nodes_)
        {
            switch (Item.op_)
            {
            case Op::Number: Stack.emplace_back(Item.number_); break;
            case Op::String: Stack.emplace_back(Item.text_); break;
            case Op::Extension: Stack.emplace_back(Extension); break;
            case Op::Size: Stack.emplace_back(Input.size_); break;
            case Op::Force: Stack.emplace_back(std::uint64_t(Input.force_)); break;
            case Op::Multimedia: Stack.emplace_back(std::uint64_t(1)); break;
            case Op::Byte:
                if (Item.number_ < static_cast<std::uint64_t>(Prefix.size())) Stack.emplace_back(std::uint64_t(static_cast<unsigned char>(Prefix[static_cast<qsizetype>(Item.number_)])));
                else Stack.emplace_back(std::monostate{});
                break;
            case Op::Find: Stack.emplace_back(std::uint64_t(Prefix.contains(Item.text_))); break;
            case Op::FindI:
                if (!Folded) Folded = Upper(Prefix);
                Stack.emplace_back(std::uint64_t(Folded->contains(Upper(Item.text_)))); break;
            case Op::Not:
            {
                if (std::holds_alternative<QByteArray>(Stack.back())) return Invalid("wlx-detect-boolean-type");
                const auto Operand = Truth(Stack.back()); Stack.back() = Boolean(Operand ? std::optional<bool>(!*Operand) : std::nullopt); break;
            }
            default:
            {
                auto Right = std::move(Stack.back()); Stack.pop_back(); auto Left = std::move(Stack.back()); Stack.pop_back();
                if (Item.op_ == Op::And || Item.op_ == Op::Or)
                {
                    if (std::holds_alternative<QByteArray>(Left) || std::holds_alternative<QByteArray>(Right)) return Invalid("wlx-detect-boolean-type");
                    const auto L = Truth(Left), R = Truth(Right); const bool Decisive = Item.op_ == Op::Or;
                    if (L == Decisive || R == Decisive) Stack.push_back(Boolean(Decisive));
                    else Stack.push_back(Boolean(L && R ? std::optional<bool>(!Decisive) : std::nullopt));
                }
                else Stack.push_back(Compare(std::move(Left), std::move(Right), Item.op_));
                break;
            }
            }
        }
        if (Stack.size() != 1 || std::holds_alternative<QByteArray>(Stack.back())) return Invalid("wlx-detect-boolean-type");
        return WlxDetectionMatch{Truth(Stack.back()), program_->multimedia_};
    }
}
