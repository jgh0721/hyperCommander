#pragma once
#include "pipeChannel.hpp"
#include <QJsonObject>
#include <map>

namespace hc::plugins
{
    // The SDK calls and native enumeration handles stay on the host thread.
    class TcDataHostService final
    {
    public:
        TcDataHostService(HMODULE Module, PluginManifest Manifest);
        ~TcDataHostService();
        storage::Result<storage::Unit> Initialize(const QString& Ini);
        storage::Result<QJsonObject> Probe();
        storage::Result<QJsonObject> Handle(WireKind Kind, const QJsonObject& Request);
    private:
        struct Cursor { HANDLE handle_; WIN32_FIND_DATAW next_; bool end_ = false; };
        storage::Result<QJsonObject> enumerate(const QJsonObject& Request);
        storage::Result<QJsonObject> metadata(const QJsonObject& Request);
        HMODULE module_;
        PluginManifest manifest_;
        std::map<int, Cursor> cursors_;
        int nextCursor_ = 1;
    };
}
