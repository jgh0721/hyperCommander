#pragma once
#include <QStringList>
#include <QSize>
#include <cstdint>
#include <memory>
namespace hc::epub::poc
{
    struct OwnerMode
    {
        bool enabled = false;
        QString pipe;
        QByteArray connection;
        std::uint32_t processId = 0;
        std::uintptr_t parentWindow = 0;
    };
    // Stable Win32 attachment surface, independent of later Qt renderer creation.
    class OwnerSurface final
    {
    public:
        OwnerSurface();
        ~OwnerSurface();
        bool Create(const OwnerMode& Owner);
        std::uintptr_t Window() const;
        QSize Size() const;
        bool Refresh(std::uintptr_t Reader = 0);
    private:
        struct State;
        std::unique_ptr<State> state_;
    };
    OwnerMode ParseOwnerMode();
    bool RefreshOwnerWindow(std::uintptr_t Child, const OwnerMode& Owner);
}
