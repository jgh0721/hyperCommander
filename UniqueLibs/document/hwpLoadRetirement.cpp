#include "hwpLoadRetirement.hpp"
namespace hc::document
{
    bool ConnectHwpRetirement(HwpLoadBridge& Bridge, HwpProcessBackend& Backend,
        quint64 Binding, DocumentOperationTicket Ticket)
    {
        const auto Native = Backend.NativeBinding();
        if(Bridge.CurrentState() != HwpLoadBridge::State::Idle || !Backend.OwnsLoad(Binding, Ticket, Native)
            || Bridge.property("hcRetirementConnected").toBool()) return false;
        Bridge.setProperty("hcRetirementConnected", true);
        struct RetirementState { QString error_; DocumentOperationTicket ticket_; };
        const auto State = std::make_shared<RetirementState>();
        State->ticket_ = std::move(Ticket);
        QPointer<HwpProcessBackend> Owner(&Backend);
        QObject::connect(&Bridge, &HwpLoadBridge::Failed, &Bridge,
            [State](const QString& Code) { State->error_ = Code; });
        QObject::connect(&Bridge, &HwpLoadBridge::ViewFailed, &Bridge,
            [State](const QString&) { State->error_="rhwp-renderer-load"; });
        QObject::connect(&Bridge, &HwpLoadBridge::RetirementRequested, &Bridge,
            [Owner, State, Binding, Native] {
                if(Owner) Owner->RequestRetirement(Binding, State->ticket_, Native, State->error_);
            });
        QObject::connect(&Backend, &HwpSdiBackend::Stopped, &Bridge,
            [&Bridge, State, Binding](quint64 FinishedBinding) {
                // Stopped follows proven Job + worker retirement and proxy teardown.
                if(FinishedBinding == Binding) { State->ticket_ = {}; Bridge.Retire(); }
            });
        QObject::connect(&Backend, &QObject::destroyed, &Bridge, [State, &Bridge] {
            State->ticket_ = {}; Bridge.FailLoad(); // Registry still owns the lease; no Retire ACK.
        });
        return true;
    }
}
