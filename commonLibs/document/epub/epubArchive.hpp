#pragma once
#include "epubPackage.hpp"

/// Framework headers
#include <QMap>
#include <QSet>
#include <QXmlStreamReader>
#include <utility>

namespace hc::document::epub::detail
{
    struct Rejected { QString code_; QString location_; };
    [[noreturn]] inline void Reject(const QString& Code, const QString& Location = {}) { throw Rejected{Code, Location}; }
    inline void Check(const Cancellation& Cancel) { if(Cancel && Cancel()) Reject("cancelled"); }
    bool PassiveDoctype(const QXmlStreamReader& Reader);
    class XhtmlEntities final : public QXmlStreamEntityResolver
    {
    public:
        bool enabled_ = false;
        QString resolveUndeclaredEntity(const QString& Name) override
        { return enabled_ && Name == "nbsp" ? QString(QChar(0x00a0)) : QString{}; }
    };
    QString Canonical(const QString& Name);
    QString Utf8(const QByteArray& Bytes, const QString& Location);

    struct Entry
    {
        QString path_;
        quint32 crc_ = 0, compressed_ = 0, size_ = 0, offset_ = 0;
        quint16 method_ = 0;
        bool directory_ = false;
        qsizetype data_ = 0, end_ = 0;
        QByteArray central_;
    };

    // Canonical stored ZIP output: UTF-8 names, method 0, no data descriptors.
    // EPUB requires mimetype as the first entry; the caller adds it first.
    class StoredZipWriter final
    {
    public:
        explicit StoredZipWriter(Cancellation Cancel, qsizetype CommentSize = 0)
            : cancel_(std::move(Cancel)), commentSize_(CommentSize) {}
        quint32 Offset() const { return quint32(output_.size()); }
        void Add(const QString& Path, const QByteArray& Data);
        // A prepared local header + data and its central record (offset already patched).
        void Append(const QByteArray& Local, const QByteArray& Central);
        QByteArray Finish(const QByteArray& Comment = {});
    private:
        void budget() const;
        Cancellation cancel_;
        qsizetype commentSize_ = 0;
        QByteArray output_, central_;
        int count_ = 0;
    };

    // Epub: an EPUB container (mimetype entry, UTF-8 names, Rewrite emits the stored mimetype first).
    // General: any ZIP within the same limits. Names without the UTF-8 flag decode with the local code page,
    // there is no mimetype requirement, and Rewrite keeps the entry order as it is.
    enum class ZipMode { Epub, General };

    // Implicitly shared immutable bytes and a value-owned cancellation callback.
    class Archive final
    {
    public:
        Archive(const QByteArray& Bytes, const Cancellation& Cancel, qsizetype EntryLimit = 16 * 1024 * 1024, ZipMode Mode = ZipMode::Epub);
        qsizetype Size(const QString& Path) const { return entries_.contains(Path) ? entries_.value(Path).size_ : -1; }
        QByteArray Read(const QString& Path) const;
        bool Contains(const QString& Path) const { return entries_.contains(Path); }
        void VerifyAll() const;
        void ValidateResourceLimits(const QSet<QString>& Fonts) const;
        QStringList CompatibilityWarnings() const { return warnings_; }
        QByteArray Rewrite(const QMap<QString,QByteArray>& Replacements,const QMap<QString,QByteArray>& Additions={},const QSet<QString>& Removals={}) const;
    private:
        QByteArray readEntry(const Entry& Item) const;
        QByteArray bytes_;
        Cancellation cancel_;
        QMap<QString, Entry> entries_;
        QByteArray comment_;
        QStringList warnings_;
        ZipMode mode_ = ZipMode::Epub;
    };
}
