#pragma once
#include "core/compare.hpp"
#include "commonLibs/storage/storage.hpp"
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <array>
#include <mutex>
#include <deque>
#include <QMap>
#include <QStringList>

namespace hc::compare
{
    struct Request;
    struct Result;
    struct DirectoryPolicy
    {
        QStringList include_, exclude_;
        QString level_ = "bytes";
    };
    struct DirectoryEntry
    {
        QString path_;
        std::optional<storage::ResourceRevision> revision_;
        std::uint64_t size_ = 0;
        std::int64_t createdUtcMs_ = 0, modifiedUtcMs_ = 0;
        std::uint32_t attributes_ = 0;
        bool directory_ = false;
    };
    // Keep native locators and provider session IDs out of the per-entry inventory.
    using DirectoryTree = QMap<QString, DirectoryEntry>;
    struct DirectoryRow
    {
        struct Side { QString path_; std::uint64_t size_ = 0; std::int64_t modified_ = 0; bool present_ = false, directory_ = false; };
        QString path_;
        std::array<Side, 3> sides_;
        std::array<quint8, 4> statuses_{};
        QJsonArray errors_;
        DirectoryRow() = default;
        explicit DirectoryRow(const QJsonObject& Object);
        QJsonObject toObject() const;
        QString Status(int Index = 0) const;
    };
    // Implicit sharing preserves cheap Result/model copies; JSON is materialized only on demand.
    class DirectoryEntries : public QList<DirectoryRow>
    {
    public:
        DirectoryEntries() = default;
        DirectoryEntries(const QJsonArray& Array) { reserve(Array.size()); for (const auto& value : Array) append(value.toObject()); }
        using QList<DirectoryRow>::append;
        void append(const QJsonObject& Object) { QList<DirectoryRow>::append(DirectoryRow(Object)); }
        QJsonArray Json() const { QJsonArray result; for (const auto& row : *this) result.append(row.toObject()); return result; }
    };
    struct DirectoryProgress
    {
        DirectoryEntries rows_;
        QString phase_;
        int side_ = -1;
        qsizetype processed_ = 0, total_ = 0;
    };
    class DirectoryProgressChannel
    {
    public:
        void Push(DirectoryProgress Value, const Budget& Limits);
        std::optional<DirectoryProgress> Take();
    private:
        std::mutex mutex_;
        std::deque<DirectoryProgress> queue_;
    };
    struct DirectorySnapshot
    {
        QString root_;
        storage::EntrySnapshot entry_;
        DirectoryTree entries_;
        storage::EntrySnapshot Expand(const DirectoryEntry& Entry) const;
        QMap<QString, QString> issues_; // Incomplete subtrees, keyed by folded relative path.
    };
    DirectorySnapshot CaptureDirectory(const QString& Root, const DirectoryPolicy& Policy, const Budget& Limits, bool AllowIncomplete = false, const std::function<void(const DirectoryEntry&)>& Discovered = {});
    void ValidateDirectory(const DirectorySnapshot& Snapshot, const DirectoryPolicy& Policy, const Budget& Limits);
    void CompareDirectory(const Request& Input, Result& Value, const Budget& Limits);
}
