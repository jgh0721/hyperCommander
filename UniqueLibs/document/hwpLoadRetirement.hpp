#pragma once
#include "hwpLoadBridge.hpp"
#include "hwpProcessBackend.hpp"
namespace hc::document
{
    // Native GUI-thread wiring, installed once while Bridge is Idle and the
    // backend has an active binding. No renderer-supplied authority is accepted.
    bool ConnectHwpRetirement(HwpLoadBridge& Bridge, HwpProcessBackend& Backend,
        quint64 Binding, DocumentOperationTicket Ticket);
}
