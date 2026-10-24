#pragma once
#include "epubPackage.hpp"

/// Framework headers
#include <QMap>

namespace hc::document::epub::detail
{
    struct Rejected { QString code_; QString location_; };
    [[noreturn]] inline void Reject(const QString& Code, const QString& Location = {}) { throw Rejected{Code, Location}; }
    inline void Check(const Cancellation& Cancel) { if(Cancel && Cancel()) Reject("cancelled"); }
    QString Canonical(const QString& Name);
    QString Utf8(const QByteArray& Bytes, const QString& Location);

    struct Entry
    {
        QString path_;
        quint32 crc_ = 0, compressed_ = 0, size_ = 0, offset_ = 0;
        quint16 method_ = 0;
        bool directory_ = false;
        qsizetype data_ = 0, end_ = 0;
    };

    // Borrows an immutable snapshot for the duration of Prepare().
    class Archive final
    {
    public:
        Archive(const QByteArray& Bytes, const Cancellation& Cancel);
        QByteArray Read(const QString& Path) const;
        bool Contains(const QString& Path) const { return entries_.contains(Path); }
        void VerifyAll() const;
    private:
        const QByteArray& bytes_;
        const Cancellation& cancel_;
        QMap<QString, Entry> entries_;
    };
}
