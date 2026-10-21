#pragma once
#include "hwpSdiWindow.hpp"
#include "documentWebProcess.hpp"
class QTimer;
namespace hc::document
{
    struct HwpNativeBinding
    {
        quint64 owner_ = 0, generation_ = 0;
        bool operator==(const HwpNativeBinding&) const = default;
    };
    // One-shot read-only adapter. Native runtime pins and leased input are required.
    // Loaded success follows helper FirstFrame; Stop retains leases through Job retirement.
    class HwpProcessBackend final : public HwpSdiBackend
    {
    public:
        HwpProcessBackend(WebProcessRuntime Runtime, WebProcessLimits Limits, int LoadTimeoutMs = 30000, bool Editable = false);
        ~HwpProcessBackend() override;
        void Load(quint64 Binding, DocumentOperationTicket Ticket, SourceIdentity Source) override;
        void Cancel(quint64 Binding, DocumentOperationTicket Ticket) override;
        void Stop(quint64 Binding) override;
        bool EditorCommand(const QJsonObject& Request) override;
        bool OwnsLoad(quint64 Binding, const DocumentOperationTicket& Ticket, HwpNativeBinding Native) const;
        bool RequestRetirement(quint64 Binding, const DocumentOperationTicket& Ticket,
            HwpNativeBinding Native, const QString& FailureCode = {});
        HwpNativeBinding NativeBinding() const;
        bool IsCurrentBinding(HwpNativeBinding Binding) const;
        void InvalidateBinding(HwpNativeBinding Binding);
        // GUI-thread shutdown gate. Keep processing events until IsDrained().
        // Normal application Quit events are deferred automatically; exit() is not.
        static void InitializeLifecycle();
        // Release the drained QApplication-owned registry before a plugin unload.
        static void ReleaseLifecycle();
        static void RequestShutdown();
        static bool IsDrained();
    protected:
        bool event(QEvent* Event) override;
    private:
        struct Work;
        struct BindingState;
        class Registry;
        std::shared_ptr<BindingState> native_;
        static void run(const std::shared_ptr<Work>& State, WebProcessRuntime Runtime, WebProcessLimits Limits);
        void collect();
        WebProcessRuntime runtime_;
        WebProcessLimits limits_;
        std::shared_ptr<Work> work_;
        QTimer* timer_;
        quint64 binding_ = 0;
        int loadTimeoutMs_;
        bool editable_ = false;
        quint64 reportedSequence_ = 0;
        bool bound_ = false, loadReported_ = false;
    };
}
