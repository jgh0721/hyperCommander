#pragma once
#include "commonLibs/document/documentSaveCoordinator.hpp"
#include "commonLibs/document/documentTypes.hpp"
#include "documentTextCodec.hpp"
#include "commonLibs/document/documentRecovery.hpp"
#include <QByteArray>
#include <QString>
#include <QImage>
#include <QJsonObject>

namespace hc::document
{
    // Captured on the GUI owner; called only by a worker, without live widgets.
    using DocumentEncoder = std::function<storage::Result<QByteArray>(storage::OperationContext)>;
    struct ImageBook;
    struct TextInput
    {
        QString path_, text_;
        QByteArray hash_, bytes_;
        QImage image_;
        std::shared_ptr<ImageBook> book_;
        QString viewId_ = "text", encoding_ = "UTF-8";
        bool bom_ = false;
        // How encoding_ was chosen (text only): detected, or named by "다른 인코딩으로 다시 읽기".
        TextDetection detection_ = TextDetection::Explicit;
        QString eol_ = "CRLF";
        storage::EntrySnapshot source_;
        std::optional<SourceIdentity> recoveryOrigin_;
        std::optional<RecoverySnapshot> recoveryRecord_;
        bool recoveryOriginalMatched_ = false;
    };
    struct LocalSaveResult
    {
        storage::WriteReceipt receipt_;
        storage::EntrySnapshot source_;
        QByteArray hash_;
    };
    storage::Result<TextInput> ReadDocumentFile(const QString& Path, storage::OperationContext Context = {}, bool Unlimited = false, std::function<void(QImage)> Preview = {}, QSize PreviewBounds = {1024,1024}, std::function<void(QString)> TextPreview = {}, QJsonObject ReadPrefix = {});
    storage::Result<TextInput> ReadTextFile(const QString& Path, storage::OperationContext Context = {});
    // Decodes Input.bytes_ (detected when Encoding is empty) into text_, encoding_, bom_ and eol_.
    storage::Result<TextInput> DecodeTextInput(TextInput Input, const QString& Encoding = {});
    storage::Result<LocalSaveResult> SaveTextFile(const QString& Path, const QByteArray& Bytes,
        storage::DestinationExpectation Expected, const QByteArray& ExpectedHash,
        storage::OperationContext Context = {}, bool Image = false, bool Unlimited = false);
    QByteArray EncodeText(const QString& Text, bool Bom, const QString& Eol);
}
