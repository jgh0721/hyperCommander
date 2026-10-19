#pragma once
#include "pipeChannel.hpp"
#include "dopusAbi.hpp"
#include <QJsonObject>
#include <QJsonArray>
#include <QTemporaryFile>
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
        bool CanConfigure() const;
        storage::Result<QJsonObject> Handle(WireKind Kind, const QJsonObject& Request, storage::OperationContext Context = {});
        storage::Result<std::shared_ptr<storage::IReadStream>> OpenStream(const QJsonObject& Request, storage::OperationContext Context);
    private:
        struct Record { WIN32_FIND_DATAW file_; QJsonObject columns_; };
        struct Cursor {
            std::shared_ptr<QTemporaryFile> file_;
            std::uint64_t count_ = 0, next_ = 0;
            storage::Result<storage::Unit> Append(const Record& Value);
            storage::Result<Record> Read();
        };
        storage::Error failure(const char* Operation);
        storage::Result<storage::Unit> selectFormat(const QString& Path);
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
        DWORD flags_ = 0;
        QString root_, extensions_, name_, selectedExtension_;
        QJsonArray columns_;
        std::map<int, Cursor> cursors_;
        int nextCursor_ = 1;
        storage::OperationContext context_;
    };
}
