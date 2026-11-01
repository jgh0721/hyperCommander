#pragma once

// 실행 파일 정보: 버전 리소스와 서명(파일에 포함된 서명 또는 카탈로그 서명).

#include <cstdint>
#include <string>

namespace hc::system {

struct VersionInfo
{
    std::wstring description;
    std::wstring company;
    std::wstring product;
    std::wstring version;
};
bool ReadVersionInfo( const std::wstring& Path, VersionInfo& Out );

enum class SignatureState : uint8_t { Unknown, Signed, Unsigned, Invalid };
struct SignatureInfo
{
    SignatureState state   = SignatureState::Unknown;
    std::wstring   signer;
    bool           catalog = false;
    uint32_t       code    = 0;
};
// 네트워크 조회 없이 캐시된 폐기 정보만 쓴다. 파일을 열 수 없으면 Unknown.
SignatureInfo VerifySignature( const std::wstring& Path );

} // namespace hc::system
