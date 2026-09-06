#include "qtViewerProvider.hpp"
#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>
#include <QStringDecoder>
#include <Windows.h>
#include <cstring>

namespace hc::viewer
{
    using namespace storage;
    namespace
    {
        QByteArray Bytes(std::span<const std::byte> Data) { return QByteArray(reinterpret_cast<const char*>(Data.data()), static_cast<qsizetype>(Data.size())); }
        QString Extension(const EntrySnapshot& Entry) { return QFileInfo(QString::fromStdString(Entry.name_)).suffix().toLower(); }
        bool UnicodeBom(const QByteArray& Data)
        { return Data.startsWith("\xef\xbb\xbf") || Data.startsWith("\xff\xfe") || Data.startsWith("\xfe\xff") || Data.startsWith(QByteArray("\0\0\xfe\xff", 4)); }
        bool ValidUnicodeUnits(const QByteArray& Data, QStringConverter::Encoding Encoding)
        {
            const bool Utf16 = Encoding == QStringConverter::Utf16 || Encoding == QStringConverter::Utf16LE || Encoding == QStringConverter::Utf16BE;
            const bool Utf32 = Encoding == QStringConverter::Utf32 || Encoding == QStringConverter::Utf32LE || Encoding == QStringConverter::Utf32BE;
            if (!Utf16 && !Utf32) return true;
            const qsizetype Width = Utf16 ? 2 : 4;
            if (Data.size() % Width) return false;
            bool Little = Encoding != QStringConverter::Utf16BE && Encoding != QStringConverter::Utf32BE;
            if (Data.startsWith(Utf16 ? QByteArray::fromHex("fffe") : QByteArray::fromHex("fffe0000"))) Little = true;
            if (Data.startsWith(Utf16 ? QByteArray::fromHex("feff") : QByteArray::fromHex("0000feff"))) Little = false;
            bool HighSurrogate = false;
            for (qsizetype Offset = 0; Offset < Data.size(); Offset += Width)
            {
                std::uint32_t Value = 0;
                for (qsizetype Index = 0; Index < Width; ++Index)
                    Value |= static_cast<std::uint32_t>(static_cast<unsigned char>(Data[Offset + Index])) << (8 * (Little ? Index : Width - Index - 1));
                if (Utf32)
                {
                    if (Value > 0x10ffff || (Value >= 0xd800 && Value <= 0xdfff)) return false;
                }
                else if (HighSurrogate)
                {
                    if (Value < 0xdc00 || Value > 0xdfff) return false;
                    HighSurrogate = false;
                }
                else if (Value >= 0xd800 && Value <= 0xdbff) HighSurrogate = true;
                else if (Value >= 0xdc00 && Value <= 0xdfff) return false;
            }
            return !HighSurrogate;
        }
        Result<QByteArray> ReadAll(ViewerInput& Input, OperationContext Context, const std::function<void(std::uint64_t)>& Progress)
        {
            if (!Input.stream_ || !Context.maxReadBytes_ || Input.prefix_.size() > Input.source_.size_)
                return Error{ErrorCategory::InvalidRequest, "invalid-viewer-input"};
            if (Input.source_.size_ > 128ULL * 1024 * 1024)
                return Error{ErrorCategory::BudgetExceeded, "viewer-input-budget"};
            auto Data = Bytes(Input.prefix_); Input.prefix_.clear();
            if (Input.source_.size_ > Input.request_.limits_.maxInputBytes_) return Error{ErrorCategory::BudgetExceeded, "viewer-input-budget"};
            Data.reserve(static_cast<qsizetype>(Input.source_.size_));
            while (!Input.eof_)
            {
                if (const auto Failure = Context.Check({})) return *Failure;
                const auto ReadSize = std::min<std::size_t>(Context.maxReadBytes_, 1024 * 1024);
                auto Chunk = Input.stream_->Read(ReadSize, Context).get(); if (!Chunk.HasValue()) return Chunk.Failure();
                if (Chunk.Value().bytes_.size() > ReadSize || (Chunk.Value().bytes_.empty() && !Chunk.Value().eof_))
                    return Error{ErrorCategory::Internal, "invalid-viewer-read"};
                if (Chunk.Value().bytes_.size() > Input.source_.size_ - Data.size()) return Error{ErrorCategory::StaleRevision, "viewer-source-size-changed"};
                Data += Bytes(Chunk.Value().bytes_); Input.eof_ = Chunk.Value().eof_;
                if (Progress) Progress(Data.size());
            }
            if (static_cast<std::uint64_t>(Data.size()) != Input.source_.size_) return Error{ErrorCategory::StaleRevision, "viewer-source-size-changed"};
            if (const auto Failure = Context.Check({})) return *Failure;
            if (Progress) Progress(Data.size());
            return Data;
        }
        Result<QString> Decode(const QByteArray& Data, std::string& Name)
        {
            if (Name == "auto")
            {
                // Test the four-byte BOM before UTF-16's shared prefix.
                if (Data.startsWith(QByteArray("\xff\xfe\0\0", 4))) Name = "utf-32le";
                else if (Data.startsWith(QByteArray("\0\0\xfe\xff", 4))) Name = "utf-32be";
                else if (Data.startsWith("\xff\xfe")) Name = "utf-16le";
                else if (Data.startsWith("\xfe\xff")) Name = "utf-16be";
                else Name = "utf-8";
            }
            if (Name == "cp949")
            {
                if (Data.isEmpty()) return QString{};
                const auto Size = MultiByteToWideChar(949, MB_ERR_INVALID_CHARS, Data.constData(), static_cast<int>(Data.size()), nullptr, 0);
                if (!Size) return Error{ErrorCategory::InvalidRequest, "viewer-invalid-encoding"};
                std::wstring Text(Size, L'\0');
                if (!MultiByteToWideChar(949, MB_ERR_INVALID_CHARS, Data.constData(), static_cast<int>(Data.size()), Text.data(), Size))
                    return Error{ErrorCategory::InvalidRequest, "viewer-invalid-encoding"};
                return QString::fromStdWString(Text);
            }
            const auto Encoding = Name == "system" ? std::optional<QStringConverter::Encoding>(QStringConverter::System)
                : QStringConverter::encodingForName(QString::fromStdString(Name));
            if (!Encoding) return Error{ErrorCategory::Unsupported, "viewer-encoding-unavailable"};
            // Qt's conversion error flag alone does not reject every malformed
            // UTF-16/32 input (trailing bytes and surrogate scalar values).
            if (!ValidUnicodeUnits(Data, *Encoding)) return Error{ErrorCategory::InvalidRequest, "viewer-invalid-encoding"};
            // One complete input, so incomplete trailing sequences are errors.
            QStringDecoder Decoder(*Encoding, QStringConverter::Flag::Stateless);
            QString Text = Decoder(Data);
            if (Decoder.hasError()) return Error{ErrorCategory::InvalidRequest, "viewer-invalid-encoding"};
            return Text;
        }
        class BuiltinProvider final : public IViewerProvider
        {
        public:
            explicit BuiltinProvider(ViewerKind Kind) : kind_(Kind) {}
            std::string Id() const override { return kind_ == ViewerKind::Text ? "builtin.text" : kind_ == ViewerKind::Markdown ? "builtin.markdown" : "builtin.image"; }
            std::vector<std::string> Commands() const override
            {
                if (kind_ == ViewerKind::Image) return {"zoom-in", "zoom-out", "fit", "original-size"};
                if (kind_ == ViewerKind::Markdown) return {"copy", "find", "zoom-in", "zoom-out", "source"};
                return {"copy", "find", "wrap", "zoom-in", "zoom-out", "encoding"};
            }
            Result<int> Probe(const EntrySnapshot& Source, std::span<const std::byte> Prefix) const override
            {
                const auto Ext = Extension(Source); auto Data = Bytes(Prefix);
                if (kind_ == ViewerKind::Markdown) return Ext == "md" || Ext == "markdown" ? 90 : 0;
                if (kind_ == ViewerKind::Image)
                {
                    QBuffer Buffer(&Data); Buffer.open(QIODevice::ReadOnly); QImageReader Reader(&Buffer); Reader.setDecideFormatFromContent(true);
                    if (Reader.canRead()) return 100;
                    return QStringList{"png", "jpg", "jpeg", "gif", "webp", "bmp", "ico", "tif", "tiff", "ppm", "pgm", "pbm", "xbm", "xpm"}.contains(Ext) ? 80 : 0;
                }
                if (QStringList{"pdf", "exe", "dll", "zip", "7z", "rar", "wcx64", "wlx64"}.contains(Ext)) return 0;
                if (UnicodeBom(Data)) return 20;
                for (const auto Byte : Data) if (static_cast<unsigned char>(Byte) < 32 && Byte != '\t' && Byte != '\r' && Byte != '\n' && Byte != '\f') return 0;
                return 10;
            }
            Result<ViewerDocument> Open(ViewerInput Input, OperationContext Context, std::function<void(std::uint64_t)> Progress) override
            {
                auto Read = ReadAll(Input, Context, Progress); if (!Read.HasValue()) return Read.Failure();
                ViewerDocument Document; Document.kind_ = kind_;
                if (kind_ == ViewerKind::Image)
                {
                    QBuffer Buffer(&Read.Value()); Buffer.open(QIODevice::ReadOnly); QImageReader Reader(&Buffer);
                    Reader.setDecideFormatFromContent(true); Reader.setAutoTransform(true);
                    const auto Size = Reader.size();
                    if (!Size.isValid() || Size.isEmpty()) return Error{ErrorCategory::Unsupported, "viewer-image-header-invalid"};
                    const auto Pixels = static_cast<std::uint64_t>(Size.width()) * Size.height();
                    if (Pixels > Input.request_.limits_.maxImagePixels_ || Pixels > Input.request_.limits_.maxDecodedBytes_ / 4)
                        return Error{ErrorCategory::BudgetExceeded, "viewer-image-pixel-budget"};
                    auto Image = Reader.read();
                    if (Image.isNull()) return Error{ErrorCategory::InvalidRequest, "viewer-image-decode-failed"};
                    if (const auto Failure = Context.Check({})) return *Failure;
                    if (static_cast<std::uint64_t>(Image.width()) * Image.height() > Input.request_.limits_.maxImagePixels_
                        || static_cast<std::uint64_t>(Image.width()) * Image.height() > Input.request_.limits_.maxDecodedBytes_ / 4
                        || static_cast<std::uint64_t>(Image.sizeInBytes()) > Input.request_.limits_.maxDecodedBytes_)
                        return Error{ErrorCategory::BudgetExceeded, "viewer-image-decoded-budget"};
                    Image = Image.convertToFormat(QImage::Format_RGBA8888);
                    if (Image.isNull()) return Error{ErrorCategory::Unavailable, "viewer-image-conversion-failed"};
                    Document.width_ = Image.width(); Document.height_ = Image.height(); Document.imageFormat_ = Reader.format().toStdString();
                    Document.pixels_.resize(static_cast<std::size_t>(Document.width_) * Document.height_ * 4);
                    for (std::uint32_t Row = 0; Row < Document.height_; ++Row)
                    {
                        if (const auto Failure = Context.Check({})) return *Failure;
                        std::memcpy(Document.pixels_.data() + static_cast<std::size_t>(Row) * Document.width_ * 4, Image.constScanLine(Row), static_cast<std::size_t>(Document.width_) * 4);
                    }
                }
                else
                {
                    auto Encoding = Input.request_.encoding_;
                    auto Text = Decode(Read.Value(), Encoding);
                    if (!Text.HasValue() && Input.request_.encoding_ == "auto" && !UnicodeBom(Read.Value()))
                    { Encoding = "cp949"; Text = Decode(Read.Value(), Encoding); }
                    if (!Text.HasValue()) return Text.Failure();
                    const auto Limit = kind_ == ViewerKind::Markdown ? Input.request_.limits_.maxMarkdownCharacters_ : Input.request_.limits_.maxTextCharacters_;
                    if (static_cast<std::size_t>(Text.Value().size()) > Limit || static_cast<std::uint64_t>(Text.Value().size()) > Input.request_.limits_.maxDecodedBytes_ / 2)
                        return Error{ErrorCategory::BudgetExceeded, "viewer-text-budget"};
                    Document.text_ = Text.Value().toStdU16String(); Document.encoding_ = Encoding;
                }
                return Document;
            }
        private:
            ViewerKind kind_;
        };
    }
    std::shared_ptr<ViewerRegistry> MakeBuiltinViewerRegistry()
    {
        auto Registry = std::make_shared<ViewerRegistry>();
        for (const auto Kind : {ViewerKind::Text, ViewerKind::Markdown, ViewerKind::Image}) Registry->Register(std::make_shared<BuiltinProvider>(Kind));
        return Registry;
    }
}
