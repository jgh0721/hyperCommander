#pragma once
/// 고유 라이브러리
#include "registryLinks.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QElapsedTimer>

namespace hc::registry {
// Worker-owned snapshots contain only names, timestamps and extracted links.
// No raw registry value payload is persisted in the disposable index cache.
class RegistryLinkCache final
{
public:
    struct Node {
        RegistryLocation location;
        quint64 written = 0;
        QStringList children;
        QList<RegistryLink> links;
        quint32 error = 0;
        bool link = false;
    };
    explicit RegistryLinkCache(View Bits);
    bool Load(const QString& File, const QString& Sid);
    bool Save(const QString& File, const QString& Sid) const;
    void Validate();
    bool Advance(IRegistryBackend& Backend, const std::atomic_bool* Cancel, qsizetype Chunk = 100000,
        int Milliseconds = 750, std::atomic<qsizetype>* Progress = nullptr);
    RegistryLinkIndex Snapshot() const;
    bool FastComplete() const { return fastDone_; }
    bool Complete() const { return fullDone_; }
    qsizetype Keys() const { return nodes_.size(); }
    qsizetype Reads() const { return reads_; }
    qint64 FastMilliseconds() const { return fastMs_; }
    qint64 FullMilliseconds() const { return fullMs_; }
private:
    Node read(IRegistryBackend& Backend, const RegistryLocation& Location, const std::atomic_bool* Cancel);
    bool fastChild(const RegistryLocation& Parent, const QString& Child) const;
    View bits_;
    QHash<QString, Node> nodes_;
    QList<RegistryLocation> fastPending_, fullPending_;
    QSet<QString> fastSeen_, fullSeen_;
    bool fastDone_ = false, fullDone_ = false;
    qsizetype reads_ = 0;
    qint64 fastMs_ = 0, fullMs_ = 0;
};
} // namespace hc::registry