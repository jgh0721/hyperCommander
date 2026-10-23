#pragma once
#include "core/compare.hpp"
#include <miniz.h>
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace hc::compare
{
    // Memory-only reader for archive and Office workers. No filesystem extraction API is linked.
    class ZipArchive final
    {
    public:
        struct Entry { QString path_; int ordinal_ = 0; bool directory_ = false; quint64 size_ = 0; };
        ZipArchive(QByteArray Source, const Budget& Limits);
        ~ZipArchive();
        ZipArchive(const ZipArchive&) = delete;
        ZipArchive& operator=(const ZipArchive&) = delete;
        const QVector<Entry>& Entries() const { return entries_; }
        QString Digest() const { return digest_; }
        QByteArray Read(int Ordinal, const Budget& Limits);
        QString Hash(int Ordinal, const Budget& Limits);
        QJsonObject Location(int Ordinal, const QString& Digest) const;
    private:
        QByteArray extract(int Ordinal, const Budget& Limits, bool HashOnly);
        QByteArray source_;
        mz_zip_archive archive_{};
        QVector<Entry> entries_;
        QString digest_;
    };
    QJsonObject ArchiveWorkerRequest(const QJsonObject& Payload);
}
