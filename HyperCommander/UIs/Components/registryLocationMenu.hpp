#pragma once

/// 고유 라이브러리
#include "UniqueLibs/registry/registryFavorites.hpp"
/// 외부 라이브러리
#include <fmwidgets/Menu.h>

class CRegistryView;
// Reads the shared favorites through the view's asynchronous cache. This menu
// never creates or repairs registry data just because a location is absent.
class RegistryLocationMenu final : public fm::ui::Menu
{
public:
    explicit RegistryLocationMenu(CRegistryView* View, QWidget* Parent);
    void RefreshFavorite();
private:
    void rebuild();
    void updateLocations();
    void updateStar();
    void addLocation(const QString& Label, const hc::registry::RegistryLocation& Location);
    CRegistryView* view_;
};
