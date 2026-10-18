# ISO 호환성 검증 기록

검증일: 2026-09-13 · Windows x64 · Qt 6.12.0 · Visual Studio 2026 v145

## 실제 Windows 이미지

사용자가 제공한 Microsoft 다운로드의 `Windows11_InsiderPreview_Client_x64_ko-kr_26340_9482.iso`를 사용하였다. 만료 토큰이 포함된 다운로드 URL과 Windows 파일은 저장소·배포 패키지에 포함하지 않는다.

| 항목 | 결과 |
|---|---|
| 원본 이미지 크기 | 8,602,294,272바이트 |
| 원본 SHA-256 | `9a4dedc42e250680e66682eec40df78e82da959c305f58f2b70264e7b3f4820d` |
| 원본 ISO 9660 | 안내 파일 1개 |
| 원본 UDF | revision 1.02, physical partition, 파일 1,068개·폴더 103개 |
| 원본 Joliet | 없음 |
| 전체 파일 크기 합계 | 8,596,067,841바이트 |
| `sources/install.wim` 크기 | 7,564,422,206바이트 |
| `install.wim` SHA-256 | `65706cdbcab908de501b1f68241a3841e45978f6d411197bca71ed091c2f2c41` |

자동 namespace 선택은 UDF를 우선하여 실제 설치 파일을 열거하였다. C++ 엔진으로 추출한 모든 파일의 경로·크기·SHA-256을 PyCdlib 1.20.0의 독립 판독 결과와 대조하여 1,171개 항목이 일치하였다.

## 재생성 및 독립 판독

Windows 파일 집합에는 Joliet의 64 UTF-16 unit 제한을 넘는 파일명이 포함되어 있다. ISO 9660 + UDF 1.02 프로필을 추가하여 UDF에 원문 이름을 보존하였다. Joliet 생략 시 ISO terminator 직후에 UDF VRS를 배치하도록 수정하였다. 이 배치 오류는 자체 reader 이후의 PyCdlib 비교에서 검출되었다.

| 항목 | 결과 |
|---|---|
| 재생성 프로필 | ISO 9660 + UDF 1.02, `HC_ISO_UDF_NO_JOLIET` / 프로필 3 |
| 볼륨 이름 | `HC_WINDOWS_TEST` |
| 재생성 크기 | 8,601,274,368바이트 |
| 재생성 SHA-256 | `39b5384e4840b1d908b7295acd97f7110d90865aa8c41c76b9d14e938f468cb7` |
| ISO 9660 대조 | 별칭을 포함한 항목 수·종류·크기·SHA-256 multiset 일치 |
| UDF 대조 | 전체 경로·종류·크기·SHA-256 일치 |
| 독립 reader | PyCdlib 1.20.0에서 두 namespace의 전체 파일 판독 통과 |
| Windows 마운트 | 파일 시스템 `UDF`, 파일 1,068개·폴더 103개 및 전체 SHA-256 일치 |
| 원본 보존 | 검증 전후 원본 ISO의 SHA-256 일치 |

Windows 마운트는 읽기 전용으로 수행하고 시험이 만든 연결을 해제하였다. 재생성 결과는 데이터 이미지이며 El Torito catalog와 BIOS·UEFI 부팅 정보를 재생성하지 않았다. VM 부팅 검증 결과로 해석하지 않는다.

## 격리 WCX 및 제품 통합

| 항목 | 결과 |
|---|---|
| WCX 원본 열거·전체 읽기·추출 | 파일 1,068개·총 8,596,067,841바이트, 전체 SHA-256 일치 |
| WCX 신규 생성 | ISO 9660 + UDF 1.02, 8,601,274,368바이트 |
| WCX 기존 이미지 변경 | marker 파일 추가·전체 읽기·삭제 후 최초 크기 복원 |
| 최종 볼륨 이름 | `CCCOMA_X64FRE_KO-KR_DV9` 유지 |
| 최종 WCX 이미지 SHA-256 | `c9beb6eb353eea4e5646380bd6fe2e5b96b1b5ebcb49bacbb9d8e95007e342d4` |
| 변경 후 PyCdlib 비교 | UDF 전체 경로·크기·SHA-256 일치 |
| 변경 후 Windows 마운트 | UDF 전체 경로·크기·SHA-256 일치 |
| UI | 일반·긴 이름 UDF 두 프로필의 생성·확인·전체 읽기·자동 진입, 100%·150% 통과 |
| 소스 재빌드 | 실제 배포 ZIP에서 독립 엔진 재빌드 및 PyCdlib 왕복 대조 통과 |
| native 소스 목록 | CMake와 Visual Studio의 first-party 210개 항목 동등성 통과 |
| native 준비·배포 복구 | Visual Studio 준비 단계 및 누락된 ISO manifest의 `hc_deploy` 복구 통과 |
| 전체 Debug 회귀 | 34/34 통과, 배포 검사 포함, 188.80초 |
| 전체 Release 회귀 | 34/34 통과, 배포 검사 포함, 142.72초 |

Debug WCX 전량 추출 시험은 894개 파일 대조 후 QtTest 기본 300초 제한으로 종료되었다. 제한을 1,800초로 지정한 Release 시험에서 전량 추출을 완료하였다. 대용량 쓰기 시험에서는 공통 호스트가 헬퍼 없는 WCX에 일반 요청 제한을 적용하는 결함을 검출하였다. `ArchiveUpdateApply`에 쓰기 전용 제한을 적용하도록 수정한 뒤 실제 생성·추가·삭제가 통과하였다. 별도 fixture에서는 헬퍼 없이 300ms 일반 요청 한도를 넘는 1초 쓰기의 정상 완료를 회귀 검사한다.

