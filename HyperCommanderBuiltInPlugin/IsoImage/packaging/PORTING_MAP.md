# PyCdlib → C++ 포팅 대조표

기준: `source-lock.json`의 PyCdlib 1.20.0. 변경일: 2026-09-13.

| 원본 모듈·영역 | C++ 파일 | 포팅 내용·변경 근거 |
|---|---|---|
| `utils.py`, `dates.py`, `udf.py`의 Unicode·tag·CRC | `src/codec.cpp` | endian, 날짜, UTF-8/UTF-16, OSTA compressed Unicode, CRC-CCITT를 명시적 바이트 연산으로 구현한다. 가변 정수·slice의 경계 검사를 C++에서 명시한다. |
| `headervd.py`, `dr.py`, `pycdlib.py`의 ISO tree traversal | `src/reader.cpp` | ISO/Joliet descriptor와 directory record, multi-extent를 읽는다. 순환·중복·예산 초과를 거부한다. |
| `udf.py`의 anchor, sequence, partition, FE/EFE, FID, allocation | `src/reader.cpp` | physical partition, short/long/embedded allocation을 읽는다. 가상·metadata·sparable partition, sparse·continuation은 명시적으로 미지원 처리한다. |
| `dr.py`, `headervd.py`, `path_table_record.py`, `pycdlib.py`의 layout/write | `src/writer.cpp` | ISO level 3, Joliet, 경로표, namespace별 정렬과 공유 payload extent를 작성한다. 입력 snapshot·독점 staging·모든 namespace 재검증을 추가한다. |
| `udf.py`의 descriptor `new`/`record` | `src/udfWriter.cpp` | NSR02, descriptor sequence, 세 anchor, FSD, FE, FID, short allocation을 작성한다. 큰 파일의 allocation을 분할한다. |
| facade·오류 전달 | `include/hcIso/isoImage.h`, `src/api.cpp` | 고정 폭 정수·UTF-8·opaque handle의 C ABI로 대체한다. 예외와 allocator 소유권을 DLL 내부에 유지한다. |

## UDF revision 정정

원본 `PyCdlib.new(udf='2.60')`는 `NSRVolumeStructure.new(2)`를 호출한다.
`UDFLogicalVolumeDescriptor.new`와 `UDFFileSetDescriptor.new`의 domain suffix는
`02 01 03`, `UDFLogicalVolumeImplementationUse.new`의 read/write revision은
258(`0x0102`)이다. 원본이 생성한 이미지도 이 값을 기록함을 확인하였다.
따라서 C++ 프로필은 **ISO 9660 + UDF 1.02 bridge**로 표시하며 Joliet 포함 프로필을 별도로 제공한다.
API 인수 이름을 근거로 UDF 2.60 작성 완료를 주장하지 않는다.

## 현재 범위

파일과 빈 디렉터리, 한글 이름, 결정적 primary alias, streaming 작성,
ISO multi-extent와 UDF 분할 allocation을 포함한다. Joliet 이름은 64 UTF-16
단위로 제한한다. 실제 Windows ISO의 긴 파일명에는 Joliet을 생략한 프로필을 사용한다.
이때 ISO terminator 직후에 UDF VRS를 배치하여 독립 reader와 Windows의 인식을 보장한다.
부팅·Rock Ridge·UDF-only·metadata partition·VAT는 후속 범위다.
ISO 자체에는 일반 파일 payload checksum이 없으므로 기존 이미지의 구조 확인과
원본 대비 내용 검증을 구분한다. 작성 시에는 원본 파일의 전체 바이트를 대조한다.

## 검증

`tests/tstIsoEngine.cpp`는 네 프로필, 긴 UDF 이름, 한글·primary alias 충돌, 독점 생성,
재현성, endian 불일치·UDF CRC·잘린 이미지, 취소·입력 변경을 검사한다.
개발용 비교 하네스는 C++ 출력의 PyCdlib 읽기와 PyCdlib 출력의 C++ 읽기를
서로 수행한다. 실제 Windows ISO의 1,171개 항목을 추출·재생성하고 PyCdlib와
Windows UDF 마운트에서 전체 파일의 SHA-256 일치를 확인하였다.
제품 소스의 `BuiltInPlugin/IsoImage/VALIDATION.md`에 상세 결과를 기록한다.
