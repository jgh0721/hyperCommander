#pragma once

// Immutable UTF-8 bytes shared by editing, folding, preview and save workers.
// An edit copies boundary chunks and AVL paths, never the unchanged document.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace hc::document
{
    class TextSnapshot
    {
    public:
        struct EncodingResult { std::string bytes_, error_; };
        using Encoder = std::function<EncodingResult(std::string_view)>;
    private:
        struct Node;
        using Root = std::shared_ptr<const Node>;
        struct Node
        {
            Root left_, right_;
            std::shared_ptr<const std::string> data_;
            std::shared_ptr<const std::string> encoded_;
            std::string error_;
            std::size_t offset_ = 0, size_ = 0;
            std::size_t encodedSize_ = 0;
            std::size_t cr_ = 0, lf_ = 0, crlf_ = 0;
            unsigned char first_ = 0, last_ = 0;
            int height_ = 1;
            std::uint64_t hash_ = 0, power_ = 1;
            std::uint64_t encodedHash_ = 0, encodedPower_ = 1;
        };
        static constexpr std::uint64_t Base = 1099511628211ULL;
        static constexpr std::size_t ChunkSize = 4096;
        Root root_;
        std::shared_ptr<const Encoder> encoder_;
        std::string prefix_;
        std::string emptyBytes_;
        static int height(const Root& Value) { return Value ? Value->height_ : 0; }
        static std::size_t size(const Root& Value) { return Value ? Value->size_ : 0; }
        static Root leaf(std::shared_ptr<const std::string> Data, std::size_t Offset, std::size_t Length, const std::shared_ptr<const Encoder>& Encode)
        {
            if (!Length) return {};
            auto Value = std::make_shared<Node>();
            Value->data_ = std::move(Data); Value->offset_ = Offset; Value->size_ = Length;
            Value->first_ = static_cast<unsigned char>((*Value->data_)[Offset]);
            for (const unsigned char Byte : std::string_view(*Value->data_).substr(Offset, Length)) {
                Value->cr_ += Byte == '\r'; Value->lf_ += Byte == '\n';
                Value->crlf_ += Value->last_ == '\r' && Byte == '\n'; Value->last_ = Byte;
                Value->hash_ = Value->hash_ * Base + Byte + 1;
                Value->power_ *= Base;
            }
            Value->encodedSize_ = Length; Value->encodedHash_ = Value->hash_; Value->encodedPower_ = Value->power_;
            if (Encode) {
                auto Encoded = (*Encode)(std::string_view(*Value->data_).substr(Offset, Length));
                Value->error_ = std::move(Encoded.error_);
                Value->encoded_ = std::make_shared<const std::string>(std::move(Encoded.bytes_));
                Value->encodedSize_ = Value->encoded_->size(); Value->encodedHash_ = 0; Value->encodedPower_ = 1;
                for (const unsigned char Byte : *Value->encoded_) {
                    Value->encodedHash_ = Value->encodedHash_ * Base + Byte + 1;
                    Value->encodedPower_ *= Base;
                }
            }
            return Value;
        }
        static Root branch(Root Left, Root Right)
        {
            if (!Left) return Right;
            if (!Right) return Left;
            auto Value = std::make_shared<Node>();
            Value->left_ = std::move(Left); Value->right_ = std::move(Right);
            Value->size_ = Value->left_->size_ + Value->right_->size_;
            Value->height_ = 1 + std::max(height(Value->left_), height(Value->right_));
            Value->hash_ = Value->left_->hash_ * Value->right_->power_ + Value->right_->hash_;
            Value->power_ = Value->left_->power_ * Value->right_->power_;
            Value->encodedSize_ = Value->left_->encodedSize_ + Value->right_->encodedSize_;
            Value->encodedHash_ = Value->left_->encodedHash_ * Value->right_->encodedPower_ + Value->right_->encodedHash_;
            Value->encodedPower_ = Value->left_->encodedPower_ * Value->right_->encodedPower_;
            Value->error_ = Value->left_->error_.empty() ? Value->right_->error_ : Value->left_->error_;
            Value->cr_ = Value->left_->cr_ + Value->right_->cr_;
            Value->lf_ = Value->left_->lf_ + Value->right_->lf_;
            Value->crlf_ = Value->left_->crlf_ + Value->right_->crlf_ + (Value->left_->last_ == '\r' && Value->right_->first_ == '\n');
            Value->first_ = Value->left_->first_; Value->last_ = Value->right_->last_;
            return Value;
        }
        static Root balance(Root Left, Root Right)
        {
            if (height(Left) > height(Right) + 1) {
                if (height(Left->left_) >= height(Left->right_))
                    return branch(Left->left_, branch(Left->right_, std::move(Right)));
                const auto Middle = Left->right_;
                return branch(branch(Left->left_, Middle->left_), branch(Middle->right_, std::move(Right)));
            }
            if (height(Right) > height(Left) + 1) {
                if (height(Right->right_) >= height(Right->left_))
                    return branch(branch(std::move(Left), Right->left_), Right->right_);
                const auto Middle = Right->left_;
                return branch(branch(std::move(Left), Middle->left_), branch(Middle->right_, Right->right_));
            }
            return branch(std::move(Left), std::move(Right));
        }
        static Root join(Root Left, Root Right)
        {
            if (!Left) return Right;
            if (!Right) return Left;
            if (height(Left) > height(Right) + 1)
                return balance(Left->left_, join(Left->right_, std::move(Right)));
            if (height(Right) > height(Left) + 1)
                return balance(join(std::move(Left), Right->left_), Right->right_);
            return branch(std::move(Left), std::move(Right));
        }
        static std::pair<Root, Root> split(const Root& Value, std::size_t Position, const std::shared_ptr<const Encoder>& Encode)
        {
            if (!Position) return {{}, Value};
            if (Position == size(Value)) return {Value, {}};
            if (Value->data_) return {leaf(Value->data_, Value->offset_, Position, Encode),
                leaf(Value->data_, Value->offset_ + Position, Value->size_ - Position, Encode)};
            const auto LeftSize = size(Value->left_);
            if (Position < LeftSize) {
                auto [Before, After] = split(Value->left_, Position, Encode);
                return {std::move(Before), join(std::move(After), Value->right_)};
            }
            auto [Before, After] = split(Value->right_, Position - LeftSize, Encode);
            return {join(Value->left_, std::move(Before)), std::move(After)};
        }
        static Root build(const std::shared_ptr<const std::string>& Data, std::size_t Offset, std::size_t Length, const std::shared_ptr<const Encoder>& Encode)
        {
            if (Length <= ChunkSize + 4) return leaf(Data, Offset, Length, Encode);
            auto LeftSize = ((Length / ChunkSize + 1) / 2) * ChunkSize;
            // Encoders consume complete UTF-8 scalars. Scintilla edit boundaries are scalar boundaries too.
            while (LeftSize < Length && (static_cast<unsigned char>((*Data)[Offset + LeftSize]) & 0xc0) == 0x80) ++LeftSize;
            if (LeftSize == Length) LeftSize = ((Length / ChunkSize + 1) / 2) * ChunkSize;
            return branch(build(Data, Offset, LeftSize, Encode), build(Data, Offset + LeftSize, Length - LeftSize, Encode));
        }
        template<typename Visitor> static void visit(const Root& Value, Visitor& Visit)
        {
            if (!Value) return;
            if (Value->data_) Visit(std::string_view(*Value->data_).substr(Value->offset_, Value->size_));
            else { visit(Value->left_, Visit); visit(Value->right_, Visit); }
        }
        static void appendEncoded(const Root& Value, std::string& Bytes)
        {
            if (!Value) return;
            if (Value->data_) {
                if (Value->encoded_) Bytes.append(*Value->encoded_);
                else Bytes.append(*Value->data_, Value->offset_, Value->size_);
            } else { appendEncoded(Value->left_, Bytes); appendEncoded(Value->right_, Bytes); }
        }
        std::uint64_t serializedHash() const
        {
            if (root_ && !root_->error_.empty()) return root_->hash_;
            std::uint64_t Hash = 0;
            for (const unsigned char Byte : root_ ? prefix_ : emptyBytes_) Hash = Hash * Base + Byte + 1;
            return root_ ? Hash * root_->encodedPower_ + root_->encodedHash_ : Hash;
        }
    public:
        TextSnapshot() = default;
        explicit TextSnapshot(std::string Bytes, Encoder Encode = {}, std::string Prefix = {}, std::string EmptyBytes = {})
            : prefix_(std::move(Prefix)), emptyBytes_(std::move(EmptyBytes))
        {
            if (Encode) encoder_ = std::make_shared<const Encoder>(std::move(Encode));
            auto Data = std::make_shared<const std::string>(std::move(Bytes));
            root_ = build(Data, 0, Data->size(), encoder_);
        }
        std::size_t Size() const { return size(root_); }
        // Bits: CRLF, lone CR, lone LF. Counts compose correctly across chunk boundaries.
        unsigned LineEndings() const
        {
            return !root_ ? 0 : (root_->crlf_ ? 1u : 0u) | (root_->cr_ > root_->crlf_ ? 2u : 0u) | (root_->lf_ > root_->crlf_ ? 4u : 0u);
        }
        int Depth() const { return height(root_); }
        bool SameRoot(const TextSnapshot& Other) const { return root_ == Other.root_; }
        // A mismatch proves inequality; a matching hash never proves equality.
        bool MayEqual(const TextSnapshot& Other) const
        {
            return Size() == Other.Size() && (!root_ || root_->hash_ == Other.root_->hash_);
        }
        TextSnapshot Replace(std::size_t Position, std::size_t Removed, std::string Inserted) const
        {
            if (Position > Size() || Removed > Size() - Position) throw std::out_of_range("text snapshot edit");
            auto [Before, Tail] = split(root_, Position, encoder_);
            auto [Discarded, After] = split(Tail, Removed, encoder_);
            TextSnapshot Result;
            Result.encoder_ = encoder_; Result.prefix_ = prefix_; Result.emptyBytes_ = emptyBytes_;
            auto Data = std::make_shared<const std::string>(std::move(Inserted));
            Result.root_ = join(join(std::move(Before), build(Data, 0, Data->size(), encoder_)), std::move(After));
            return Result;
        }
        template<typename Visitor> void ForEach(Visitor Visit) const { visit(root_, Visit); }
        // Worker-side materialization. Capturing the TextSnapshot itself is O(1).
        std::string Bytes() const
        {
            std::string Result; Result.reserve(Size());
            ForEach([&](std::string_view Part) { Result.append(Part); });
            return Result;
        }
        std::string SerializationError() const { return root_ ? root_->error_ : std::string{}; }
        std::size_t SerializedSize() const
        {
            return root_ && !root_->error_.empty() ? Size() : (root_ ? prefix_.size() + root_->encodedSize_ : emptyBytes_.size());
        }
        bool SameSerializedRoot(const TextSnapshot& Other) const { return SameRoot(Other) && prefix_ == Other.prefix_ && emptyBytes_ == Other.emptyBytes_; }
        bool MayEqualSerialized(const TextSnapshot& Other) const
        {
            return SerializedSize() == Other.SerializedSize() && serializedHash() == Other.serializedHash();
        }
        std::string SerializedBytes() const
        {
            // Preserve all source on an encoding error; callers must check the paired verdict before writing.
            if (root_ && !root_->error_.empty()) return Bytes();
            std::string Result = root_ ? prefix_ : emptyBytes_; Result.reserve(SerializedSize()); appendEncoded(root_, Result); return Result;
        }
    };
}