## 재현 명령

PyCdlib 원본은 `packaging/source-lock.json`에 고정한 소스를 별도 개발 디렉터리에 준비한다. 비교 하네스의 `--work`에는 새 디렉터리를 지정한다.

```powershell
python tools/Test-IsoReference.py `
  --core _build/vc2026-qt612/Debug/hc_iso_core.dll `
  --image <Windows-ISO-경로> --work <새-작업-디렉터리> `
  --pycdlib-source <pycdlib-1.20.0-소스-루트> --profile 3

powershell -NoProfile -ExecutionPolicy Bypass -File tools/Test-IsoWindowsMount.ps1 `
  -Image <작업-디렉터리>/recreated-bridge.iso `
  -Inventory <작업-디렉터리>/inventory.json -Report <작업-디렉터리>/windows-mount.json

$env:HC_ISO_REFERENCE_IMAGE = '<Windows-ISO-절대-경로>'
$env:HC_ISO_REFERENCE_WORK = '<작업-디렉터리-절대-경로>'
$env:QTEST_FUNCTION_TIMEOUT = '1800000'
_build/vc2026-qt612/Release/hc_iso_wcx_tests.exe externalBridgeImage externalBridgeWrite
```

일반 CTest의 WCX 외부 이미지 시험은 위 환경 변수가 없는 경우 건너뛴다. 기본 프로필·변경·취소·승인·손상 입력 시험에는 외부 ISO나 Python이 필요하지 않다.

## Total Commander 설치 ZIP 검증

2026-09-13에 Qt 6.12.0 / Visual Studio 2026 v145 x64 kit로 `hc_iso_tc_package`의 Release·Debug 빌드를 완료하였다.

| 검증 | 결과 |
|---|---|
| TC 11.56 x64 실제 설치 | 별도 `wincmd.ini` 및 작업 공간 내부 plugin 경로에서 ZIP 자동 인식·설치·ISO 확장자 등록 통과 |
| TC 실제 읽기 | PyCdlib UDF bridge ISO의 한글 폴더 진입과 파일 열거 확인, 설치된 WCX 및 엔진 DLL 로드 확인 |
| WCX 경로 호환성 | TC가 `/` 경로를 루트 파일로 표시하는 문제 수정. 헤더에 `\`를 반환하며 원시 ABI 회귀 시험 추가 |
| ZIP 구성 | `.wcx`·`.wcx64` 동일 AMD64 바이너리, 엔진 DLL·설치 정보·라이선스·소스 ZIP 포함, 사용자 INI 제외 |
| 런타임 의존성 | Release PE import 검사에서 ISO 엔진과 Windows DLL만 확인. Qt·Python·HyperCommander 실행 파일 불필요 |
| 패키지 바이너리 시험 | Release·Debug ZIP에서 각각 추출한 `.wcx`로 외부 ISO 열거·추출, 한글·빈 파일·빈 폴더 생성 및 추출, 파일 추가·삭제 통과 |
| 독립 판독 | 패키지로 생성한 ISO의 한글 파일을 PyCdlib UDF·Joliet 판독 결과와 비교하여 전체 바이트 일치 확인 |
| 회귀 시험 | `hc.iso_engine`, `hc.iso_wcx`, `hc.iso_job` 각각 Release 3/3, Debug 3/3 통과 |

TC 화면 검증은 자동 설치와 폴더 탐색까지 수행하였다. 전경 창 변경이 반복되어 TC 화면에서의 추출·생성 완료는 검증 범위에 포함하지 않았다. 해당 작업은 설치 ZIP의 바이너리를 직접 호출하는 WCX ABI 시험으로 검증하였다. 대용량 외부 ISO의 회귀 시험은 이번 패키징 변경에서 반복하지 않았다.

패키지 시험 기록은 `_build/iso-tc-validation/abi-release/report.json`과 `abi-debug/report.json`, 재현 스크립트는 같은 상위 디렉터리의 `check-package.py`에 보관하였다. 빌드·CTest 로그는 `_build/iso-reference/tc-final-*-build.log` 및 `tc-*-tests.log`에 보관하였다.

## 로컬 근거

원본 ISO는 `_build/iso-reference/Windows11_InsiderPreview_Client_x64_ko-kr_26340_9482.iso`에 보존한다. 중복 추출본과 실패한 중간 이미지는 제거하였다.

원본 비교와 Windows 마운트 결과는 `_build/iso-reference/windows-final/report.json`, `inventory.json`, `windows-mount.json`에 기록하였다. WCX 결과는 `wcx-independent.json`, `wcx-windows-mount.json` 및 상위 디렉터리의 `windows-wcx-release.log`, `windows-wcx-write.log`에 기록하였다. 기준 PyCdlib 이미지의 세 namespace 대조와 독립 C++ 엔진 재빌드 대조도 각각 `_build/iso-reference/reference-roundtrip`, `standalone-roundtrip`, `package-roundtrip`에서 통과하였다. 실행 시의 전체 로그·최종 추출 파일·생성 이미지는 Git 제외 디렉터리에 보관한다.
