#include "hwpInputInspection.hpp"
#include <QSet>
#include <QXmlStreamReader>
#include <QtEndian>
// Fixed Qt kit exports its namespaced zlib API; never grow a buffer from ZIP data.
#include <zlib.h>
#include <array>
#include <algorithm>
#include <vector>
#include <memory>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>

namespace hc::document {
    namespace {
        quint16 U16(const QByteArray& B, qsizetype At) { return qFromLittleEndian<quint16>(B.constData() + At); }
        quint32 U32(const QByteArray& B, qsizetype At) { return qFromLittleEndian<quint32>(B.constData() + At); }
        quint32 Crc(const QByteArray& Bytes) {
            static const auto Table = [] { std::array<quint32,256> Result{};
                for(quint32 I=0; I<256; ++I) { auto V=I; for(int Bit=0; Bit<8; ++Bit) V=(V>>1)^((V&1)?0xedb88320U:0); Result[I]=V; } return Result; }();
            quint32 Value=0xffffffffU;
            for(const auto Byte:Bytes) Value=Table[(Value^static_cast<unsigned char>(Byte))&255]^(Value>>8);
            return Value^0xffffffffU;
        }
        template<class T> using Com = std::unique_ptr<T, void(*)(T*)>;
        HwpInputInspection Cfb(const QByteArray& Bytes) {
            HwpInputInspection Result{false,false,"hwp","rhwp-invalid-cfb"};
            const auto Memory=GlobalAlloc(GMEM_MOVEABLE,static_cast<SIZE_T>(Bytes.size())); if(!Memory) return Result;
            auto* Pointer=GlobalLock(Memory); if(!Pointer){GlobalFree(Memory);return Result;}
            std::memcpy(Pointer,Bytes.constData(),static_cast<size_t>(Bytes.size())); GlobalUnlock(Memory);
            ILockBytes* Raw=nullptr;
            if(FAILED(CreateILockBytesOnHGlobal(Memory,TRUE,&Raw))){GlobalFree(Memory);return Result;}
            Com<ILockBytes> Lock(Raw,[](ILockBytes* Value){Value->Release();}); IStorage* Store=nullptr;
            if(FAILED(StgOpenStorageOnILockBytes(Lock.get(),nullptr,STGM_READ|STGM_SHARE_EXCLUSIVE,nullptr,0,&Store))) return Result;
            Com<IStorage> Storage(Store,[](IStorage* Value){Value->Release();}); IStream* Stream=nullptr;
            if(FAILED(Storage->OpenStream(L"FileHeader",nullptr,STGM_READ|STGM_SHARE_EXCLUSIVE,0,&Stream))) return Result;
            Com<IStream> Header(Stream,[](IStream* Value){Value->Release();});
            // HWP 5 FileHeader is exactly 256 bytes, including its reserved tail.
            // Reject a truncated or extended layout before interpreting flags.
            constexpr ULONG HeaderLength = 256;
            STATSTG Information{}; char Data[HeaderLength]{}; ULONG Count = 0;
            if(FAILED(Header->Stat(&Information,STATFLAG_NONAME)) || Information.cbSize.QuadPart!=HeaderLength
                || FAILED(Header->Read(Data,HeaderLength,&Count)) || Count!=HeaderLength
                || std::memcmp(Data,"HWP Document File",17)!=0) return Result;
            const auto Version = qFromLittleEndian<quint32>(Data+32);
            if((Version>>24)!=5){Result.error_="rhwp-unsupported-hwp-version";return Result;}
            const auto Flags=qFromLittleEndian<quint32>(Data+36);
            if(Flags&(0x02|0x100)){Result.error_="rhwp-password-required";return Result;}
            // DRM and certificate DRM are protected even without the password bit.
            Result.restricted_=(Flags&(0x04|0x10|0x80|0x400))!=0; Result.accepted_=true; Result.error_.clear(); return Result;
        }
    }
    HwpInputInspection InspectHwpInput(const QByteArray& Bytes,const QString& Format) {
        HwpInputInspection Result{false,false,Format,"rhwp-input-container"};
        if(Bytes.isEmpty()){Result.error_="rhwp-input-budget";return Result;}
        if(Format=="hwp") {
            if(!Bytes.startsWith(QByteArray::fromHex("d0cf11e0a1b11ae1")))return Result;
            return Cfb(Bytes);
        }
        if(Format!="hwpx" || !Bytes.startsWith(QByteArray::fromHex("504b0304")))return Result;
        Result.error_="rhwp-invalid-zip";
        qsizetype End=-1;
        for(qsizetype At=Bytes.size()-22; At>=qMax<qsizetype>(0,Bytes.size()-65557); --At) {
            if(U32(Bytes,At)==0x06054b50 && At+22+U16(Bytes,At+20)==Bytes.size()){End=At;break;}
        }
        if(End<0 || U16(Bytes,End+4) || U16(Bytes,End+6) || U16(Bytes,End+8)!=U16(Bytes,End+10))return Result;
        const auto Count=U16(Bytes,End+10); const quint64 Length=U32(Bytes,End+12), Offset=U32(Bytes,End+16);
        if(!Count || Offset+Length!=static_cast<quint64>(End))return Result;
        struct Entry { QString name_; quint64 offset_, size_, compressed_; quint16 method_; quint32 crc_; bool directory_; };
        std::vector<Entry> Entries;
        std::vector<std::pair<quint64,quint64>> Ranges;
        QSet<QString> Names; QSet<quint32> LocalOffsets; quint64 Expanded=0; qsizetype At=static_cast<qsizetype>(Offset);
        for(int Index=0;Index<Count;++Index) {
            if(At+46>End || U32(Bytes,At)!=0x02014b50)return Result;
            const auto Flags=U16(Bytes,At+8), Method=U16(Bytes,At+10), NameLength=U16(Bytes,At+28);
            const quint64 Compressed=U32(Bytes,At+20), Size=U32(Bytes,At+24); const auto Local=U32(Bytes,At+42);
            const quint64 Next=static_cast<quint64>(At)+46+NameLength+U16(Bytes,At+30)+U16(Bytes,At+32);
            if(Next>static_cast<quint64>(End) || !NameLength || U16(Bytes,At+34) || (Method!=0 && Method!=8))return Result;
            if(Flags&(1|64)){Result.error_="rhwp-password-required";return Result;}
            if(Method==0 && Size!=Compressed)return Result;
            const auto RawName=Bytes.mid(At+46,NameLength); const auto Name=QString::fromUtf8(RawName);
            const auto Parts=Name.split('/');
            if(Name.toUtf8()!=RawName || Name.contains(QChar(0)) || Name.contains('\\') || Name.contains(':')
                || Name.startsWith('/') || Parts.contains("..") || Parts.contains(".") || Names.contains(Name.toCaseFolded())
                || LocalOffsets.contains(Local))return Result;
            if(static_cast<quint64>(Local)+30>Offset || U32(Bytes,Local)!=0x04034b50 || U16(Bytes,Local+6)!=Flags
                || U16(Bytes,Local+8)!=Method || U16(Bytes,Local+26)!=NameLength)return Result;
            const quint64 DataOffset=static_cast<quint64>(Local)+30+NameLength+U16(Bytes,Local+28);
            if(DataOffset+Compressed>Offset || Bytes.mid(Local+30,NameLength)!=RawName)return Result;
            if(!(Flags&8) && (U32(Bytes,Local+14)!=U32(Bytes,At+16) || U32(Bytes,Local+18)!=Compressed || U32(Bytes,Local+22)!=Size))return Result;
            quint64 RangeEnd=DataOffset+Compressed;
            if(Flags&8) {
                if(RangeEnd+12>Offset)return Result;
                if(U32(Bytes,static_cast<qsizetype>(RangeEnd))==0x08074b50)RangeEnd+=4;
                if(RangeEnd+12>Offset || U32(Bytes,static_cast<qsizetype>(RangeEnd))!=U32(Bytes,At+16)
                    || U32(Bytes,static_cast<qsizetype>(RangeEnd+4))!=Compressed
                    || U32(Bytes,static_cast<qsizetype>(RangeEnd+8))!=Size)return Result;
                RangeEnd+=12;
            }
            const auto Attributes=U32(Bytes,At+38);const auto Kind=(Attributes>>16)&0170000;
            const bool Directory=(Attributes&0x10) || Kind==0040000 || Name.endsWith('/');
            if((Kind && Kind!=0100000 && Kind!=0040000) || (Directory && (!Name.endsWith('/') || Size)))return Result;
            Entries.push_back({Name,DataOffset,Size,Compressed,Method,U32(Bytes,At+16),Directory});
            Ranges.emplace_back(Local,RangeEnd);
            Names.insert(Name.toCaseFolded()); LocalOffsets.insert(Local);
            if(Name.contains("signature",Qt::CaseInsensitive))Result.restricted_=true;
            At=static_cast<qsizetype>(Next);
        }
        if(At!=End || !Names.contains("contents/content.hpf") || !Names.contains("contents/header.xml"))return Result;
        std::sort(Ranges.begin(),Ranges.end());
        for(size_t Index=1;Index<Ranges.size();++Index)if(Ranges[Index-1].second>Ranges[Index].first)return Result;
        for(const auto& Entry:Entries) {
            QByteArray Data;
            if(Entry.method_==0)Data=Bytes.mid(static_cast<qsizetype>(Entry.offset_),static_cast<qsizetype>(Entry.size_));
            else {
                // One bounded output allocation, including one overflow-detection
                // byte. Z_FINISH never retries with a larger output buffer.
                Data.resize(static_cast<qsizetype>(Entry.size_+1));z_stream Stream{};
                Stream.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(Bytes.constData()+Entry.offset_));
                Stream.avail_in=static_cast<uInt>(Entry.compressed_);
                Stream.next_out=reinterpret_cast<Bytef*>(Data.data());Stream.avail_out=static_cast<uInt>(Data.size());
                if(inflateInit2(&Stream,-MAX_WBITS)!=Z_OK)return Result;
                const auto Code=inflate(&Stream,Z_FINISH);
                const bool Valid=Code==Z_STREAM_END && Stream.total_out==Entry.size_ && Stream.avail_in==0;
                inflateEnd(&Stream);if(!Valid){Result.error_="rhwp-zip-output-budget-or-stream";return Result;}
                Data.resize(static_cast<qsizetype>(Entry.size_));
            }
            if(Crc(Data)!=Entry.crc_)return Result;
            if(Entry.name_=="META-INF/manifest.xml") {
                QXmlStreamReader Xml(Data);
                while(!Xml.atEnd()) { Xml.readNext();
                    if(Xml.tokenType()==QXmlStreamReader::DTD || Xml.name()==u"encryption-data") {
                        Result.error_="rhwp-unsafe-manifest";return Result;
                    }
                }
                if(Xml.hasError())return Result;
            }
        }
        Result.accepted_=true;Result.error_.clear();return Result;
    }
}
