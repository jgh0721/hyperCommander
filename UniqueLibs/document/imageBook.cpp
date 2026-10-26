#include "imageBook.hpp"
#include "imagePartialDecoder.hpp"
#include "imageZipRecovery.hpp"
#include "pluginHost/imageArchiveTransport.hpp"
#include <QJsonDocument>
#include "HyperCommanderBuiltInPlugin/SevenZip/builtinSevenZip.hpp"
#include "UniqueLibs/qtArchivePassword.hpp"
#include <QCollator>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QBuffer>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QDateTime>
#include <algorithm>
#include <QCoreApplication>
#include <Windows.h>

namespace
{
void InitializeImageCodecs()
{
    const auto Root = QCoreApplication::applicationDirPath() + "/Libs/imageformats/";
    // Preload only our pinned codecs by absolute path. Do not change the process DLL search path.
    for (const auto *Name : {"libde265.dll", "dav1d.dll", "heif.dll", "avif.dll"})
    {
        const auto Path = (Root + QLatin1String(Name)).toStdWString();
        if (!LoadLibraryExW(Path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
            QCoreApplication::instance()->setProperty("imageCodecLoadError", QString::number(GetLastError()));
    }
}
} // namespace
Q_COREAPP_STARTUP_FUNCTION(InitializeImageCodecs)

namespace hc::document
{
namespace
{
storage::Error Error(const char *Code)
{
    return {storage::ErrorCategory::InvalidRequest, Code, {"image-book"}};
}
bool Ignored(const QString &Path)
{
    for (const auto &Part : Path.split('/'))
        if (Part.startsWith('.') || Part.compare("__MACOSX", Qt::CaseInsensitive) == 0 ||
            Part.compare("Thumbs.db", Qt::CaseInsensitive) == 0)
            return true;
    return false;
}
QString Revision(const QString &Path)
{
    const QFileInfo Info(Path);
    return QString::number(Info.size()) + ":" + QString::number(Info.lastModified().toMSecsSinceEpoch());
}
struct Volume
{
    QString series_, unit_, edition_;
    qlonglong number_ = -1;
};
Volume ParseVolume(QString Name)
{
    Name = Name.normalized(QString::NormalizationForm_KC).toCaseFolded();
    const QRegularExpression Explicit("^(.*?)(?:\\s*(?:vol\\.?|v)\\s*(\\d+)|\\s*(\\d+)\\s*(권|화|회))(.*)$");
    auto Match = Explicit.match(Name);
    if (!Match.hasMatch())
    {
        Match = QRegularExpression("^(.*\\D)\\s+(\\d+)\\s*$").match(Name);
        if (!Match.hasMatch())
            return {};
        return {Match.captured(1).simplified(), "bare", {}, Match.captured(2).toLongLong()};
    }
    if (Match.captured(5).startsWith('.') || Match.captured(5).startsWith(','))
        return {};
    const auto Number = Match.captured(2).isEmpty() ? Match.captured(3) : Match.captured(2);
    auto Unit = Match.captured(4);
    if (Unit.isEmpty())
        Unit = "권";
    if (Unit == "회")
        Unit = "화";
    return {Match.captured(1).simplified(), Unit, Match.captured(5).simplified(), Number.toLongLong()};
}
} // namespace
QStringList ImageExtensions()
{
    QStringList Result;
    for (const auto &Format : QImageReader::supportedImageFormats())
    {
        auto Name = QString::fromLatin1(Format).toLower();
        if (Name != "pdf")
            Result << Name;
    }
    if (Result.contains("jpeg") || Result.contains("jpg"))
        Result << "jpg" << "jpeg" << "jpe";
    if (Result.contains("tiff") || Result.contains("tif"))
        Result << "tif" << "tiff";
    Result.removeDuplicates();
    Result.sort();
    return Result;
}
bool IsImageFile(const QString &Path)
{
    return ImageExtensions().contains(QFileInfo(Path).suffix().toLower());
}
bool IsImageBook(const QString &Path)
{
    return QStringList{"zip", "cbz", "rar", "cbr", "7z", "cb7"}.contains(QFileInfo(Path).suffix().toLower());
}
storage::Result<std::shared_ptr<ImageBook>> OpenImageBook(const QString &Path, storage::OperationContext Context, bool ExpandTiff)
{
    if (!Context.passwordPrompt_)
        Context.passwordPrompt_ = plugins::PromptArchivePassword;
    if (auto Stop = Context.Check({"image-book"}))
        return *Stop;
    const QFileInfo Info(Path);
    if (!Info.exists())
        return Error("image-book-not-found");
    auto Book = std::make_shared<ImageBook>();
    Book->path_ = Info.isDir()        ? Info.absoluteFilePath()
                  : IsImageBook(Path) ? Info.absoluteFilePath()
                                      : Info.absolutePath();
    Book->title_ = QFileInfo(Book->path_).completeBaseName();
    Book->revision_ = Revision(Book->path_);
    if (Info.isDir() || !IsImageBook(Path))
    {
        const QDir Dir(Book->path_);
        for (const auto &Entry : Dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot))
        {
            if (auto Stop = Context.Check({"image-book"}))
                return *Stop;
            if (IsImageFile(Entry.fileName()) && !Ignored(Entry.fileName()))
            {
                ImagePage Page;
                Page.file_ = Entry.absoluteFilePath();
                Page.name_ = Entry.fileName();
                Page.sourceRevision_ = Revision(Page.file_);
                Page.id_ = Page.name_ + ":" + Page.sourceRevision_;
                Book->pages_ << Page;
            }
        }
    }
    else
    {
        auto Options = plugins::BuiltinSevenZipOptions();
        Options.passwordPrompt_ = Context.passwordPrompt_;
        const auto EndpointBytes=QByteArray::fromBase64(qgetenv("HC_IMAGE_ARCHIVE_BROKER"));const auto Endpoint=QJsonDocument::fromJson(EndpointBytes);
        if(!EndpointBytes.isEmpty()){
            if(!Endpoint.isObject()||Endpoint.object()["address"].toString().isEmpty()||Endpoint.object()["token"].toString().size()!=32)return Error("image-archive-endpoint");
            Book->archive_=plugins::MakeImageArchiveClient(Endpoint.object());
        }else Book->archive_ = plugins::MakeApprovedWcxBackend(std::move(Options));
        auto Listing = Book->archive_->Enumerate(Book->path_.toUtf8().toStdString(), false, Context);
        if (!Listing.HasValue()) {
            if(auto Stop=Context.Check({"image-book"}))return *Stop;
            if(!QStringList{"zip","cbz"}.contains(Info.suffix().toLower()) || Listing.Failure().category_==storage::ErrorCategory::AccessDenied)return Listing.Failure();
            auto Recovered=zipRecovery::Scan(Book->path_,Context);if(!Recovered.HasValue())return Listing.Failure();
            for(const auto& E:Recovered.Value())if(IsImageFile(E.name_)&&!Ignored(E.name_)){
                ImagePage Page;Page.name_=E.name_;Page.ordinal_=Book->pages_.size();Page.id_=Book->revision_+":recovered:"+QString::number(E.offset_);
                Page.recoveredOffset_=E.offset_;Page.recoveredPacked_=E.compressed_;Page.recoveredSize_=E.size_;Page.recoveredCrc_=E.crc_;Page.recoveredMethod_=E.method_;
                Book->pages_.append(Page);
            }
            if(Book->pages_.isEmpty())return Listing.Failure();
            Book->listingWarning_="image-recovery-partial-directory";
        }else {
        std::uint32_t Ordinal = 0;
        for (;;)
        {
            auto Batch = Listing.Value()->Next(128, Context);
            if (!Batch.HasValue()) {
                if(auto Stop=Context.Check({"image-book"}))return *Stop;
                if(Book->pages_.isEmpty())return Batch.Failure();
                Book->listingWarning_=QString::fromStdString(Batch.Failure().code_);break;
            }
            for (const auto &Entry : Batch.Value().entries_)
            {
                const auto Name = QString::fromUtf8(Entry.name_).replace('\\', '/');
                if (!Entry.directory_ && IsImageFile(Name) && !Ignored(Name))
                {
                    ImagePage Page;
                    Page.name_ = Name;
                    Page.entry_ = Entry;
                    Page.ordinal_ = Ordinal;
                    Page.id_ = Book->revision_ + ":" + QString::number(Ordinal) + ":" + Name;
                    Book->pages_ << Page;
                }
                if (Ordinal == (std::numeric_limits<std::uint32_t>::max)())
                    return Error("image-book-ordinal-overflow");
                ++Ordinal;
            }
            if (Batch.Value().end_)
                break;
        }
        }
    }
    QCollator Sort;
    Sort.setNumericMode(true);
    Sort.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(Book->pages_.begin(), Book->pages_.end(),
              [&](const auto &A, const auto &B)
              {
                  const auto Left = A.name_.split('/'), Right = B.name_.split('/');
                  for (int I = 0; I < qMin(Left.size(), Right.size()); ++I)
                  {
                      const auto C = Sort.compare(Left[I], Right[I]);
                      if (C)
                          return C < 0;
                  }
                  if (Left.size() != Right.size())
                      return Left.size() < Right.size();
                  if (A.name_ != B.name_)
                      return A.name_ < B.name_;
                  return A.ordinal_ < B.ordinal_;
              });
    // TIFF pages are document pages, not timed animation frames.
    QList<ImagePage> Expanded;
    for (const auto &Page : std::as_const(Book->pages_))
    {
        Expanded << Page;
        const auto Suffix = QFileInfo(Page.name_).suffix().toLower();
        if (Suffix != "tif" && Suffix != "tiff")
            continue;
        if (auto Stop = Context.Check({"image-book"}))
            return *Stop;
        if(!ExpandTiff){Book->subpagesPending_=true;continue;}
        int Count = 1;
        if (Book->archive_)
        {
            const int Index = int(&Page - Book->pages_.constData());
            auto Content = ReadImagePage(Book, Index, Context);
            if (Content.HasValue())
            {
                QBuffer Buffer(&Content.Value().bytes_);
                if (Buffer.open(QIODevice::ReadOnly))
                {
                    QImageReader Reader(&Buffer);
                    Count = Reader.imageCount();
                }
            }
        }
        else
        {
            QImageReader Reader(Page.file_);
            Count = Reader.imageCount();
        }
        for (int Subpage = 1; Subpage < Count; ++Subpage)
        {
            auto Sub = Page;
            Sub.subpage_ = Subpage;
            Sub.id_ += QString(":subpage=%1").arg(Subpage);
            Expanded << Sub;
        }
    }
    Book->pages_ = std::move(Expanded);
    if (Book->pages_.isEmpty())
        return Error("image-book-no-images");
    return Book;
}
storage::Result<ImagePageContent> ReadImagePage(const std::shared_ptr<ImageBook> &Book, int Index,
                                                storage::OperationContext Context, std::function<void(QImage)> Preview)
{
    if (!Context.passwordPrompt_)
        Context.passwordPrompt_ = plugins::PromptArchivePassword;
    if (!Book || Index < 0 || Index >= Book->pages_.size())
        return Error("image-page-index");
    if (auto Stop = Context.Check({"image-book"}))
        return *Stop;
    const auto &Page = Book->pages_[Index];
    if (!Book->archive_ && Page.sourceRevision_ != Revision(Page.file_))
        return Error("image-page-changed");
    QString Path = Page.file_;
    QTemporaryDir Temp;
    QByteArray StreamBytes;
    ImagePartialDecoder Progressive;
    const auto Publish=[&](const QByteArray& Bytes){
        if(!Context.stopToken_.stop_requested())Progressive.Update(Bytes,Preview);
    };
    bool Streamed = false;
    QString Warning;
    if(Page.recoveredMethod_>=0){
        if(Revision(Book->path_)!=Book->revision_)return Error("image-book-changed");
        auto Recovered=zipRecovery::Read(Book->path_,{Page.name_,Page.recoveredOffset_,Page.recoveredPacked_,Page.recoveredSize_,Page.recoveredCrc_,quint16(Page.recoveredMethod_)},Context);
        if(!Recovered.HasValue())return Recovered.Failure();
        StreamBytes=std::move(Recovered.Value().bytes_);Warning=Recovered.Value().warning_;Streamed=true;
    }
    if (Book->archive_ && !Streamed)
    {
        std::unique_lock<std::timed_mutex> Lock(Book->streamMutex_,std::defer_lock);
        while(!Lock.try_lock_for(std::chrono::milliseconds(10)))if(auto Stop=Context.Check({"image-book"}))return *Stop;
        if (Revision(Book->path_) != Book->revision_)
            return Error("image-book-changed");
        storage::Result<storage::Unit> Session=storage::Unit{};
        if(!Book->stream_ || Book->nextStreamPage_!=Index || Book->streamAge_.elapsed()>5000){
            Book->stream_.reset();Book->nextStreamPage_=-1;
            std::vector<std::uint32_t> Ordinals{Page.ordinal_};Book->lastStreamPage_=Index;
            // Natural reading order may differ from archive order. Stop at an
            // inversion (or TIFF subpage); never skip an unread selected member.
            for(int I=Index+1;I<Book->pages_.size() && !Page.entry_.encrypted_ && Ordinals.size()<128;++I){
                if(Book->pages_[I].entry_.encrypted_ || Book->pages_[I].ordinal_<=Ordinals.back())break;
                Ordinals.push_back(Book->pages_[I].ordinal_);Book->lastStreamPage_=I;
            }
            auto Opened=Book->archive_->OpenReadSession(Book->path_.toUtf8().toStdString(),std::move(Ordinals),Context);
            if(Opened.HasValue()){Book->stream_=std::move(Opened.Value());Book->streamAge_.start();}else Session=Opened.Failure();
        }
        if (Session.HasValue())
        {
            for (;;)
            {
                auto Chunk = Book->stream_->Read(Page.ordinal_, 192 * 1024, Context);
                if (!Chunk.HasValue()){
                    Book->stream_.reset();Book->nextStreamPage_=-1;
                    if(auto Stop=Context.Check({"image-book"}))return *Stop;
                    if(StreamBytes.isEmpty() || Chunk.Failure().category_!=storage::ErrorCategory::Unavailable)return Chunk.Failure();
                    Warning=QString::fromStdString(Chunk.Failure().code_);break;
                }
                const auto &Data = Chunk.Value().bytes_;
                StreamBytes.append(reinterpret_cast<const char *>(Data.data()), Data.size());
                Publish(StreamBytes);
                if (Chunk.Value().eof_)
                    break;
                if (Data.empty())
                    return Error("image-page-incomplete");
            }
            Book->nextStreamPage_=Index+1;
            if(Index>=Book->lastStreamPage_){Book->stream_.reset();Book->nextStreamPage_=-1;}
            Streamed = true;
        }
        else
        {
            if (Session.Failure().category_ != storage::ErrorCategory::Unsupported)
                return Session.Failure();
            if (!Temp.isValid())
                return Error("image-page-temp");
            Path = Temp.filePath("page.bin");
            auto Extracted = Book->archive_->Extract(Book->path_.toUtf8().toStdString(), Page.entry_,
                                                     Path.toUtf8().toStdString(), Context);
            if (!Extracted.HasValue())
                return Extracted.Failure();
        }
    }
    QFile File(Path);
    if (!Streamed && !File.open(QIODevice::ReadOnly))
        return Error("image-page-open");
    ImagePageContent Content;
    Content.bytes_ = std::move(StreamBytes);Content.warning_=Warning;
    while (!Streamed && !File.atEnd())
    {
        if (auto Stop = Context.Check({"image-book"}))
            return *Stop;
        const auto Chunk = File.read(256 * 1024);
        if (Chunk.isEmpty() && !File.atEnd())
            return Error("image-page-read");
        Content.bytes_ += Chunk;Publish(Content.bytes_);
    }
    if (File.error() != QFileDevice::NoError)
        return Error("image-page-read");
    QBuffer Buffer(&Content.bytes_);
    Buffer.open(QIODevice::ReadOnly);
    QImageReader Reader(&Buffer);
    Reader.setAutoTransform(true);
    if (Page.subpage_ && !Reader.jumpToImage(Page.subpage_))
        return Error("image-subpage");
    Content.image_ = Reader.read();
    Content.format_ = QString::fromLatin1(Reader.format());
    if(Content.bytes_.startsWith(QByteArray("\xff\xd8",2)) && !Content.bytes_.right(64).contains(QByteArray("\xff\xd9",2)))
        Content.warning_="image-jpeg-incomplete";
    if (Content.image_.isNull())
        return Error("image-page-decode");
    if (auto Stop = Context.Check({"image-book"}))
        return *Stop;
    if (Book->archive_ ? Revision(Book->path_) != Book->revision_ : Page.sourceRevision_ != Revision(Page.file_))
        return Error("image-page-changed");
    return Content;
}
QStringList NextImageBooks(const QString &Path, int Direction, std::stop_token Stop)
{
    const QFileInfo Current(Path);
    const auto Parsed = ParseVolume(Current.isDir() ? Current.fileName() : Current.completeBaseName());
    if (Parsed.number_ < 0 || Parsed.series_.isEmpty())
        return {};
    QStringList Result;
    for (const auto &Entry : Current.dir().entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot))
    {
        if (Stop.stop_requested())
            return {};
        if (!Entry.isDir() && !IsImageBook(Entry.fileName()))
            continue;
        const auto Candidate = ParseVolume(Entry.isDir() ? Entry.fileName() : Entry.completeBaseName());
        if (Candidate.series_ == Parsed.series_ && Candidate.unit_ == Parsed.unit_ &&
            Candidate.edition_ == Parsed.edition_ && Candidate.number_ == Parsed.number_ + Direction)
            Result << Entry.absoluteFilePath();
    }
    Result.sort();
    return Result;
}
} // namespace hc::document
