#include "stdafx.h"
#include "updateMessages.hpp"

#include <algorithm>

namespace hc::update {
std::wstring Explain(std::string_view Error, const Path& Root) {
    const auto Is = [&](std::initializer_list<std::string_view> Codes) {
        return std::any_of(Codes.begin(), Codes.end(), [&](std::string_view Code) { return Error.starts_with(Code); });
    };
    // Details follow ": " in the code. Only the disk space needs to be repeated.
    const auto Detail = [&] {
        const auto Colon = Error.find(": ");
        try { return Colon == Error.npos ? std::wstring() : Wide(Error.substr(Colon + 2)); } catch (const std::exception&) { return std::wstring(); }
    };
    auto Folder = Root; Folder.make_preferred();
    const auto Recover = L"\"" + (Folder / L"HyperCommanderUpdater.exe").wstring() + L"\" --recover \"" + Folder.wstring() + L"\"";
    if (Is({"update-elevated-process"}))
        return L"관리자 권한으로 실행 중이어서 자동 업데이트를 할 수 없습니다. 일반 권한으로 다시 실행한 뒤 업데이트를 확인하십시오. UAC를 끈 PC에서는 Portable ZIP을 직접 설치하십시오.";
    if (Is({"update-requires-local-ntfs-or-refs"}))
        return L"자동 업데이트는 로컬 NTFS·ReFS 드라이브에 설치한 경우만 지원합니다. USB(FAT32·exFAT)나 네트워크 드라이브에서는 Portable ZIP을 직접 설치하십시오.";
    if (Is({"update-path-linked", "update-path-access"}))
        return L"설치 경로에 정션·심볼릭 링크나 OneDrive 같은 클라우드 동기화 항목이 있거나 접근할 수 없는 폴더가 있어 자동 업데이트를 할 수 없습니다. 일반 폴더로 옮기거나 Portable ZIP을 직접 설치하십시오.";
    if (Is({"update-install-root"}))
        return L"설치 폴더의 실제 경로를 확인할 수 없습니다. SUBST 드라이브나 링크된 경로가 아닌 실제 폴더에서 실행하십시오.";
    if (Is({"update-root-not-writable"}))
        return L"설치 폴더에 쓸 수 없습니다. Program Files처럼 보호된 위치라면 쓰기 가능한 폴더로 옮기거나 Portable ZIP을 직접 설치하십시오.";
    if (Is({"update-disk-space"}))
        return L"설치 드라이브의 여유 공간이 부족합니다. 필요한 공간: " + Detail();
    if (Is({"update-download-"}))
        return L"다운로드를 마치지 못했습니다. 네트워크 연결을 확인한 뒤 다시 시도하십시오. 받은 부분부터 이어서 받습니다.";
    if (Is({"update-unowned-path"}))
        return L"새 버전의 파일 구성을 이 버전의 업데이트 기능이 처리할 수 없습니다. Portable ZIP을 직접 설치하십시오.";
    if (Is({"update-signature-", "update-catalog-", "update-file-hash", "update-file-size", "update-zip-hash", "update-inventory-hash", "update-pe-"}))
        return L"파일의 서명이나 해시가 예상과 달라 업데이트하지 않았습니다. 다시 시도해도 같으면 Portable ZIP을 직접 설치하십시오.";
    if (Is({"update-close-product-processes", "update-parent-still-running", "update-work-still-running"}))
        return L"다른 HyperCommander·편집기·플러그인 창이나 파일 작업이 아직 실행 중입니다. 모두 닫은 뒤 다시 시도하십시오.";
    if (Is({"update-move-failed", "update-copy-failed", "update-rollback-delete", "update-product-flush"}))
        return L"제품 파일을 교체하지 못했습니다. 백신이나 다른 프로그램이 파일을 사용 중일 수 있습니다. 잠시 후 다시 시도하십시오.";
    if (Is({"update-health-check-failed", "update-recovery-or-health-check-pending"}))
        return L"이전 업데이트의 설치 확인을 마치지 못했습니다. 잠시 후 다시 확인해도 같으면 설치 파일이 바뀌었거나 백신이 격리한 것일 수 있습니다. "
            L"Portable ZIP을 다시 설치하거나, 제품 창을 모두 닫고 다음 명령으로 이전 버전으로 되돌리십시오.\n" + Recover;
    return {};
}
}
