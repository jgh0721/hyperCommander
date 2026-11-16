#pragma once
#include "environmentSnapshot.hpp"
#include <QProcessEnvironment>
#include <functional>
#include <mutex>

namespace hc::environment {
// Native writers are supplied only by product startup. Unit tests use a map.
// Child launches can obtain one immutable generation instead of observing a
// partially applied sequence of Windows environment API calls.
class EnvironmentRuntime final
{
public:
    using Reader = std::function<ValueMap()>;
    using Writer = std::function<quint32(const std::wstring&, const std::optional<std::wstring>&)>;
    EnvironmentRuntime(ValueMap Inherited, ValueMap InitialModel, ValueMap Overlay, Reader Read, Writer Write);
    quint32 Rebuild(const ValueMap& NextModel);
    QProcessEnvironment ChildEnvironment() const;
    ValueMap Snapshot() const;
    quint64 Generation() const;
private:
    mutable std::mutex mutex_;
    ValueMap inherited_, lastModel_, overlay_, published_;
    Reader read_;
    Writer write_;
    quint64 generation_ = 0;
};
quint32 WriteOwnEnvironmentValue(const std::wstring& Name, const std::optional<std::wstring>& Text);
ValueMap EffectiveEnvironment(const MergeResult& Result);
}
