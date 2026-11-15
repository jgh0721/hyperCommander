#pragma once
#include <QtGlobal>

namespace hc::system {
// Worker only: each unresponsive top-level window may use the timeout.
// Does not edit registry/process environment or synthesize a successful write.
quint32 NotifyEnvironmentChanged();
}