#pragma once
#include "archiveBackend.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>

namespace hc::plugins
{
    class ArchiveSelectionDigest
    {
    public:
        void Add(std::uint32_t Ordinal, const ArchiveHeader& Header)
        {
            std::string Fields;
            storage::AppendField(Fields, std::to_string(Ordinal));
            storage::AppendField(Fields, Header.name_);
            storage::AppendField(Fields, std::to_string(Header.size_));
            storage::AppendField(Fields, std::to_string(Header.crc_));
            storage::AppendField(Fields, Header.directory_ ? "1" : "0");
            hash_.addData(QByteArrayView(Fields.data(), qsizetype(Fields.size())));
        }
        QByteArray Result() const { return hash_.result().toHex(); }
        storage::Result<storage::Unit> Save(const QString& Directory) const
        {
            QFile File(QDir(Directory).filePath("selection.sha256"));
            if (!File.open(QIODevice::WriteOnly | QIODevice::NewOnly) || File.write(Result()) != 64 || !File.flush())
                return storage::Error{storage::ErrorCategory::Unavailable, "archive-selected-digest-write"};
            return storage::Unit{};
        }
    private:
        QCryptographicHash hash_{QCryptographicHash::Sha256};
    };
}
