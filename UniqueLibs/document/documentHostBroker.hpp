#pragma once
#include "commonLibs/storage/materializedContent.hpp"
#include "commonLibs/document/documentSaveCoordinator.hpp"
#include <QJsonObject>
#include <map>
#include <set>

namespace hc::document
{
    // HC worker-side authority. Helper tokens never authorize an arbitrary path.
    // The binding owns its source/lease until save and session retirement finish.
    class DocumentHostBroker final
    {
    public:
        QString Bind(storage::EntrySnapshot Source, std::shared_ptr<storage::IWritableStorage> Writer,
            std::shared_ptr<storage::IMaterializedFileLease> Lease = {}, QByteArray ContentHash = {});
        QJsonObject Commit(const QJsonObject& Prepared, storage::OperationContext Context = {});
        QJsonObject CommitBytes(const QJsonObject& Prepared, const QByteArray& Bytes, storage::OperationContext Context = {});
        void Retain(const std::set<QString>& Tokens);
        void Close();
        struct EditorInput {
            QString path_;
            bool original_ = false;
            std::shared_ptr<storage::IMaterializedFileLease> lease_;
        };
        std::optional<EditorInput> ResolveEditorInput(const QString& SourceToken) const;
    private:
        struct Binding
        {
            storage::EntrySnapshot source_;
            std::shared_ptr<storage::IWritableStorage> writer_;
            std::shared_ptr<storage::IMaterializedFileLease> lease_;
            QByteArray hash_;
        };
        std::map<QString, Binding> sources_;
        std::set<QString> retiredSaves_;
    };
}
