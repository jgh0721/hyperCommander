#include "stdafx.h"
#include "directoryText.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/document/documentTextCodec.hpp"

namespace hc::compare
{
    namespace
    {
        template<class T> T require(storage::Result<T> Value)
        { if (!Value.HasValue()) throw Failure(Value.Failure().code_); return std::move(Value.Value()); }
        constexpr qsizetype limit = 16 * 1024 * 1024;
        QByteArray read(const storage::EntrySnapshot& Entry, const Budget& Limits)
        {
            Limits.Check(); const auto provider = storage::MakeNativeReadProvider(Entry.locator_.kind_);
            storage::OperationContext ctx; ctx.deadline_ = Limits.deadline_; ctx.workerRead_ = true; ctx.denyConcurrentMutation_ = true;
            const auto bound = require(provider->Resolve(Entry.locator_, ctx).get());
            if (bound.id_.revision_ != Entry.id_.revision_) throw Failure("DirectorySourceChanged");
            const auto stream = require(provider->OpenRead(bound.id_, ctx).get()); QByteArray bytes;
            for (;;)
            {
                Limits.Check(); const auto chunk = require(stream->Read(1024 * 1024, ctx).get());
                if (chunk.bytes_.size() > std::size_t(limit - bytes.size())) throw Failure("DirectorySourceChanged");
                bytes.append(reinterpret_cast<const char*>(chunk.bytes_.data()), qsizetype(chunk.bytes_.size()));
                if (chunk.eof_) break; if (chunk.bytes_.empty()) throw Failure("ReadMadeNoProgress");
            }
            if (std::uint64_t(bytes.size()) != Entry.size_ || require(provider->Stat(bound.id_, ctx).get()).id_.revision_ != Entry.id_.revision_) throw Failure("DirectorySourceChanged");
            return bytes;
        }
    }
    std::optional<QByteArray> MergeDirectoryText(const std::array<storage::EntrySnapshot, 3>& Inputs, const QString& Encoding, const Budget& Limits)
    {
        for (const auto& input : Inputs) if (input.directory_ || input.size_ > std::uint64_t(limit)) return {};
        std::array<QByteArray, 3> text; QString outputEncoding; bool bom = false;
        for (std::size_t i = 0; i < Inputs.size(); ++i)
        {
            const auto raw = read(Inputs[i], Limits);
            // Match text-session policy: no implicit locale-dependent legacy fallback.
            const auto encoding = !Encoding.isEmpty() ? Encoding : raw.startsWith("\xff\xfe") ? QString("UTF-16LE") : raw.startsWith("\xfe\xff") ? QString("UTF-16BE") : QString("UTF-8");
            const auto decoded = document::DecodeDocumentText(raw, encoding); if (!decoded.HasValue()) return {};
            text[i] = decoded.Value().text_.toUtf8(); if (text[i].size() > limit) return {};
            const auto lines = SplitLines({text[i].constData(), std::size_t(text[i].size())});
            if (lines.size() > 300000) return {};
            for (const auto line : lines) if (line.size() > 256 * 1024) return {};
            if (i == 0) { outputEncoding = decoded.Value().encoding_; bom = decoded.Value().bom_; }
        }
        auto local = Limits; local.deadline_ = std::min(Limits.deadline_, std::chrono::steady_clock::now() + std::chrono::milliseconds(500));
        MergeResult merged;
        try
        {
            const auto view = [&](std::size_t i) { return std::string_view(text[i].constData(), std::size_t(text[i].size())); };
            merged = Merge(view(2), view(0), view(1), local);
        }
        catch (const Failure& error)
        {
            Limits.Check();
            if (error.code_ == "Timeout" || error.code_ == "DiffMemoryLimit" || error.code_ == "DiffWorkLimit") return {};
            throw;
        }
        if (!merged.conflicts_.empty() || merged.text_.size() > 32 * 1024 * 1024) return {};
        const auto encoded = document::EncodeDocumentText(QString::fromUtf8(merged.text_.data(), qsizetype(merged.text_.size())), outputEncoding, bom);
        if (!encoded.HasValue() || encoded.Value().size() > 32 * 1024 * 1024) return {};
        return encoded.Value();
    }
}
