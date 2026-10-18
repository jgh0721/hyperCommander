# 기본 제공 플러그인

HyperCommander와 함께 빌드·배포하는 플러그인 프로젝트의 위치다.

| 프로젝트 | 상태 | 문서 |
|---|---|---|
| IsoImage | C++ 엔진·x64 WCX, ISO 9660·Joliet·UDF 1.02 bridge 생성·읽기 및 제품 연결 | [구현 범위](IsoImage/PLAN.md), [호환성 검증](IsoImage/VALIDATION.md) |

각 프로젝트는 외부 호환 ABI와 내부 서비스 계약을 분리한다. 기본 제공 등록과 바이너리·런타임 배포는 상위 CMake target에서 관리한다. 검증되지 않은 기능은 지원 capability로 노출하지 않는다.
