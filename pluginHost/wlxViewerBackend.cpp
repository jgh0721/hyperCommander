#include "wlxViewerBackend.hpp"
#include "externalLibs/TotalCommander/tc_listplug.h"
#include <QFileInfo>
#include <QDir>
#include <array>
#include <algorithm>
#include <cstring>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"wlx"}}; }
        std::optional<QByteArray> AnsiIni(const QString& Path)
        {
            // DefaultIniName has no Unicode SDK variant. Never substitute an
            // unrepresentable character or silently truncate a configuration path.
            const auto Native = QDir::toNativeSeparators(Path).toStdWString();
            const bool Utf8 = GetACP() == CP_UTF8; BOOL Substituted = FALSE;
            const auto Flags = Utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
            const auto Count = WideCharToMultiByte(CP_ACP, Flags, Native.c_str(), -1, nullptr, 0, nullptr, Utf8 ? nullptr : &Substituted);
            if (!Count || Count > MAX_PATH || Substituted) return {};
            QByteArray Bytes(Count, '\0'); Substituted = FALSE;
            if (!WideCharToMultiByte(CP_ACP, Flags, Native.c_str(), -1, Bytes.data(), Count, nullptr, Utf8 ? nullptr : &Substituted) || Substituted) return {};
            Bytes.chop(1); return Bytes;
        }
    }
    WlxViewerBackend::WlxViewerBackend(HMODULE Module) : module_(Module)
    {
        load_ = reinterpret_cast<Load>(GetProcAddress(module_, "ListLoadW"));
        close_ = reinterpret_cast<CloseWindow>(GetProcAddress(module_, "ListCloseWindow"));
        command_ = reinterpret_cast<Command>(GetProcAddress(module_, "ListSendCommand"));
        notification_ = reinterpret_cast<Notification>(GetProcAddress(module_, "ListNotificationReceived"));
    }
    Result<Unit> WlxViewerBackend::Initialize(const QString& DefaultIniPath)
    {
        if (initialized_) return Invalid("host-wlx-already-initialized");
        const auto Defaults = reinterpret_cast<void(__stdcall*)(ListDefaultParamStruct*)>(GetProcAddress(module_, "ListSetDefaultParams"));
        if (Defaults)
        {
            if (DefaultIniPath.isEmpty() || DefaultIniPath.contains(QChar::Null) || !QDir::isAbsolutePath(DefaultIniPath)
                || QFileInfo(DefaultIniPath).fileName().isEmpty()) return Invalid("host-wlx-config-path");
            const auto Parent = QFileInfo(DefaultIniPath).absolutePath();
            if (!QDir().mkpath(Parent)) return HostError("host-wlx-config-directory");
            auto Encoded = AnsiIni(DefaultIniPath);
            if (!Encoded)
            {
                // An existing directory may provide an ANSI-compatible short
                // name. The INI itself need not exist; do not create/overwrite it.
                std::array<wchar_t, 32768> Short{};
                const auto Native = QDir::toNativeSeparators(Parent).toStdWString();
                const auto Size = GetShortPathNameW(Native.c_str(), Short.data(), static_cast<DWORD>(Short.size()));
                if (Size && Size < Short.size()) Encoded = AnsiIni(QDir(QString::fromWCharArray(Short.data())).filePath(QFileInfo(DefaultIniPath).fileName()));
            }
            if (!Encoded) return Error{ErrorCategory::Unsupported, "host-wlx-config-encoding", {"wlx"}};
            ListDefaultParamStruct Parameters{}; Parameters.size = sizeof(Parameters);
            // x64 Unicode interface 2.1 (minor is hundredths, hence 10).
            // Later dark-mode/focus revisions are not
            // advertised until the corresponding host behavior is implemented.
            Parameters.PluginInterfaceVersionHi = 2; Parameters.PluginInterfaceVersionLow = 10;
            std::memcpy(Parameters.DefaultIniName, Encoded->constData(), Encoded->size());
            Defaults(&Parameters);
        }
        initialized_ = true; return Unit{};
    }
    Result<QJsonObject> WlxViewerBackend::Probe()
    {
        if (!initialized_) return Invalid("host-wlx-not-initialized");
        if (!load_) return Error{ErrorCategory::Unsupported, "host-wlx-unicode-exports", {"wlx"}};
        const auto Detect = reinterpret_cast<void(__stdcall*)(char*, int)>(GetProcAddress(module_, "ListGetDetectString"));
        if (!detectString_)
        {
            std::array<char, 2048> Buffer{};
            if (Detect) Detect(Buffer.data(), static_cast<int>(Buffer.size()));
            const auto End = std::find(Buffer.begin(), Buffer.end(), '\0');
            if (End == Buffer.end()) return Invalid("host-wlx-detect-unterminated");
            detectString_ = QByteArray(Buffer.data(), End - Buffer.begin());
        }
        return QJsonObject{{"format", "wlx"}, {"unicode", true}, {"commands", command_ != nullptr}, {"search", bool(GetProcAddress(module_, "ListSearchTextW") || GetProcAddress(module_, "ListSearchText"))}, {"processId", qint64(GetCurrentProcessId())},
            {"detectAvailable", Detect != nullptr}, {"detectStringBytes", QString::fromLatin1(detectString_->toBase64())}, {"ansiCodePage", int(GetACP())}};
    }

    HWND WlxViewerBackend::Open(HWND Parent, const QString& File, int Flags)
    { if (!load_) return nullptr; auto Native = File.toStdWString(); return load_(Parent, Native.data(), Flags); }
    void WlxViewerBackend::Close(HWND Viewer)
    { if (close_) close_(Viewer); else DestroyWindow(Viewer); }
    std::optional<LRESULT> WlxViewerBackend::Notify(HWND Viewer, UINT Message, WPARAM WParam, LPARAM LParam)
    { if (notification_) return notification_(Viewer, Message, WParam, LParam); return {}; }
    Result<QJsonObject> WlxViewerBackend::HandleCommand(HWND Viewer, const QJsonObject& Request)
    {
        const auto Command = Request["command"].toInt(-1), Parameter = Request["parameter"].toInt(-1);
        if (Command == 5)
        {
            const auto Text = Request["text"].toString();
            if (Request.size() != 3 || Request["command"].toDouble() != 5 || !Request["text"].isString() || Text.isEmpty() || Text.size() > 4096
                || Text.contains(QChar::Null) || Parameter < 0 || Parameter > 15 || Request["parameter"].toDouble(-1) != Parameter) return Invalid("host-viewer-search-fields");
            if (const auto Search = reinterpret_cast<int(__stdcall*)(HWND, wchar_t*, int)>(GetProcAddress(module_, "ListSearchTextW")))
            { auto Wide = Text.toStdWString(); return QJsonObject{{"result", Search(Viewer, Wide.data(), Parameter)}}; }
            if (const auto Search = reinterpret_cast<int(__stdcall*)(HWND, char*, int)>(GetProcAddress(module_, "ListSearchText")))
            {
                auto Ansi = Text.toLocal8Bit(); if (QString::fromLocal8Bit(Ansi) != Text) return Error{ErrorCategory::Unsupported, "host-viewer-search-encoding", {"wlx"}};
                return QJsonObject{{"result", Search(Viewer, Ansi.data(), Parameter)}};
            }
            return Error{ErrorCategory::Unsupported, "host-viewer-search-unavailable", {"wlx"}};
        }
        if (Request.size() != 2 || !Request["command"].isDouble() || !Request["parameter"].isDouble()
            || Request["command"].toDouble() != Command || Request["parameter"].toDouble() != Parameter
            || Command < 1 || Command > 4 || Parameter < 0 || Parameter > (Command == 4 ? 100 : 511)) return Invalid("host-viewer-command-fields");
        if (!command_) return Error{ErrorCategory::Unsupported, "host-viewer-command-unsupported", {"wlx"}};
        return QJsonObject{{"result", command_(Viewer, Command, Parameter)}};
    }
}
