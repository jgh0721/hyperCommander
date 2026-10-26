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
    constexpr quint64 PreviewLimit=256ULL*1024*1024;
    struct Measured {quint64 packed_=0,size_=0;quint32 crc_=0;bool ended_=false;};
    inline storage::Result<Measured> MeasureDeflate(QFile& File,quint64 Offset,storage::OperationContext Context)
    {
        if(!File.seek(qint64(Offset)))return Error("image-recovery-seek");
        z_stream Stream{};if(inflateInit2(&Stream,-MAX_WBITS)!=Z_OK)return Error("image-recovery-inflate");
        struct Guard {z_stream* stream_;~Guard(){inflateEnd(stream_);}} Guard{&Stream};
        Measured Result;int Status=Z_OK;
        while(Status==Z_OK && Result.packed_<PreviewLimit){
            if(auto Stop=Context.Check({"image-recovery"}))return *Stop;
            const auto Chunk=File.read(qint64(qMin(quint64(64*1024),PreviewLimit-Result.packed_)));if(Chunk.isEmpty())break;
            Stream.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(Chunk.constData()));Stream.avail_in=uInt(Chunk.size());
            do{
                char Buffer[64*1024];Stream.next_out=reinterpret_cast<Bytef*>(Buffer);Stream.avail_out=sizeof(Buffer);
                Status=inflate(&Stream,Z_NO_FLUSH);const auto Count=sizeof(Buffer)-Stream.avail_out;
                Result.size_+=Count;if(Result.size_>PreviewLimit)return Error("image-recovery-preview-budget");
                Result.crc_=quint32(crc32(Result.crc_,reinterpret_cast<const Bytef*>(Buffer),uInt(Count)));
                if(Status!=Z_OK)break;
            }while(Stream.avail_in || !Stream.avail_out);
            Result.packed_=Stream.total_in;
        }
        Result.ended_=Status==Z_STREAM_END;
        if(!Result.size_)return Error("image-recovery-empty");
        return Result;
    }
    inline std::optional<Measured> StoredDescriptor(const QByteArray& Bytes,qsizetype At,bool Wide)
    {
        const int Length=Wide?20:12;if(At<0 || At+Length>Bytes.size())return {};
        const char* P=Bytes.constData()+At;const auto Crc=qFromLittleEndian<quint32>(P);
        const quint64 Packed=Wide?qFromLittleEndian<quint64>(P+4):qFromLittleEndian<quint32>(P+4);
        const quint64 Size=Wide?qFromLittleEndian<quint64>(P+12):qFromLittleEndian<quint32>(P+8);
        const bool Signature=At>=4 && Bytes.mid(At-4,4)==QByteArray("PK\x07\x08",4);
        const auto Content=At-(Signature?4:0);
        if(Packed!=quint64(Content) || Size!=Packed || Packed>PreviewLimit)return {};
        if(crc32(0,reinterpret_cast<const Bytef*>(Bytes.constData()),uInt(Content))!=Crc)return {};
        return Measured{Packed,Size,Crc,true};
    }
    inline storage::Result<Measured> MeasureStored(QFile& File,quint64 Offset,storage::OperationContext Context)
    {
        if(!File.seek(qint64(Offset)))return Error("image-recovery-seek");
        QByteArray Bytes;qsizetype Search=0;
        while(!File.atEnd() && quint64(Bytes.size())<PreviewLimit+32){
            if(auto Stop=Context.Check({"image-recovery"}))return *Stop;
            const auto Chunk=File.read(qMin(qint64(64*1024),qint64(PreviewLimit+32)-Bytes.size()));if(Chunk.isEmpty())break;Bytes+=Chunk;
            while(Search+24<=Bytes.size()){
                const auto At=Bytes.indexOf("PK",Search);if(At<0){Search=qMax(qsizetype(0),Bytes.size()-24);break;}
                if(At+24>Bytes.size()){Search=At;break;}Search=At+1;
                const auto Magic=Bytes.mid(At,4);
                if(Magic==QByteArray("PK\x07\x08",4)){
                    for(bool Wide:{false,true})if(auto Value=StoredDescriptor(Bytes,At+4,Wide))return *Value;
                }else if(Magic==QByteArray("PK\x03\x04",4) || Magic==QByteArray("PK\x01\x02",4)){
                    for(bool Wide:{false,true})if(auto Value=StoredDescriptor(Bytes,At-(Wide?20:12),Wide))return *Value;
                }
            }
        }
        if(File.atEnd())for(bool Wide:{false,true})if(auto Value=StoredDescriptor(Bytes,Bytes.size()-(Wide?20:12),Wide))return *Value;
        return Error("image-recovery-descriptor-unavailable");
    }
    // Read-only recovery: encrypted members remain under the authenticated
    // archive engine. No repaired archive or unverified extracted file is written.
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
            quint64 Packed=U32(18),Size=U32(22);quint32 Crc=U32(14);
            const quint64 Offset=quint64(Position)+30+NameSize+Extra;
            if(U16(4)>63 || !NameSize || (Flags&(1|64)) || (Method!=0&&Method!=8) || Offset>quint64(File.size())){++Position;continue;}
            const auto Name=File.read(NameSize),Fields=File.read(Extra);if(Name.size()!=NameSize || Fields.size()!=Extra)break;
            if(Packed==UINT32_MAX || Size==UINT32_MAX){
                bool Valid=false;
                for(qsizetype At=0;At+4<=Fields.size();){
                    const auto Tag=qFromLittleEndian<quint16>(Fields.constData()+At),Length=qFromLittleEndian<quint16>(Fields.constData()+At+2);
                    At+=4;if(At+Length>Fields.size())break;
                    if(Tag==1){const auto End=At+Length;Valid=true;
                        if(Size==UINT32_MAX){if(At+8>End){Valid=false;break;}Size=qFromLittleEndian<quint64>(Fields.constData()+At);At+=8;}
                        if(Packed==UINT32_MAX){if(At+8>End){Valid=false;break;}Packed=qFromLittleEndian<quint64>(Fields.constData()+At);}
                        break;
                    }At+=Length;
                }
                if(!Valid && !(Flags&8)){++Position;continue;}
            }
            if(Flags&8){
                auto Value=Method==8?MeasureDeflate(File,Offset,Context):MeasureStored(File,Offset,Context);
                if(!Value.HasValue()){if(auto Stop=Context.Check({"image-recovery"}))return *Stop;++Position;continue;}
                Packed=Value.Value().packed_;Size=Value.Value().size_;Crc=Value.Value().crc_;
                // Preserve a declared CRC mismatch as a warning during Read().
                if(Method==8 && Value.Value().ended_ && File.seek(qint64(Offset+Packed))){
                    const auto Descriptor=File.read(24);const int At=Descriptor.startsWith(QByteArray("PK\x07\x08",4))?4:0;
                    if(Descriptor.size()>=At+12)Crc=qFromLittleEndian<quint32>(Descriptor.constData()+At);
                }
            }
            const auto Text=(Flags&0x800)?QString::fromUtf8(Name):QString::fromLatin1(Name);
            if(!Text.contains(QChar::Null)&&!Text.endsWith('/'))Entries.append({Text,Offset,Packed,Size,Crc,Method});
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
