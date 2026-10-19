#pragma once
#include "commonLibs/plugins/archiveBackend.hpp"
#include <QImage>
#include <QStringList>
#include <memory>

namespace hc::document
{
struct ImagePage
{
    QString name_, file_, id_, sourceRevision_;
    plugins::ArchiveHeader entry_;
    std::uint32_t ordinal_ = 0;
    int subpage_ = 0;
};
struct ImageBook
{
    QString path_, revision_, title_;
    QList<ImagePage> pages_;
    std::shared_ptr<plugins::IArchiveBackend> archive_;
};
struct ImagePageContent
{
    QImage image_;
    QByteArray bytes_;
    QString format_;
};
QStringList ImageExtensions();
bool IsImageFile(const QString &Path);
bool IsImageBook(const QString &Path);
storage::Result<std::shared_ptr<ImageBook>> OpenImageBook(const QString &Path, storage::OperationContext Context = {});
storage::Result<ImagePageContent> ReadImagePage(const std::shared_ptr<ImageBook> &Book, int Index,
                                                storage::OperationContext Context = {});
QStringList NextImageBooks(const QString &Path, int Direction = 1, std::stop_token Stop = {});
} // namespace hc::document
