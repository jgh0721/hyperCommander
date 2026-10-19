#pragma once
#include "imagePerformance.hpp"
#include <QBuffer>
#include <QCache>
#include <QCryptographicHash>
#include <QFile>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QtConcurrent>
#include <optional>
#include <memory>
#include <stop_token>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <io.h>
namespace hc::document
{
    struct CachedImageInput { QByteArray bytes_,hash_,signature_;QImage image_; };
    class ImageDecodeCache
    {
        struct State
        {
            QMutex mutex_;
            QCache<QString,CachedImageInput> items_{256*1024};
            QSet<QString> pending_;
        };
        static std::shared_ptr<State> state(){static auto Value=std::make_shared<State>();return Value;}
      public:
        static QByteArray Signature(QFile& File)
        {
            if(File.handle()<0)return {};
            const auto Handle=reinterpret_cast<HANDLE>(_get_osfhandle(File.handle()));
            BY_HANDLE_FILE_INFORMATION Info{};FILE_BASIC_INFO Basic{};
            if(Handle==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(Handle,&Info)||
                !GetFileInformationByHandleEx(Handle,FileBasicInfo,&Basic,sizeof(Basic))||!Basic.ChangeTime.QuadPart||
                (!Info.nFileIndexHigh&&!Info.nFileIndexLow))return {};
            // ChangeTime also detects an overwrite whose size and LastWriteTime
            // were restored. A missing reliable revision disables reuse, not loading.
            return QByteArray::number(Info.dwVolumeSerialNumber)+":"+QByteArray::number(Info.nFileIndexHigh)+":"+
                QByteArray::number(Info.nFileIndexLow)+":"+QByteArray::number(Basic.ChangeTime.QuadPart)+":"+
                QByteArray::number(Basic.LastWriteTime.QuadPart)+":"+QByteArray::number(Info.nFileSizeHigh)+":"+QByteArray::number(Info.nFileSizeLow);
        }
        static std::optional<CachedImageInput> Find(const QString& Path)
        {
            const auto S=state();{QMutexLocker Guard(&S->mutex_);const auto* Item=S->items_.object(Path);if(!Item||Item->signature_.isEmpty())return {};}
            QFile File(Path);if(!File.open(QIODevice::ReadOnly))return {};
            const auto Revision=Signature(File);if(Revision.isEmpty())return {};
            QMutexLocker Guard(&S->mutex_);const auto* Item=S->items_.object(Path);
            if(!Item||Item->signature_.isEmpty())return {};
            if(Item->signature_!=Revision){S->items_.remove(Path);return {};}
            ImageTraceSpan Trace("image.cache-hit",Path);return *Item;
        }
        static void Remember(const QString& Path,CachedImageInput Input)
        {
            if(Input.image_.isNull())return;
            const auto Cost=(Input.image_.sizeInBytes()+Input.bytes_.size()+1023)/1024;
            const auto S=state();QMutexLocker Guard(&S->mutex_);
            if(Cost>S->items_.maxCost())return;
            S->items_.insert(Path,new CachedImageInput(std::move(Input)),int(Cost));
        }
        static void Clear(){const auto S=state();QMutexLocker Guard(&S->mutex_);S->items_.clear();}
        static QImage FindDecoded(const QString& Path,const QByteArray& Digest)
        {
            const auto S=state();QMutexLocker Guard(&S->mutex_);const auto* Item=S->items_.object(Path);
            if(!Item||Item->hash_!=Digest)return {};
            ImageTraceSpan Trace("image.cache-hash-hit",Path);return Item->image_;
        }
        static int CostKiB(){const auto S=state();QMutexLocker Guard(&S->mutex_);return S->items_.totalCost();}
        static void Prefetch(const QStringList& Paths,std::stop_token Stop,QThreadPool* Pool)
        {
            const auto S=state();
            for(const auto& Path:Paths){
                {QMutexLocker Guard(&S->mutex_);if(S->pending_.size()>=2)break;if(S->pending_.contains(Path)||S->items_.contains(Path))continue;S->pending_.insert(Path);}
                Pool->start([Path,S,Stop]{
                    try{
                        if(!Stop.stop_requested()){
                            ImageTraceSpan Trace("image.prefetch",Path);QFile File(Path);
                            if(File.open(QIODevice::ReadOnly)){
                                CachedImageInput Input;Input.signature_=Signature(File);
                                // This is a speculative cache budget; explicit opens
                                // retain their existing unrestricted image path.
                                if(!Input.signature_.isEmpty()&&File.size()<=64*1024*1024){
                                    QCryptographicHash Hash(QCryptographicHash::Sha256);
                                    while(!File.atEnd()&&!Stop.stop_requested()){
                                        const auto Chunk=File.read(256*1024);if(Chunk.isEmpty())break;Hash.addData(Chunk);Input.bytes_+=Chunk;
                                    }
                                    if(!Stop.stop_requested()&&File.atEnd()&&File.error()==QFileDevice::NoError){
                                        const bool Stable=Signature(File)==Input.signature_;File.close();
                                        if(Stable){
                                            Input.hash_=Hash.result();QBuffer Buffer(&Input.bytes_);Buffer.open(QIODevice::ReadOnly);QImageReader Reader(&Buffer);Reader.setAutoTransform(true);
                                            const auto Size=Reader.size();
                                            if(!Size.isEmpty()&&quint64(Size.width())*quint64(Size.height())<=(256ULL*1024*1024)/8){
                                                Input.image_=Reader.read();QFile Verify(Path);
                                                if(!Stop.stop_requested()&&Verify.open(QIODevice::ReadOnly)&&Signature(Verify)==Input.signature_)Remember(Path,std::move(Input));
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }catch(...){ /* Speculative work must never interrupt an explicit open. */ }
                    QMutexLocker Guard(&S->mutex_);S->pending_.remove(Path);
                },-1);
            }
        }
    };
}
