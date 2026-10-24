#include "stdafx.h"
#include "epubArchive.hpp"

/// Framework headers
#include <QSet>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QtEndian>
#include <algorithm>
#include <zlib.h>

namespace hc::document::epub::detail
{
    namespace
    {
        quint16 u16(const QByteArray& Bytes, qsizetype At) { return qFromLittleEndian<quint16>(Bytes.constData() + At); }
        quint32 u32(const QByteArray& Bytes, qsizetype At) { return qFromLittleEndian<quint32>(Bytes.constData() + At); }
        void range(const QByteArray& Bytes, qsizetype At, qsizetype Count)
        { if(At < 0 || Count < 0 || At > Bytes.size() || Count > Bytes.size() - At) Reject("zip-truncated"); }
        void extra(const QByteArray& Bytes, qsizetype At, qsizetype Count)
        {
            const auto End = At + Count;
            while(At < End) {
                if(End - At < 4) Reject("zip-extra-truncated");
                const auto Kind = u16(Bytes, At), Size = u16(Bytes, At + 2);
                At += 4;
                if(Kind == 1) Reject("zip64-unsupported");
                if(Size > End - At) Reject("zip-extra-truncated");
                At += Size;
            }
        }
    }

    QString Utf8(const QByteArray& Bytes, const QString& Location)
    {
        QStringDecoder Decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        const QString Text = Decoder.decode(Bytes);
        if(Decoder.hasError()) Reject("invalid-utf8", Location);
        return Text;
    }

