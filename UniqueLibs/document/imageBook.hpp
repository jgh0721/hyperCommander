#pragma once
#include "commonLibs/plugins/archiveBackend.hpp"
#include "imageReadingClassifier.hpp"
#include <QImage>
#include <QStringList>
#include <memory>
#include <mutex>
#include <QElapsedTimer>

namespace hc::document
{
struct ImagePage
{
    QString name_, file_, id_, sourceRevision_;
    plugins::ArchiveHeader entry_;
    std::uint32_t ordinal_ = 0;
    int subpage_ = 0;
    quint64 recoveredOffset_=0,recoveredPacked_=0,recoveredSize_=0;
    quint32 recoveredCrc_=0;
    int recoveredMethod_=-1;
};
struct ImageBook
{
    QString path_, revision_, title_, listingWarning_;
    ImageReadingAnalysis readingAnalysis_;
    bool subpagesPending_ = false;
    QList<ImagePage> pages_;
    std::shared_ptr<plugins::IArchiveBackend> archive_;
    // One consumer at a time retains decoder state across sequential pages.
    std::timed_mutex streamMutex_;
    QElapsedTimer streamAge_;
    std::unique_ptr<plugins::IArchiveReadSession> stream_;
    int nextStreamPage_ = -1, lastStreamPage_ = -1;
};
struct ImagePageContent
{
    QImage image_;
    QByteArray bytes_;
    QString format_, warning_;
};
QStringList ImageExtensions();
bool IsImageFile(const QString &Path);
bool IsImageBook(const QString &Path);
storage::Result<std::shared_ptr<ImageBook>> OpenImageBook(const QString &Path, storage::OperationContext Context = {}, bool ExpandTiff = true);
storage::Result<ImagePageContent> ReadImagePage(const std::shared_ptr<ImageBook> &Book, int Index,
                                                storage::OperationContext Context = {}, std::function<void(QImage)> Preview = {},
                                                std::function<void(QSize)> Metadata = {});
void AnalyzeImageBook(const std::shared_ptr<ImageBook>& Book, storage::OperationContext Context = {});
QStringList NextImageBooks(const QString &Path, int Direction = 1, std::stop_token Stop = {});
} // namespace hc::document
