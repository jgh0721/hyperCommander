#pragma once
#include "commonLibs/document/documentSaveCoordinator.hpp"
#include "commonLibs/document/documentTypes.hpp"
#include <QByteArray>
#include <QString>
#include <QImage>

namespace hc::document
{
    struct ImageBook;
    struct TextInput
    {
        QString path_, text_;
        QByteArray hash_, bytes_;
        QImage image_;
        std::shared_ptr<ImageBook> book_;
        QString viewId_ = "text", encoding_ = "UTF-8";
        bool bom_ = false;
        QString eol_ = "CRLF";
        storage::EntrySnapshot source_;
        std::optional<SourceIdentity> recoveryOrigin_;
    };
    struct LocalSaveResult
    {
        storage::WriteReceipt receipt_;
        storage::EntrySnapshot source_;
        QByteArray hash_;
    };
    storage::Result<TextInput> ReadDocumentFile(const QString& Path, storage::OperationContext Context = {});
    storage::Result<TextInput> ReadTextFile(const QString& Path, storage::OperationContext Context = {});
    storage::Result<LocalSaveResult> SaveTextFile(const QString& Path, const QByteArray& Bytes,
        storage::DestinationExpectation Expected, const QByteArray& ExpectedHash,
        storage::OperationContext Context = {}, bool Image = false);
    QByteArray EncodeText(const QString& Text, bool Bom, const QString& Eol);
}
