#pragma once
#include "pipeChannel.hpp"
#include "dopusAbi.hpp"
#include <QJsonObject>
#include <map>
#include <vector>

namespace hc::plugins
{
    class DopusVfsHostService final
    {
    public:
        DopusVfsHostService(HMODULE Module, PluginManifest Manifest);
        ~DopusVfsHostService();
        storage::Result<storage::Unit> Initialize();
        storage::Result<QJsonObject> Probe();
        storage::Result<QJsonObject> Handle(WireKind Kind, const QJsonObject& Request);
    private:
        struct Cursor { std::vector<WIN32_FIND_DATAW> entries_; std::size_t next_ = 0; };
        storage::Error failure(const char* Operation);
        storage::Result<std::wstring> path(const QJsonValue& Remote);
        storage::Result<QJsonObject> enumerate(const QJsonObject& Request);
        storage::Result<QJsonObject> transfer(const QJsonObject& Request, bool Write);
        bool batchAvailable(std::wstring Path, UINT Operation, std::wstring Destination = {});
        storage::Result<bool> batch(std::wstring Path, const std::vector<std::wstring>& Files, UINT Operation, std::wstring Destination = {});
        HMODULE module_;
        PluginManifest manifest_;
        HANDLE instance_ = nullptr;
        WinHandle abort_;
        bool initialized_ = false;
        DWORD capabilities_ = 0;
        QString root_, extensions_, name_;
        std::map<int, Cursor> cursors_;
        int nextCursor_ = 1;
        std::size_t retainedEntries_ = 0;
    };
}
