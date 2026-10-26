#pragma once

/// 외부 라이브러리

/// 플랫폼/프레임워크 헤더파일들( Windows, Boost, Qt, ... )
#include <fmstyle/ColorScheme.h>

#include <algorithm>
#include <utility>

namespace hc::theme
{
    /// 제품 화면 디자인 — HyperCommander · HyperEditor · HyperViewer가 같은 디자인을 쓴다.
    inline constexpr fm::style::Design ProductDesign = fm::style::Design::PaperFluent;

    /// 워터컬러를 쓰던 때 저장한 기준 색 · 직접 지정은 그 디자인 칸에 남아 있다. 제품 디자인 칸이
    /// 모두 비어 있으면 워터컬러 칸을 제품 디자인 칸으로 옮긴다 — 복사가 아니라 옮겨야, 변형 초기화로
    /// 제품 칸을 비운 뒤 다시 읽을 때 지운 색이 워터컬러 칸에서 되살아나지 않는다.
    inline fm::style::ColorScheme AdoptLegacyScheme(fm::style::ColorScheme Scheme)
    {
        using fm::style::Design;
        if constexpr (ProductDesign != Design::Watercolor) {
            bool Empty = true;
            for (std::size_t V = 0; V < fm::style::kVariantCount; ++V) {
                const auto Variant = static_cast<fm::style::Variant>(V);
                const auto& Overrides = Scheme.overridesFor(ProductDesign, Variant);
                Empty = Empty && Scheme.seeds.variantSeeds(ProductDesign, Variant).isEmpty()
                    && std::none_of(Overrides.cbegin(), Overrides.cend(), [](const auto& C) { return C.has_value(); });
            }
            if (Empty) {
                for (std::size_t V = 0; V < fm::style::kVariantCount; ++V) {
                    const auto Variant = static_cast<fm::style::Variant>(V);
                    Scheme.seeds.variantSeeds(ProductDesign, Variant) = std::exchange(Scheme.seeds.variantSeeds(Design::Watercolor, Variant), {});
                    Scheme.overridesFor(ProductDesign, Variant) = std::exchange(Scheme.overridesFor(Design::Watercolor, Variant), {});
                }
            }
        }
        return Scheme;
    }

    /// 설정의 색 구성표 JSON(appearance.watercolorTheme)을 제품 디자인용으로 읽는다.
    inline fm::style::ColorScheme ReadProductScheme(const QJsonObject& Object)
    {
        return AdoptLegacyScheme(fm::style::ColorScheme::fromJson(Object).value_or(fm::style::ColorScheme{}));
    }
}