    bool PassiveDoctype(const QXmlStreamReader& Reader)
    {
        // Legacy EPUB2 XHTML declarations are labels only. Never admit an
        // internal subset, custom entities, or an arbitrary external DTD.
        static const QRegularExpression Declaration(QStringLiteral(
            R"hc(^<!DOCTYPE\s+html(?:\s+PUBLIC\s+(["'])(-//W3C//DTD XHTML (?:1[.]1|1[.]0 (?:Strict|Transitional))//EN)\1\s+(["'])(http://www[.]w3[.]org/TR/(?:xhtml11/DTD/xhtml11[.]dtd|xhtml1/DTD/xhtml1-(?:strict|transitional)[.]dtd))\3)?\s*>$)hc"));
        static const QRegularExpression Ncx(QStringLiteral(
            R"hc(^<!DOCTYPE\s+ncx\s+PUBLIC\s+(["'])-//NISO//DTD ncx 2005-1//EN\1\s+(["'])http://www[.]daisy[.]org/z3986/2005/ncx-2005-1[.]dtd\2\s*>$)hc"));
        const auto Text = Reader.text().trimmed();
        return Reader.entityDeclarations().isEmpty() && Reader.notationDeclarations().isEmpty()
            && (Declaration.match(Text).hasMatch() || Ncx.match(Text).hasMatch());
    }

    QString Canonical(const QString& Name)
    {
        if(Name.toUtf8().size() > 4096) Reject("entry-path-limit");
        if(Name.isEmpty() || Name.startsWith('/') || Name.contains('\\') || Name.contains(':') || Name.contains(QChar(0)))
            Reject("unsafe-entry-path", Name);
        const auto Path = Name.endsWith('/') ? Name.chopped(1) : Name;
        for(const auto& Part : Path.split('/')) if(Part.isEmpty() || Part == "." || Part == "..") Reject("unsafe-entry-path", Name);
        return Path;
    }

    Archive::Archive(const QByteArray& Bytes, const Cancellation& Cancel) : bytes_(Bytes), cancel_(Cancel)
    {
        Check(cancel_);
        if(Bytes.size() > InputLimit) Reject("input-limit");
        qsizetype End = -1, ArchiveEnd = Bytes.size();
        // Some EPUB exporters leave one PKCS#7 block's padding after EOCD.
        // Accept only a complete 1..16 byte repeated padding value; arbitrary
        // trailers and truncated comments must still fail. Check ZIP first so
        // an actual archive comment ending in padding-like bytes is preserved.
        const auto FindEnd = [&](qsizetype Size) -> qsizetype {
            for(qsizetype At = Size - 22; At >= std::max<qsizetype>(0, Size - 65557); --At)
                if(u32(Bytes, At) == 0x06054b50 && At + 22 + u16(Bytes, At + 20) == Size) return At;
            return -1;
        };
        End = FindEnd(ArchiveEnd);
        if(End < 0 && !Bytes.isEmpty()) {
            const auto Padding = static_cast<unsigned char>(Bytes.back());
            if(Padding >= 1 && Padding <= 16 && Bytes.size() >= Padding
                && Bytes.right(Padding) == QByteArray(Padding, char(Padding))) {
                ArchiveEnd -= Padding;
                End = FindEnd(ArchiveEnd);
            }
        }
        if(End < 0) Reject("zip-end-missing");
        comment_=Bytes.mid(End+22, ArchiveEnd-End-22);
        const auto Count = u16(Bytes, End + 10);
        if(u16(Bytes, End + 4) || u16(Bytes, End + 6) || u16(Bytes, End + 8) != Count) Reject("zip-multidisk-unsupported");
        if(!Count || Count > 4096) Reject("entry-count-limit");
        const qsizetype Central = u32(Bytes, End + 16), CentralSize = u32(Bytes, End + 12);
        if(Central > End || CentralSize != End - Central) Reject("zip-central-range");
        qsizetype At = Central;
        quint64 Total = 0;
        QSet<QString> Folded;
        QList<QPair<qsizetype, qsizetype>> Ranges;
        for(int Index = 0; Index < Count; ++Index) {
            Check(cancel_);
            range(Bytes, At, 46);
            if(At + 46 > End || u32(Bytes, At) != 0x02014b50) Reject("zip-central-header");
            const auto Flags = u16(Bytes, At + 8), Method = u16(Bytes, At + 10);
            const auto NameSize = u16(Bytes, At + 28), ExtraSize = u16(Bytes, At + 30), CommentSize = u16(Bytes, At + 32);
            const qsizetype Next = At + 46 + NameSize + ExtraSize + CommentSize;
            if(Next > End) Reject("zip-central-range");
            if(Flags & 1) Reject("encrypted-zip-entry");
            if(Flags & ~quint16(0x080e)) Reject("zip-flags-unsupported");
            if(Method == 0 && (Flags & 6)) Reject("zip-flags-unsupported");
            if(Method != 0 && Method != 8) Reject("unsupported-zip-method");
            if(u16(Bytes, At + 34)) Reject("zip-multidisk-unsupported");
            const auto NameBytes = Bytes.mid(At + 46, NameSize);
            if(!(Flags & 0x0800) && std::any_of(NameBytes.cbegin(), NameBytes.cend(), [](char C) { return static_cast<unsigned char>(C) >= 128; }))
                Reject("zip-name-encoding-unsupported");
            const auto Name = Utf8(NameBytes, "zip-name"), Path = Canonical(Name);
            if(entries_.contains(Path) || Folded.contains(Path.toCaseFolded())) Reject("duplicate-entry", Path);
            Folded.insert(Path.toCaseFolded());
            const auto Mode = u32(Bytes, At + 38) >> 16, Kind = Mode & 0170000;
            if(Kind && Kind != 0100000 && Kind != 0040000) Reject("zip-special-entry", Path);
            Entry Item{Path, u32(Bytes, At + 16), u32(Bytes, At + 20), u32(Bytes, At + 24), u32(Bytes, At + 42), Method, Name.endsWith('/')};
            Item.central_=Bytes.mid(At,Next-At);
            Total += Item.size_;
            if(Item.size_ > 16 * 1024 * 1024 || Total > 128 * 1024 * 1024) Reject("decompressed-size-limit", Path);
            if(quint64(Item.size_) > quint64(std::max(1U, Item.compressed_)) * 200) Reject("compression-ratio-limit", Path);
            if(Item.directory_ && (Item.size_ || Item.compressed_ || Item.crc_)) Reject("zip-directory-content", Path);
            if(Kind == 0040000 && !Item.directory_) Reject("zip-directory-name", Path);
            extra(Bytes, At + 46 + NameSize, ExtraSize);
            const qsizetype Local = Item.offset_;
            range(Bytes, Local, 30);
            if(Local + 30 > Central || u32(Bytes, Local) != 0x04034b50 || u16(Bytes, Local + 6) != Flags || u16(Bytes, Local + 8) != Method)
                Reject("zip-local-header", Path);
            const auto LocalName = u16(Bytes, Local + 26), LocalExtra = u16(Bytes, Local + 28);
            Item.data_ = Local + 30 + LocalName + LocalExtra;
            if(Item.data_ > Central || LocalName != NameSize || Bytes.mid(Local + 30, LocalName) != NameBytes) Reject("zip-local-name", Path);
            extra(Bytes, Local + 30 + LocalName, LocalExtra);
            Item.end_ = Item.data_ + Item.compressed_;
            if(Item.end_ > Central) Reject("zip-data-range", Path);
            if(Flags & 8) {
                const auto Start = Item.end_;
                if(Central - Start < 12) Reject("zip-descriptor", Path);
                const auto Signature = u32(Bytes, Start) == 0x08074b50 ? 4 : 0;
                if(Central - Start < Signature + 12 || u32(Bytes, Start + Signature) != Item.crc_
                    || u32(Bytes, Start + Signature + 4) != Item.compressed_ || u32(Bytes, Start + Signature + 8) != Item.size_)
                    Reject("zip-descriptor", Path);
                Item.end_ += Signature + 12;
                if((u32(Bytes, Local + 14) && u32(Bytes, Local + 14) != Item.crc_)
                    || (u32(Bytes, Local + 18) && u32(Bytes, Local + 18) != Item.compressed_)
                    || (u32(Bytes, Local + 22) && u32(Bytes, Local + 22) != Item.size_)) Reject("zip-local-size", Path);
            } else if(u32(Bytes, Local + 14) != Item.crc_ || u32(Bytes, Local + 18) != Item.compressed_ || u32(Bytes, Local + 22) != Item.size_)
                Reject("zip-local-size", Path);
            // Compatibility: compressed/descriptor-backed mimetype is safe
            // after the same size, descriptor and CRC checks as every entry.
            if(Path == "mimetype" && (Local || LocalExtra)) Reject("invalid-mimetype-layout");
            Ranges.append({Local, Item.end_}); entries_.insert(Path, Item); At = Next;
        }
        if(At != End) Reject("zip-central-count");
        std::sort(Ranges.begin(), Ranges.end());
        for(qsizetype Index = 1; Index < Ranges.size(); ++Index)
            if(Ranges[Index].first < Ranges[Index - 1].second) Reject("zip-overlap");
        if(Read("mimetype") != "application/epub+zip") Reject("invalid-mimetype-value");
    }

    QByteArray Archive::Rewrite(const QMap<QString,QByteArray>& Replacements) const {
        for(auto I=Replacements.begin();I!=Replacements.end();++I)if(!entries_.contains(I.key()) || entries_[I.key()].directory_ || I.key()=="mimetype" || I.value().size()>16*1024*1024)Reject("edit-resource-denied",I.key());
        auto Ordered=entries_.values();std::sort(Ordered.begin(),Ordered.end(),[](const Entry& A,const Entry& B){return A.offset_<B.offset_;});
        QByteArray Output,Central;
        const auto Put16=[](QByteArray& B,qsizetype At,quint16 V){qToLittleEndian(V,B.data()+At);};
        const auto Put32=[](QByteArray& B,qsizetype At,quint32 V){qToLittleEndian(V,B.data()+At);};
        for(const auto& Item:Ordered) {
            Check(cancel_);auto Record=Item.central_;Put32(Record,42,quint32(Output.size()));
            if(!Replacements.contains(Item.path_))Output+=bytes_.mid(Item.offset_,Item.end_-Item.offset_);
            else {
                const auto Data=Replacements[Item.path_];const auto Crc=quint32(crc32(0,reinterpret_cast<const Bytef*>(Data.constData()),uInt(Data.size())));
                auto Header=bytes_.mid(Item.offset_,Item.data_-Item.offset_);const auto Flags=quint16(u16(Header,6)&~quint16(14));
                Put16(Header,6,Flags);Put16(Header,8,0);Put32(Header,14,Crc);Put32(Header,18,quint32(Data.size()));Put32(Header,22,quint32(Data.size()));
                Put16(Record,8,Flags);Put16(Record,10,0);Put32(Record,16,Crc);Put32(Record,20,quint32(Data.size()));Put32(Record,24,quint32(Data.size()));Output+=Header;Output+=Data;
            }
            Central+=Record;if(Output.size()+Central.size()+22+comment_.size()>InputLimit)Reject("edit-output-limit");
        }
        const auto Offset=Output.size();Output+=Central;QByteArray End(22,0);Put32(End,0,0x06054b50);Put16(End,8,quint16(Ordered.size()));Put16(End,10,quint16(Ordered.size()));Put32(End,12,quint32(Central.size()));Put32(End,16,quint32(Offset));Put16(End,20,quint16(comment_.size()));Output+=End;Output+=comment_;return Output;
    }

    QByteArray Archive::Read(const QString& Path) const
    {
        Check(cancel_);
        const auto Found = entries_.constFind(Path);
        if(Found == entries_.cend() || Found->directory_) Reject("missing-resource", Path);
        const auto& Item = Found.value();
        QByteArray Result;
        if(Item.method_ == 0) {
            if(Item.compressed_ != Item.size_) Reject("zip-stored-size", Path);
            Result = bytes_.mid(Item.data_, Item.size_);
        } else {
            Result.resize(qsizetype(Item.size_) + 1);
            z_stream Stream{};
            Stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes_.constData() + Item.data_));
            Stream.avail_in = Item.compressed_;
            if(inflateInit2(&Stream, -MAX_WBITS) != Z_OK) Reject("zip-inflate-init", Path);
            struct EndInflate { z_stream* stream_; ~EndInflate() { inflateEnd(stream_); } } Cleanup{&Stream};
            int Status = Z_OK;
            while(Status == Z_OK) {
                Check(cancel_);
                const auto Remaining = Result.size() - qsizetype(Stream.total_out);
                if(Remaining <= 0) Reject("zip-inflate-size", Path);
                Stream.next_out = reinterpret_cast<Bytef*>(Result.data() + Stream.total_out);
                Stream.avail_out = static_cast<uInt>(std::min<qsizetype>(Remaining, 65536));
                Status = inflate(&Stream, Z_NO_FLUSH);
            }
            if(Status != Z_STREAM_END || Stream.total_out != Item.size_ || Stream.total_in != Item.compressed_) Reject("zip-inflate-size", Path);
            Result.resize(Item.size_);
        }
        Check(cancel_);
        const auto Crc = crc32(0, reinterpret_cast<const Bytef*>(Result.constData()), static_cast<uInt>(Result.size()));
        if(Crc != Item.crc_) Reject("zip-crc", Path);
        return Result;
    }

    void Archive::VerifyAll() const
    {
        for(const auto& Item : entries_) {
            Check(cancel_);
            if(!Item.directory_) Read(Item.path_);
        }
    }
}
