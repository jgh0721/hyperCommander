#pragma once
#include "commonLibs/storage/storage.hpp"
#include <QFile>
#include <QList>
#include <QtEndian>
#include <zlib.h>

namespace hc::document::zipRecovery
{
    struct Entry
    {
        QString name_;
        quint64 offset_=0, compressed_=0, size_=0;
        quint32 crc_=0;
        quint16 method_=0;
    };
    struct Content { QByteArray bytes_;QString warning_; };
    inline storage::Error Error(const char* Code){return {storage::ErrorCategory::Unavailable,Code,{"image-recovery"}};}
    // Recovery is a read-only preview path. It never changes normal extraction's
    // CRC contract, writes a repaired archive, or trusts names as filesystem paths.
    inline storage::Result<QList<Entry>> Scan(const QString& Path,storage::OperationContext Context)
    {
        QFile File(Path);if(!File.open(QIODevice::ReadOnly))return Error("image-recovery-open");
        QList<Entry> Entries;qint64 Position=0;
        while(Position+30<=File.size()){
            if(auto Stop=Context.Check({"image-recovery"}))return *Stop;
            if(!File.seek(Position))break;
            const auto Block=File.read(64*1024);const auto Found=Block.indexOf(QByteArray("PK\x03\x04",4));
            if(Found<0){if(Block.size()<=3)break;Position+=Block.size()-3;continue;}
            Position+=Found;if(!File.seek(Position))break;
            const auto Header=File.read(30);if(Header.size()!=30)break;
            const auto U16=[&](int At){return qFromLittleEndian<quint16>(Header.constData()+At);};
            const auto U32=[&](int At){return qFromLittleEndian<quint32>(Header.constData()+At);};
            const auto Flags=U16(6),Method=U16(8),NameSize=U16(26),Extra=U16(28);
            const quint64 Packed=U32(18),Size=U32(22),Offset=quint64(Position)+30+NameSize+Extra;
            // Unknown descriptor sizes, ZIP64 and encrypted members need the
            // archive engine. Resynchronise to later recoverable local headers.
            if(U16(4)>63 || !NameSize || (Flags&1) || (Flags&8) || (Method!=0&&Method!=8)
                || Packed==UINT32_MAX || Size==UINT32_MAX || Offset>quint64(File.size())){++Position;continue;}
            const auto Name=File.read(NameSize);if(Name.size()!=NameSize)break;
            const auto Text=(Flags&0x800)?QString::fromUtf8(Name):QString::fromLatin1(Name);
            if(!Text.contains(QChar::Null)&&!Text.endsWith('/'))Entries.append({Text,Offset,Packed,Size,U32(14),Method});
            if(Entries.size()>=100000)break;
            if(Packed>quint64(File.size())-Offset)break;
            Position=qint64(Offset+Packed);
        }
        if(Entries.isEmpty())return Error("image-recovery-no-local-entries");
        return Entries;
    }
    inline storage::Result<Content> Read(const QString& Path,const Entry& Item,storage::OperationContext Context)
    {
        constexpr quint64 RecoveryLimit=256ULL*1024*1024;
        if(Item.size_>RecoveryLimit || Item.compressed_>RecoveryLimit)return Error("image-recovery-preview-budget");
        QFile File(Path);if(!File.open(QIODevice::ReadOnly)||!File.seek(qint64(Item.offset_)))return Error("image-recovery-open");
        Content Out;Out.bytes_.reserve(qsizetype(Item.size_));
        z_stream Stream{};const bool Deflate=Item.method_==8;
        if(Deflate && inflateInit2(&Stream,-MAX_WBITS)!=Z_OK)return Error("image-recovery-inflate");
        struct Cleanup {z_stream* stream_;~Cleanup(){if(stream_)inflateEnd(stream_);}} Cleanup{Deflate?&Stream:nullptr};
        quint64 Remaining=Item.compressed_;int Status=Z_OK;
        while(Remaining && Status==Z_OK){
            if(auto Stop=Context.Check({"image-recovery"}))return *Stop;
            const auto Chunk=File.read(qint64(qMin(Remaining,quint64(64*1024))));if(Chunk.isEmpty())break;
            Remaining-=quint64(Chunk.size());
            if(!Deflate){Out.bytes_+=Chunk;continue;}
            Stream.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(Chunk.constData()));Stream.avail_in=uInt(Chunk.size());
            do{
                char Buffer[64*1024];Stream.next_out=reinterpret_cast<Bytef*>(Buffer);Stream.avail_out=sizeof(Buffer);
                Status=inflate(&Stream,Z_NO_FLUSH);const auto Count=sizeof(Buffer)-Stream.avail_out;
                if(quint64(Out.bytes_.size())+Count>RecoveryLimit)return Error("image-recovery-preview-budget");
                Out.bytes_.append(Buffer,qsizetype(Count));
                if(Status!=Z_OK)break;
            }while(Stream.avail_in || !Stream.avail_out);
        }
        const auto Crc=crc32(0,reinterpret_cast<const Bytef*>(Out.bytes_.constData()),uInt(Out.bytes_.size()));
        if(Remaining || (Deflate&&Status!=Z_STREAM_END) || quint64(Out.bytes_.size())!=Item.size_ || Crc!=Item.crc_)
            Out.warning_="image-recovery-incomplete-or-crc";
        if(Out.bytes_.isEmpty())return Error("image-recovery-empty");
        return Out;
    }
}
