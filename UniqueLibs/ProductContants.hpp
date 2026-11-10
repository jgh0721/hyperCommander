#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

#if defined(DECLARE_CMNLIBSV2_NAMESPACE)
DECLARE_CMNLIBSV2_NAMESPACE
#endif

namespace hc::product
{
    inline constexpr int jobConcurrencyMin = 1;
    inline constexpr int jobConcurrencyMax = 16;
    inline constexpr int jobConcurrencyDefault = 4;

    static_assert(jobConcurrencyMin <= jobConcurrencyDefault && jobConcurrencyDefault <= jobConcurrencyMax);

    // 평면 보기 결과를 목록에 반영하는 주기. 첫 묶음과 마지막 묶음은 기다리지 않는다.
    inline constexpr int flatViewDisplayIntervalMs = 500;
}
