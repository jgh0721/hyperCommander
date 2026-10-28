#pragma once
#include "commonLibs/document/epub/epubPackage.hpp"
#include <QHash>
#include <QThreadPool>
#include <atomic>
#include <memory>

namespace hc::epub::poc {
// Accessed on the scheme-handler thread. Workers only use immutable sessions
// and the atomic byte counter. A reply owns its payload until Chromium retires
// the QIODevice; eviction never invalidates a live response.
class PublicationResources final {
public:
    struct Payload {
        QByteArray bytes_;
        QString error_;
        std::shared_ptr<std::atomic<qint64>> used_;
        qint64 reserved_ = 0;
        ~Payload() { if(used_) used_->fetch_sub(reserved_); }
    };
    using Lease = std::shared_ptr<Payload>;
    explicit PublicationResources(std::shared_ptr<document::epub::PublicationSession> Session) : session_(std::move(Session)) {}
    Lease Cached(const QString& Path) {
        auto At=cache_.find(Path); if(At==cache_.end()) return {};
        At->stamp_=++clock_; return At->data_;
    }
    Lease Reserve(qint64 Bytes) {
        trim(qMax(qint64(0), SoftLimit-Bytes));
        if(Bytes<0 || Bytes>32*1024*1024 || used_->load()>HardLimit-Bytes) return {};
        auto Value=std::make_shared<Payload>();
        Value->used_=used_; Value->reserved_=Bytes; used_->fetch_add(Bytes); return Value;
    }
    void Retain(const QString& Path, const Lease& Value) {
        cache_.insert(Path,{Value,++clock_}); trim(SoftLimit);
    }
    static Lease Load(const std::shared_ptr<document::epub::PublicationSession>& Session, const QString& Path, Lease Value) {
        const auto Result=Session->ReadValidatedResource(Path);
        if(!Result) Value->error_=Result.error_+": "+Result.location_;
        else if(Result.bytes_.size()!=Value->reserved_) Value->error_="epub-resource-length";
        else Value->bytes_=Result.bytes_;
        return Value;
    }
    auto Session() const { return session_; }
    static QThreadPool* Pool() {
        static QThreadPool Pool;
        static const bool Initialized=[] { Pool.setMaxThreadCount(2); return true; }();
        Q_UNUSED(Initialized); return &Pool;
    }
private:
    static constexpr qint64 SoftLimit=48LL*1024*1024, HardLimit=96LL*1024*1024;
    struct Entry { Lease data_; quint64 stamp_=0; };
    void trim(qint64 Target) {
        while(used_->load()>Target && !cache_.isEmpty()) {
            auto Oldest=cache_.end();
            for(auto At=cache_.begin();At!=cache_.end();++At)
                if(At->data_.use_count()==1 && (Oldest==cache_.end() || At->stamp_<Oldest->stamp_)) Oldest=At;
            if(Oldest==cache_.end()) break;
            cache_.erase(Oldest);
        }
    }
    std::shared_ptr<document::epub::PublicationSession> session_;
    std::shared_ptr<std::atomic<qint64>> used_=std::make_shared<std::atomic<qint64>>(0);
    QHash<QString,Entry> cache_;
    quint64 clock_=0;
};
}
