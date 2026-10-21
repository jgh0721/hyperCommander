#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>
#include <cstdint>
#include <memory>
#include <optional>
#include <functional>

namespace hc::document
{
    struct WebProcessRuntime
    {
        // Supplied only by the trusted built-in package admission layer. This
        // launcher checks pinned bytes; it does not grant package approval.
        QString root_;
        QMap<QString, QByteArray> sha256_; // Relative paths, including HyperEditorWeb.exe.
        // Product callers select inventory admission and supply a native policy pin.
        // Empty pin denies admission. Nullopt retains the explicit trusted-map port
        // used by diagnostic fixtures; never populate maps from adjacent files.
        // Inventory admission and a caller-provided map are mutually exclusive.
        std::optional<QByteArray> approvedInventorySha256_;
    };
    struct WebProcessLimits
    {
        // Zero delegates process/memory capacity to Windows.
        std::uint32_t processes_ = 0;
        std::uint64_t memoryBytes_ = 0;
        std::uint32_t startupMs_ = 5000;
    };
    struct WebProcessError { QString code_; std::uint32_t native_ = 0; std::optional<std::uint32_t> childExitCode_; };
    struct WebProcessSnapshot
    {
        std::uint32_t processId_ = 0, activeProcesses_ = 0;
        std::uint64_t peakJobBytes_ = 0;
        // Observed at authenticated Ready, not an authority for future UI commands.
        std::uintptr_t childWindow_ = 0;
        bool running_ = false, authenticated_ = false;
    };
    // Blocking, serialized worker-side owner. Not a generic plugin capability.
    // UI-restricted callers are rejected rather than escaping their existing Job.
    // Destruction waits for proven retirement. Async callers retain failed-start
    // owners via PendingRetirement so this wait never runs on the GUI thread.
    class DocumentWebProcess final
    {
    public:
        // A nonzero parent is an owner-thread native binding retained by the
        // caller until retirement; raw HWND reuse/rebinding is not supported.
        // On failed startup, an async caller supplies an empty PendingRetirement
        // slot to retain any process whose termination is not yet confirmed.
        static std::unique_ptr<DocumentWebProcess> Start(const WebProcessRuntime& Runtime,
            WebProcessLimits Limits, std::uintptr_t ParentWindow, WebProcessError& Error,
            std::unique_ptr<DocumentWebProcess>* PendingRetirement = nullptr,
            const std::function<bool()>& Cancelled = {});
        ~DocumentWebProcess();
        bool Exchange(const QByteArray& Request, QByteArray& Reply, std::uint32_t TimeoutMs, WebProcessError& Error);
        WebProcessSnapshot Snapshot() const;
        // False retains the Job and runtime leases; retry until retirement is proved.
        bool Close(std::uint32_t TimeoutMs = 5000);
        DocumentWebProcess(const DocumentWebProcess&) = delete;
        DocumentWebProcess& operator=(const DocumentWebProcess&) = delete;
    private:
        struct State;
        explicit DocumentWebProcess(std::unique_ptr<State> Value);
        std::unique_ptr<State> state_;
    };
}
