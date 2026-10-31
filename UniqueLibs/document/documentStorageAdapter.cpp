#include "documentStorageAdapter.hpp"
#include "commonLibs/document/epub/epubPackage.hpp"
#include "imagePerformance.hpp"
#include "imageDecodeCache.hpp"
#include "imagePartialDecoder.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include "imageBook.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
#include "UniqueLibs/nativeStorageRevision.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStringConverter>
#include "documentTextCodec.hpp"
#include <QBuffer>
#include <QImageReader>

namespace hc::document
{
    using namespace storage;
    namespace
    {
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"document"}}; }
        Locator nativeLocation(const QString& Path) {
            auto Native=QDir::toNativeSeparators(Path);
            if(Native.startsWith("\\\\?\\UNC\\",Qt::CaseInsensitive))Native="\\\\"+Native.mid(8);
            else if(Native.startsWith("\\\\?\\"))Native=Native.mid(4);
            return {Native.startsWith("\\\\")?LocatorKind::Smb:LocatorKind::Local,Native.toUtf8().toStdString()};
        }
        void documentReadRevision(EntrySnapshot& Source,const QString& Path)
        {
            auto Native=QDir::toNativeSeparators(Path);
            if(Native.startsWith("\\\\?\\UNC\\",Qt::CaseInsensitive))Native="\\\\"+Native.mid(8);
            // RDP IDs change across handles even when the file is unchanged.
            // Preserve exact mtime/size checks; document saves additionally verify
            // the original SHA-256 before beginning the write transaction.
            if(Native.startsWith("\\\\tsclient\\",Qt::CaseInsensitive)&&Source.id_.revision_)
                Source.id_.revision_=native::MetadataRevision(*Source.id_.revision_);
        }
        Result<QByteArray> Read(const QString& Path, OperationContext Context, bool Image = false, QCryptographicHash* Digest = nullptr, QByteArray* Revision = nullptr, qint64 Limit = DocumentByteLimit, const std::function<void(QImage)>& Preview = {})
        {
            ImageTraceSpan ReadTrace("file.read-and-hash",Path);
            if (auto Stop = Context.Check({"document"})) return *Stop;
            QFile File(Path);
            if (!File.open(QIODevice::ReadOnly)) return Error{ErrorCategory::AccessDenied, "document-open-denied"};
            const auto FileSize=File.size();
            if (!Image && FileSize > Limit) return Error{ErrorCategory::BudgetExceeded, "p0-text-byte-limit"};
            const auto Before=Revision?ImageDecodeCache::Signature(File):QByteArray{};
            ImagePartialDecoder Progressive;
            QByteArray Bytes;if(FileSize>=0&&FileSize<=64*1024*1024)Bytes.reserve(FileSize);
            while(!File.atEnd()) { if(auto Stop=Context.Check({"document"}))return *Stop;auto Chunk=File.read(256*1024);if(Chunk.isEmpty()&&!File.atEnd())return Error{ErrorCategory::Unavailable,"document-read-incomplete"};ReadTrace.AddBytes(Chunk.size());if(Digest)Digest->addData(Chunk);Bytes+=Chunk;Progressive.Update(Bytes,Preview);if(!Image && Bytes.size()>Limit)return Error{ErrorCategory::BudgetExceeded,"document-read-budget"}; }
            if (File.error() != QFileDevice::NoError || (!Image && Bytes.size() > Limit) || !File.atEnd())
                return Error{ErrorCategory::Unavailable, "document-read-incomplete"};
            if(Revision){const auto After=ImageDecodeCache::Signature(File);if(!Before.isEmpty()&&Before!=After)return Error{ErrorCategory::StaleRevision,"document-changed-during-read"};*Revision=After;}
            return Bytes;
        }
    }
    Result<TextInput> ReadDocumentFile(const QString& Path, OperationContext Context, bool Unlimited, std::function<void(QImage)> Preview)
    {
        ImageTraceSpan Trace("document.read",Path);
        if(!QFileInfo(Path).isAbsolute())return Invalid("document-absolute-path-required");
        if(IsImageBook(Path)||QFileInfo(Path).isDir()) {
            auto Book=OpenImageBook(Path,Context,false);if(!Book.HasValue())return Book.Failure();
            AnalyzeImageBook(Book.Value(),Context);
            if(auto Stop=Context.Check({"image-book"}))return *Stop;
            TextInput Input;Input.path_=QFileInfo(Path).absoluteFilePath();Input.viewId_="image";Input.book_=Book.Value();
            // Books are read-only page sources. A content hash of the complete
            // archive must not force an SMB download before its first page.
            Input.hash_=QCryptographicHash::hash((Input.path_+Book.Value()->revision_).toUtf8(),QCryptographicHash::Sha256);
            return Input;
        }
        const auto Canonical=QFileInfo(Path).canonicalFilePath();if(Canonical.isEmpty())return Error{ErrorCategory::NotFound,"document-file-missing"};
        const auto Location=nativeLocation(Canonical);auto Reader=MakeNativeReadProvider(Location.kind_);
        auto Before=Reader->Resolve(Location,Context).get();if(!Before.HasValue())return Before.Failure();
        documentReadRevision(Before.Value(),Canonical);
        if(const auto Cached=ImageDecodeCache::Find(Canonical)){
            auto After=Reader->Stat(Before.Value().id_,Context).get();if(!After.HasValue())return After.Failure();
            documentReadRevision(After.Value(),Canonical);
            if(auto Stop=Context.Check({"document"}))return *Stop;
            TextInput Input;Input.path_=Canonical;Input.source_=After.Value();Input.bytes_=Cached->bytes_;Input.hash_=Cached->hash_;Input.image_=Cached->image_;Input.viewId_="image";return Input;
        }
        QFile ProbeFile(Canonical); bool ImageContent = false;
        if (ProbeFile.open(QIODevice::ReadOnly)) {
            ImageTraceSpan ProbeTrace("image.probe",Path);
            const auto Header=ProbeFile.peek(2);
            QImageReader Probe(&ProbeFile); Probe.setDecideFormatFromContent(true);
            ImageContent = Probe.canRead();
            if(ImageContent&&Header.size()==2&&Header[0]=='P'&&Header[1]>='1'&&Header[1]<='6')ImageContent=!Probe.size().isEmpty();
        }
        QCryptographicHash Digest(QCryptographicHash::Sha256);QByteArray Revision;
        // Metadata-only sources have no reliable per-handle identity to cache;
        // their exact version is rechecked by Stat and their bytes by SHA-256.
        const bool HasIdentity=Before.Value().id_.revision_&&Before.Value().id_.revision_->token_.find(":identity:")!=std::string::npos;
        const bool Epub=QFileInfo(Path).suffix().compare("epub",Qt::CaseInsensitive)==0;
        auto Bytes=Read(Canonical,Context,(ImageContent||Unlimited)&&!Epub,&Digest,HasIdentity?&Revision:nullptr,Epub?epub::InputLimit:DocumentByteLimit,ImageContent?Preview:std::function<void(QImage)>{});
        if(!Bytes.HasValue()) {
            if(Epub&&Bytes.Failure().category_==ErrorCategory::BudgetExceeded)return Error{ErrorCategory::BudgetExceeded,"epub-input-budget"};
            return Bytes.Failure();
        }
        auto After=Reader->Stat(Before.Value().id_,Context).get();if(!After.HasValue())return After.Failure();
        documentReadRevision(After.Value(),Canonical);
        TextInput Input;Input.path_=Canonical;Input.source_=After.Value();Input.bytes_=std::move(Bytes.Value());
        Input.hash_=Digest.result();
        const auto Suffix=QFileInfo(Path).suffix().toLower();
        // RHWP validates the container and renders in its isolated process.
        // Never decode an HWPX ZIP or binary HWP as text or an image book.
        if(Suffix=="epub"){Input.viewId_="epub";return Input;}
        if(Suffix=="hwp"||Suffix=="hwpx") {Input.viewId_=Suffix;return Input;}
        if(Input.bytes_.startsWith("%PDF-")) {Input.viewId_="pdf";return Input;}
        if(Suffix=="pdf")return Invalid("document-pdf-invalid-header");
        Input.image_=ImageDecodeCache::FindDecoded(Canonical,Input.hash_);
        if(!Input.image_.isNull()){
            ImageDecodeCache::Remember(Canonical,{Input.bytes_,Input.hash_,Revision,Input.image_});Input.viewId_="image";return Input;
        }
        QBuffer Buffer(&Input.bytes_);Buffer.open(QIODevice::ReadOnly);QImageReader ImageReader(&Buffer);
        if(ImageReader.canRead()) {
            const auto Size=ImageReader.size();
            // The PBM plugin recognizes a plain-text line beginning with P4.
            // An invalid image header must fall through to TEXT; explicit image
            // suffixes are still rejected by the codec check below.
            if(!Size.isEmpty()) {
                // Qt and the OS report allocation failure; image dimensions are not a product quota.
                QImageReader::setAllocationLimit(0);
                if(Preview){auto Image=ImagePartialDecoder::DecodeCompletePreview(Input.bytes_);if(!Image.isNull())Preview(std::move(Image));}
                if(auto Stop=Context.Check({"document"}))return *Stop;
                ImageReader.setAutoTransform(true);{ImageTraceSpan DecodeTrace("image.decode",Path);Input.image_=ImageReader.read();}
                const bool IncompleteJpeg=Input.bytes_.startsWith(QByteArray("\xff\xd8",2))&&!ImagePartialDecoder::HasCompleteJpeg(Input.bytes_);
                if(Input.image_.isNull() || IncompleteJpeg){
                    if(Preview){auto Partial=ImagePartialDecoder::Decode(Input.bytes_);if(!Partial.isNull())Preview(std::move(Partial));}
                    return Invalid("document-image-incomplete");
                }
                if(auto Stop=Context.Check({"document"}))return *Stop;
                if(Preview)Preview(Input.image_.width()>1024 || Input.image_.height()>1024
                    ? Input.image_.scaled(QSize(1024,1024),Qt::KeepAspectRatio,Qt::SmoothTransformation) : Input.image_);
                if(auto Stop=Context.Check({"document"}))return *Stop;
                ImageDecodeCache::Remember(Canonical,{Input.bytes_,Input.hash_,Revision,Input.image_});
                Input.viewId_="image";return Input;
            }
        }
        if(IsImageFile(Path)||QStringList{"heic","heif","avif","jxl"}.contains(Suffix)){
            if(Preview){auto Partial=ImagePartialDecoder::Decode(Input.bytes_);if(!Partial.isNull())Preview(std::move(Partial));}
            return Invalid("document-image-codec-unavailable-or-corrupt");
        }
        if(!Unlimited&&Input.bytes_.size()>TextByteLimit)return Error{ErrorCategory::BudgetExceeded,"document-text-byte-limit"};
        auto Text=DecodeDocumentText(Input.bytes_);if(!Text.HasValue())return Text.Failure();
        Input.text_=Text.Value().text_;Input.encoding_=Text.Value().encoding_;Input.bom_=Text.Value().bom_;
        bool CrLf=false,Cr=false,Lf=false;
        for(qsizetype I=0;I<Input.text_.size();++I){
            if(Input.text_[I]=='\r'){
                if(I+1<Input.text_.size() && Input.text_[I+1]=='\n'){CrLf=true;++I;}else Cr=true;
            }else if(Input.text_[I]=='\n')Lf=true;
            if(int(CrLf)+int(Cr)+int(Lf)>1)break;
        }
        Input.eol_=(int(CrLf)+int(Cr)+int(Lf)>1)?"MIXED":CrLf?"CRLF":Cr?"CR":"LF";
        return Input;
    }
    Result<TextInput> ReadTextFile(const QString& Path, OperationContext Context)
    {
        if (!QFileInfo(Path).isAbsolute()) return Invalid("document-absolute-path-required");
        const auto Canonical = QFileInfo(Path).canonicalFilePath();
        if (Canonical.isEmpty()) return Error{ErrorCategory::NotFound, "document-file-missing"};
        const auto Location=nativeLocation(Canonical);auto Reader = MakeNativeReadProvider(Location.kind_);
        auto Before = Reader->Resolve(Location, Context).get(); if (!Before.HasValue()) return Before.Failure();
        documentReadRevision(Before.Value(),Canonical);
        auto ReadResult = Read(Canonical, Context); if (!ReadResult.HasValue()) return ReadResult.Failure();
        auto After = Reader->Stat(Before.Value().id_, Context).get(); if (!After.HasValue()) return After.Failure();
        documentReadRevision(After.Value(),Canonical);
        auto Bytes = std::move(ReadResult.Value());
        TextInput Input; Input.path_ = Canonical; Input.source_ = After.Value();
        Input.hash_ = QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256);
        Input.bom_ = Bytes.startsWith("\xef\xbb\xbf");
        if (Input.bom_) Bytes.remove(0, 3);
        QStringDecoder Decoder(QStringDecoder::Utf8); Input.text_ = Decoder(Bytes);
        if (Decoder.hasError() || Input.text_.contains(QChar::Null)) return Invalid("p0-utf8-text-required");
        // Qt uses U+2029 for structural blocks, so a literal separator cannot
        // round-trip in this P1 view. Reject it instead of rewriting the source.
        if (Input.text_.contains(QChar(0x2029))) return Invalid("p1-paragraph-eol-unavailable");
        const auto CrLf = Input.text_.count("\r\n");
        auto Other = Input.text_; Other.remove("\r\n");
        if (Other.contains('\r') || (CrLf && Other.contains('\n'))) return Invalid("p0-mixed-eol-unavailable");
        Input.eol_ = CrLf ? "CRLF" : "LF";
        Input.text_.replace("\r\n", "\n"); return Input;
    }
    QByteArray EncodeText(const QString& Text, bool Bom, const QString& Eol)
    {
        auto Output = Text; Output.replace("\r\n", "\n");
        if (Eol == "CRLF") Output.replace("\n", "\r\n");
        return (Bom ? QByteArray("\xef\xbb\xbf") : QByteArray{}) + Output.toUtf8();
    }
    Result<LocalSaveResult> SaveTextFile(const QString& Path, const QByteArray& Bytes,
        DestinationExpectation Expected, const QByteArray& ExpectedHash, OperationContext Context, bool Image, bool Unlimited)
    {
        if (!QFileInfo(Path).isAbsolute() || (!Unlimited && !Image && Bytes.size() > DocumentByteLimit)) return Invalid("p0-save-target-or-budget");
        if (Expected.exists_ && !ExpectedHash.isEmpty())
        {
            QFile Current(Path); if (!Current.open(QIODevice::ReadOnly)) return Error{ErrorCategory::AccessDenied, "document-open-denied"};
            QCryptographicHash Hash(QCryptographicHash::Sha256);
            while (!Current.atEnd()) {
                if (auto Stop = Context.Check({"document"})) return *Stop;
                const auto Chunk = Current.read(256 * 1024);
                if (Chunk.isEmpty() && !Current.atEnd()) return Error{ErrorCategory::Unavailable, "document-read-incomplete"};
                Hash.addData(Chunk);
            }
            if (Current.error() != QFileDevice::NoError) return Error{ErrorCategory::Unavailable, "document-read-incomplete"};
            if (Hash.result() != ExpectedHash)
                return Error{ErrorCategory::StaleRevision, "document-external-content-change"};
        }
        const auto Location=nativeLocation(Path);
        auto Reader = MakeNativeReadProvider(Location.kind_);
        auto Writer = MakeNativeWritableStorage(Location.kind_, Reader);
        auto Saved = SaveDocumentBytes(*Writer, Location, Expected,
            std::as_bytes(std::span(Bytes.constData(), std::size_t(Bytes.size()))), Context);
        if (!Saved.HasValue()) return Saved.Failure();
        // A successful commit remains successful even if the post-commit query fails.
        auto Fresh = Reader->Resolve(Location, Context).get();
        LocalSaveResult Result; Result.receipt_ = std::move(Saved.Value());
        Result.hash_ = QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256);
        if (Fresh.HasValue()) {documentReadRevision(Fresh.Value(),Path);Result.source_ = std::move(Fresh.Value());}
        else Result.receipt_.cleanupErrors_.push_back(Fresh.Failure());
        return Result;
    }
}
