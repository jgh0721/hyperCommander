#pragma once
#include "commonLibs/system/registryAccess.hpp"
#include <QMap>
#include <mutex>

class FakeRegistryBackend final : public hc::registry::IRegistryBackend
{
    std::recursive_mutex mutex_;
public:
    using Location = hc::registry::RegistryLocation;
    using Value = hc::registry::RegistryValue;
    struct Record { Location location; QList<Value> values; bool link = false; quint64 written = 0; };
    QMap<QString, Record> records_;
    int mutations_ = 0, failAt_ = -1;
    void Seed(const Location& Location) { std::lock_guard Lock(mutex_); records_.insert(Location.Id(), {Location}); }
    quint32 Fail() { std::lock_guard Lock(mutex_); return ++mutations_ == failAt_ ? ERROR_ACCESS_DENIED : 0; }
    hc::registry::RegistryResult<hc::registry::KeySnapshot> Enumerate(const Location& Location, const std::atomic_bool* Cancel = nullptr) override
    {
        std::lock_guard Lock(mutex_);
        if (Cancel && Cancel->load()) return {{}, ERROR_CANCELLED, false};
        const auto Found = records_.constFind(Location.Id());
        if (Found == records_.cend()) return {{}, ERROR_FILE_NOT_FOUND};
        hc::registry::KeySnapshot Snapshot; Snapshot.info.name = Found->location.key.section('\\', -1);
        Snapshot.info.lastWrite = Found->written; Snapshot.info.link = Found->link; Snapshot.values = Found->values;
        for (const auto& Item : records_) if (Item.location.Parent() && Item.location.Parent()->Id() == Location.Id()) Snapshot.keys.append({Item.location.key.section('\\', -1)});
        Snapshot.info.valueCount = quint32(Snapshot.values.size()); Snapshot.info.subkeyCount = quint32(Snapshot.keys.size());
        return {Snapshot};
    }
    hc::registry::RegistryResult<Value> Read(const Location& Location, const QString& Name) override
    {
        std::lock_guard Lock(mutex_);
        const auto Found = records_.constFind(Location.Id()); if (Found == records_.cend()) return {{}, ERROR_FILE_NOT_FOUND};
        for (const auto& Item : Found->values) if (Item.name.compare(Name, Qt::CaseInsensitive) == 0) return {Item};
        return {{}, ERROR_FILE_NOT_FOUND};
    }
    quint32 Create(const Location& Location) override
    {
        std::lock_guard Lock(mutex_);
        if (const auto Error = Fail()) return Error;
        if (!Location.Parent() || !records_.contains(Location.Parent()->Id())) return ERROR_PATH_NOT_FOUND;
        if (records_.contains(Location.Id())) return 0;
        Seed(Location); return 0;
    }
    quint32 CreateLink(const Location& Location, const QByteArray& Target) override
    {
        std::lock_guard Lock(mutex_);
        const auto Error = Create(Location); if (Error) return Error;
        auto& Item = records_[Location.Id()]; Item.link = true; Item.values = {{"SymbolicLinkValue", REG_LINK, Target}}; return 0;
    }
    quint32 Write(const Location& Location, const Value& NewValue) override
    {
        std::lock_guard Lock(mutex_);
        if (const auto Error = Fail()) return Error;
        auto Found = records_.find(Location.Id()); if (Found == records_.end()) return ERROR_FILE_NOT_FOUND;
        Found->values.removeIf([&](const Value& Existing) { return Existing.name.compare(NewValue.name, Qt::CaseInsensitive) == 0; });
        Found->values.append(NewValue); return 0;
    }
    quint32 DeleteValue(const Location& Location, const QString& Name) override
    {
        std::lock_guard Lock(mutex_);
        if (const auto Error = Fail()) return Error;
        auto Found = records_.find(Location.Id()); if (Found == records_.end()) return ERROR_FILE_NOT_FOUND;
        return Found->values.removeIf([&](const Value& Existing) { return Existing.name.compare(Name, Qt::CaseInsensitive) == 0; }) ? 0 : ERROR_FILE_NOT_FOUND;
    }
    quint32 DeleteKey(const Location& Location) override
    {
        std::lock_guard Lock(mutex_);
        if (const auto Error = Fail()) return Error;
        auto Snapshot = Enumerate(Location); if (!Snapshot) return Snapshot.error;
        if (!Snapshot.value.keys.isEmpty()) return ERROR_ACCESS_DENIED;
        records_.remove(Location.Id()); return 0;
    }
    quint32 RenameKey(const Location& Location, const QString& Name) override
    {
        std::lock_guard Lock(mutex_);
        if (const auto Error = Fail()) return Error;
        if (!records_.contains(Location.Id()) || !Location.Parent()) return ERROR_FILE_NOT_FOUND;
        const auto Destination = Location.Parent()->Child(Name);
        if (Destination.Id() != Location.Id() && records_.contains(Destination.Id())) return ERROR_ALREADY_EXISTS;
        const auto Copy = records_;
        for (auto It = Copy.cbegin(); It != Copy.cend(); ++It) {
            if (It.key() != Location.Id() && !It.key().startsWith(Location.Id() + '\\')) continue;
            auto Record = It.value(); Record.location.key = Destination.key + Record.location.key.mid(Location.key.size());
            records_.remove(It.key()); records_.insert(Record.location.Id(), Record);
        }
        return 0;
    }
};
