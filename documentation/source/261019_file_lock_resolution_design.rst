HyperCommander 파일 점유 해제 구현 설계 및 UI 시안
============================================================================================

작성일: 2026-10-19

개정: v1.1

상태: 제품 통합 구현 · 검증 결과와 적용 범위는 15절에 기록

산출물: ``_build/file-lock-design/design.html`` · ``ui-preview.html`` · ``captures/`` · ``prototype/``

1. 도입 범위와 권고안
----------------------------------------------------------------------------------------

HyperCommanderAdmin.exe에 파일 점유 조회·선택 핸들 강제 닫기·선택 프로세스 종료를 통합한다. 새 배포 EXE를 추가하지 않는다. 같은 EXE를 진단 세션별 별도 프로세스로 실행하여 기존 관리자 복사·폴더 조회와 수명을 분리한다. UI와 파일 작업 재개는 HyperCommander가 소유한다.

1~14절은 초기 설계와 시안 제작 시점의 기록이다. 이후 제품에 점유 조회·선택 핸들 닫기·선택 프로세스 종료 및 파일 작업 재시도를 통합하였다. 현재 구현과 초기 설계의 차이, 검증 근거와 미검증 범위는 15절을 우선 적용한다. 기존 HTML·시안 갤러리는 예시 데이터 기반의 초기 설계 자료로 유지한다.

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 구분
     - 범위
   * - 우선 지원
     - Windows x64의 로컬 NTFS·ReFS 파일 및 폴더. 영구 삭제·휴지통 삭제·단일 이름 변경·이동 원본 삭제. 실제 파일 시스템별 검증 후 지원을 확정한다.
   * - 후속 지원
     - 일괄 이름 변경의 journal 기반 재개, 복사·덮어쓰기 실패, HyperViewer·HyperEditor 등 자체 리소스의 정상 해제 연동.
   * - 조회 전용 또는 제외
     - 원격 SMB 점유자, 보호된 프로세스, 커널·드라이버 참조, 일반 메모리 매핑의 강제 해제. 압축·WFX 가상 항목에는 직접 적용하지 않는다.
   * - 자동 실행 정책
     - 자동 강제 닫기·자동 프로세스 종료·모두 강제 처리·재부팅 후 강제 삭제를 제공하지 않는다. 해제 후 파일 작업 재개도 별도 다시 시도로 수행한다.

SMB 경로에서 로컬 클라이언트의 참조를 찾더라도 서버 전체 점유자를 확인한 것으로 표시하지 않는다. 압축 내부 항목은 물리적 원본 압축 파일을 진단할 수 있으나, 압축 작업의 재개는 기존 트랜잭션 규칙을 유지한다.

2. 현재 구조와 변경 지점
----------------------------------------------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 현재 파일
     - 확인 사항과 예정 변경
   * - HyperCommanderAdmin/main.cpp
     - --request / --elevated-job / --elevated-list를 처리한다. 새 --file-lock-session 모드를 분리 추가한다. 기존 모드의 승격 검사와 업데이트 시작 차단을 유지한다.
   * - UniqueLibs/nativeFileElevation.cpp
     - Named Pipe의 nonce·상대 PID·호출자 생성 시각·유효 시간·실행 gate를 참고한다. 파일 작업 요청 version 2에 점유 프로토콜을 혼합하지 않는다.
   * - UniqueLibs/nativeWritableStorage.cpp
     - Win32 32·33을 재시도 가능으로 분류한다. 실패 stage와 실제 실패 경로·상위 폴더·파일 식별 정보를 구조화하여 전달한다. 휴지통 HRESULT도 원본 값과 정규화 코드를 보존한다.
   * - commonLibs/operations/fileOperation.cpp
     - 이동 원본 삭제에만 제한적 공유 위반 재시도가 있다. 삭제·이름 변경의 실패 단계 보존과 명시적 재개를 추가한다.
   * - commonLibs/operations/jobScheduler.hpp
     - WaitingForUser와 interaction 기반으로 UI 응답을 기다린다. 점유 조회 동안 파일 작업 worker를 반환하고 작업 context를 유지한다.
   * - UniqueLibs/fileJobUi.cpp
     - 현재 실패 항목 재시도는 일부 로컬 파일 복사·이동에 한정된다. 삭제·이름 변경에 별도 continuation을 연결한다.
   * - UniqueLibs/batchRenameJournal.cpp
     - 원본 변경과 파일 열기 실패가 동일 오류로 합쳐지는 경로를 분리한다. 임시 이름·intent·receipt·복구 상태를 그대로 유지한다.
   * - HyperCommander/UIs/dialogShell.*
     - 제품 대화상자 프레임과 헤더 아래 선택 요약 구조를 준용한다. fm::ui의 Header·Footer·Banner·Card·Button을 재사용한다.

신규 후보 파일은 commonLibs/operations/fileLockTypes.hpp, UniqueLibs/fileLockService.{hpp,cpp}, UniqueLibs/nativeFileLockScanner.{hpp,cpp}, UniqueLibs/nativeFileLockSession.{hpp,cpp}, HyperCommander/UIs/fileLockDialog.{hpp,cpp}이다. 모두 구현 시 제안명이며 이번 작업에서 제품 소스로 생성하지 않는다.

3. 프로세스 경계와 책임
----------------------------------------------------------------------------------------

본체는 실패 작업의 jobId·operationEpoch·원본·목적지·완료 영수증을 소유한다. Admin은 허용된 대상의 조회와 사용자가 선택한 해제 요청만 실행한다. helper에 삭제·이름 변경 전체 계획을 다시 전달하지 않는다.

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 계층
     - 책임
   * - FileLockCoordinator · 본체
     - 조회 세션 생성, UI 표시, 동작 승인, 재개 검증. 창 파괴와 job 취소 시 비동기 callback을 폐기한다.
   * - FileLockSession · Admin
     - 대상·권한 범위를 검증하고 조회 결과와 capability를 발급한다. 재조회 세대와 처리 결과를 세션 안에서 관리한다.
   * - RestartManagerBackend
     - 개별 파일을 등록하여 사용 프로세스를 조회한다. 반환 PID만으로 핸들 닫기를 허용하지 않는다. RmShutdown은 선택 항목 해제 용도로 사용하지 않는다.
   * - NativeHandleBackend
     - 시스템 핸들과 모듈을 조사한다. 소스 핸들 값·파일 식별 정보가 확인된 파일 핸들에만 선택 닫기 capability를 발급한다.
   * - FileOperationContinuation
     - 파일 동일성·목적지 상태·사용자 정책을 검증하고 보존된 실패 단계만 다시 수행한다.

일반 권한 조회는 새 read-only 모드로 허용한다. 접근이 부족하면 사용자가 관리자 권한 재조회를 선택한다. 승격 세션에서도 핸들 닫기와 프로세스 종료는 각각 별도 승인 대상이다. UAC 허용은 모든 점유자 종료에 대한 승인이 아니다.

동일 EXE의 인스턴스를 재사용하더라도 다른 관리자 파일 작업과 점유 진단을 같은 프로세스에 배치하지 않는다. 진단용 watchdog은 해당 진단 인스턴스만 대상으로 한다. 점유 해제 commit 이후 연결이 끊기면 결과 미확정으로 처리한다.

4. 조회 전략과 결과 해석
----------------------------------------------------------------------------------------

* 실제 작업 실패 뒤에만 자동 진단을 제안한다. Win32 32·33은 기본 진입 조건이다. 오류 5는 ACL·읽기 전용·매핑 등 원인이 다양하므로 자동으로 잠금 오류로 치환하지 않고 수동 사용 정보 조회를 제공한다.

* 개별 파일은 RmStartSession → RmRegisterResources → RmGetList → RmEndSession으로 먼저 조회한다. 결과는 ProcessReference이며 확인된 핸들로 표시하지 않는다.

* 폴더 또는 핸들 상세 요청은 시스템 핸들 snapshot을 한 번 수집하여 대상 파일·폴더 prefix와 대조한다. 여러 실패 항목마다 전체 scan을 반복하지 않는다. 폴더를 Restart Manager에 직접 등록하지 않는다.

* 실행 이미지 경로에는 QueryFullProcessImageNameW를 사용한다. 모듈 조회는 API 반환 길이를 기준으로 판정하고 stale GetLastError에 의존하지 않는다.

* 경로는 표시용과 비교용을 분리한다. 가능하면 volume serial + FILE_ID_INFO로 동일 파일을 식별한다. 하드 링크·reparse point·권한 때문에 검증하지 못한 항목에는 강제 닫기 capability를 발급하지 않는다.

* GrantedAccess는 접근 권한 정보이며 공유 모드가 아니다. 이것만으로 FILE_SHARE_DELETE 허용 여부나 실제 차단 원인을 확정하지 않는다. 원본 Win32 오류와 참조 정보의 결합도 단독 차단자 판정의 증명이 아니다.

* 조회에서 복제한 핸들은 즉시 정리한다. 진단용 복제본을 UI 선택 시간 전체에 유지하면 자연 해제를 지연시킬 수 있으므로 장기간 보유하지 않는다.

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 참조 종류
     - UI 표시 / 허용 동작
   * - NativeFileHandle
     - 파일 핸들 확인 · 사용 중. 재검증 가능한 대상에 한해 선택 핸들 강제 닫기를 제공한다.
   * - RestartManagerProcess
     - 프로세스 확인 · 핸들 상세 미확인. 핸들 닫기 비활성, 정책상 가능한 프로세스만 종료 확인을 제공한다.
   * - LoadedImage / LoadedModule
     - 실행 중 / 모듈 사용 중. 파일 핸들 닫기를 제공하지 않는다. 메모리 매핑을 원격 강제 해제하지 않는다.
   * - OwnProcess / OwnPluginHost
     - 자체 사용 중. document broker·작업 소유자에 정상 해제를 요청하고 일반 강제 닫기는 차단한다.
   * - AccessDenied / Protected / Partial
     - 확인 범위를 표시한다. 빈 결과라도 사용 중이 아니라고 단정하지 않는다.

5. 데이터 계약과 IPC
----------------------------------------------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 계약
     - 핵심 필드
   * - LockQuery
     - sessionId, queryGeneration, jobId, operationEpoch, targets[], targetIdentities[], recursive, deadline, resultLimit
   * - ProcessReference
     - pid, processCreated, imagePath, userSid, displayUser, referenceKind, visibility, terminationEligibility
   * - HandleReference
     - rowId, referenceKind=NativeFileHandle, sourceHandleHex, processCreated, objectType, fileIdentity, displayPath, grantedAccessHex, observedAt, closeEligibility
   * - ActionCapability
     - 세션 내부 opaque ID. queryGeneration·rowId·파일 범위·동작 종류·만료 시각을 결합한다. 임의 PID·핸들 숫자 입력은 허용하지 않는다.
   * - LockActionRequest
     - requestId, sessionId, queryGeneration, capabilityId, action=CloseHandle 또는 TerminateProcess, explicitApproval
   * - LockActionResult
     - requestId, action, stage, nativeCode, outcome=Completed / Rejected / TargetChanged / AlreadyGone / Unknown, requiresRefresh

PID를 제외한 핸들 값·64비트 파일 ID·FILETIME·sequence는 JSON 문자열 또는 16진수 문자열로 전송한다. JSON number로 pointer-sized 정수를 전달하지 않는다. raw 커널 포인터는 전송·로그·승인 식별자로 사용하지 않는다.

.. code-block:: json

   {"protocol":"hc.fileLocks","version":1,"kind":"closeHandle",
    "sessionId":"<session>","requestId":"<unique request>",
    "queryGeneration":"4","capabilityId":"<opaque>",
    "explicitApproval":true}

동작 메시지에는 클라이언트가 임의로 조립한 PID·핸들·경로를 넣지 않는다. Admin이 자체 조회로 생성한 capability만 해석한다. 무작위 session nonce, 호출 프로세스 PID·생성 시각·사용자 SID, pipe peer PID, 설치 루트의 신뢰 가능한 호출 EXE를 결합해 검증한다. 승격이 다른 관리자 계정으로 실행되는 경우에도 요청한 원래 사용자와 승인 대상을 유지한다.

기존 파일 작업 IPC의 frameLimit은 2 MiB이다. 새 점유 프로토콜은 별도 version과 codec을 사용하고, 제안 초기값으로 frame 64 KiB 이하·페이지 128행 이하·표시 결과 2,000행 이하를 적용한다. overflow·접근 실패·시간 제한은 partial 원인과 함께 반환한다. 파일 경로는 HTML·RichText로 해석하지 않는다.

같은 requestId를 중복 수신하면 기존 receipt를 반환한다. 세션이 종료되어 receipt를 확인할 수 없으면 Unknown으로 반환하며, 강제 동작을 재전송하지 않는다. queryGeneration 갱신·job 취소·operationEpoch 변경 시 기존 capability를 폐기한다.

6. 선택 핸들 강제 닫기
----------------------------------------------------------------------------------------

* 사용자가 파일 핸들 행을 선택하고 강제 닫기 확인 창에서 위험 안내를 확인한다. 기본 버튼과 초기 포커스는 취소로 유지한다. Enter 키만으로 강제 닫기가 실행되지 않게 한다.

* Admin은 capability 범위·세대·유효 시간·호출자·job 유효성을 확인하고, 열린 프로세스 객체의 생성 시각을 검증한다. 다른 PID로 재연결하지 않는다.

* 소스 핸들을 비파괴적으로 복제하여 파일 종류·정규화된 대상·파일 식별 정보를 재검증한다. 필요하면 CompareObjectHandles로 두 복제본의 동일 객체 여부를 보조 확인한다. 이 검사로 원격 원본 핸들 번호를 원자적으로 고정할 수 있다고 주장하지 않는다.

* 실행 gate 승인 후 DuplicateHandle에 DUPLICATE_CLOSE_SOURCE를 지정한다. 복제 대상이 있으면 로컬 복제본도 반드시 정리한다. 반환 실패만으로 원본 핸들이 유지되었다고 판단하지 않는다.

* 닫기 시도 이후는 동일 핸들 숫자로 자동 재시도하지 않는다. 결과·오류·stage를 기록하고 새로운 조회 결과로 갱신한다. UI는 닫기 요청 처리 완료와 원래 파일 작업 성공을 구분한다.

잔여 경합: 최종 검증과 close-source 호출 사이에 상대 프로세스가 핸들을 닫고 같은 값을 재사용할 수 있다. 공개 API만으로 이 간격을 완전히 제거하는 일반적 원자 연산은 제공되지 않는다. 프로세스·핸들 재사용 검사는 위험을 줄이는 수단이며 안전성 보증이 아니다. 임의 프로세스 suspend·코드 주입·커널 드라이버는 범위에 포함하지 않는다.

자체 HyperCommander·해당 Admin·업데이터·현재 작업의 필수 프로세스에는 강제 닫기를 제공하지 않는다. critical/protected 프로세스는 조회 전용으로 처리한다. 프로세스 종료는 별도 확인에서 프로그램 전체에 영향을 준다는 범위를 표시하고, 종료 후 제한 시간 동안 완료를 확인한다.

DUPLICATE_CLOSE_SOURCE는 오류 반환 여부와 무관하게 원본 핸들 닫기가 수행될 수 있다는 Windows 계약이 있다. 따라서 실패를 부작용 없음으로 표현하거나 단순 재시도하는 구현은 허용하지 않는다. [W1]

7. 작업 상태와 재개 계약
----------------------------------------------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 상태
     - 전이와 보존 규칙
   * - Running → LockBlocked
     - 실패한 leaf·부모 경로·nativeCode·실행 stage·원본 snapshot·목적지 receipt를 보존한다. 이미 완료한 항목의 영수증을 변경하지 않는다.
   * - LockBlocked → WaitingForUser
     - worker를 반환한다. 점유 조회는 작업의 mutation lane과 독립된 read-only 진단으로 실행한다. 동일 job의 중복 continuation 실행은 차단한다.
   * - WaitingForUser → Querying → Inspecting
     - partial 결과를 허용한다. 사용자는 직접 닫기 안내·새로고침·승격 재조회·개별 강제 동작·건너뛰기·작업 취소 중 가능한 동작을 선택한다.
   * - Inspecting → ActionPending → RefreshRequired
     - 단일 명시적 동작을 실행한다. 해제 성공 후에도 자동으로 삭제·이름 변경을 실행하지 않는다.
   * - Inspecting → Revalidate → ResumeFailedStep
     - 다시 시도 시 job·epoch·원본 identity와 revision·목적지 상태·사용자 정책을 확인하고 보존된 단계만 재개한다.
   * - ActionPending → Unknown
     - IPC 단절·helper 중단으로 부작용을 확정할 수 없으면 receipt와 새 조회로 확인한다. 같은 강제 명령을 자동 재전송하지 않는다.

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 작업
     - 재개 단위
   * - 영구 삭제
     - 현재 실패 항목만 Remove 재실행. 이미 삭제된 항목은 재열거 계획으로 복원하지 않는다. 새 자식이 생긴 폴더는 무조건 제거하지 않는다.
   * - 휴지통 삭제
     - 기존 recycle-only 계약 유지. 휴지통 실패를 영구 삭제로 대체하지 않는다. 결과 불명확 시 원본·휴지통 영수증을 확인한 뒤 명시적으로 재개한다.
   * - 단일 이름 변경
     - 원본 identity와 revision·원래 이름·목적지 충돌 정책을 검증한다. 프로그램이 저장하면서 파일을 교체했으면 StaleRevision을 표시하고 새 계획의 동의를 받는다.
   * - 이동 원본 삭제
     - destinationCommitted·publishedDestination·sourceRemoved를 보존한다. 검증한 목적지 저장이 끝난 경우 원본 삭제 단계만 재개한다.
   * - 일괄 이름 변경
     - 변경 전 핸들 확보 단계의 실패와 변경 중 실패를 분리한다. 변경 중 실패는 기존 journal로 복구를 확정한 뒤 새 승인 계획을 만든다. 기존 클래스에 UI 대기만 끼워 넣어 임시 이름 상태를 방치하지 않는다.
   * - 폴더·대량 선택
     - 진단에는 실제 실패 경로를 사용한다. original selection 전체의 destructive plan을 새로 제출하지 않는다. 부모 핸들 확보 실패라면 해당 부모도 query target에 포함한다.

닫기 전에 프로그램에서 저장하면 파일 revision이나 identity가 달라질 수 있다. 이 경우 자동으로 기존 snapshot을 최신 값으로 교체하여 삭제하지 않는다. 최신 상태를 표시하고 사용자가 새 작업 범위를 확인하도록 한다.

8. UI 구성과 동작
----------------------------------------------------------------------------------------

권고 화면은 현재 대화상자 구조를 따르는 단일 사용 정보 창이다. 헤더 아래 대상 파일·작업·변경할 이름을 표시하고, 프로그램 목록과 선택 항목 상세를 수직 배치한다. 하단에는 건너뛰기·작업 취소·다시 시도를 배치한다. 강제 동작은 목록의 기본 버튼이나 더블클릭에 연결하지 않는다.

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 화면
     - 내용과 기본 동작
   * - S01 · 조회 중
     - 대상 요약, 불확정 진행 표시, 확인된 결과 수, 조회 취소. 조회 취소는 파일 작업 취소와 구분한다.
   * - S02 · 사용 정보
     - 프로그램·PID·사용자·확인 범위·상태 목록, 선택 핸들 상세, 새로고침·승격 재조회. 기본 동작은 다시 시도이다.
   * - S03 · 핸들 닫기 확인
     - 프로그램·PID·파일·핸들·영향 안내. 위험 확인 체크 이전 실행 비활성. 취소가 기본 버튼이며 Esc는 취소이다.
   * - S04 · 일부 결과
     - 접근 불가·시간 제한·결과 상한 원인을 표시한다. 확인된 행은 유지하고 승격 재조회 또는 추가 조회를 제공한다.
   * - S05 · 해제 후
     - 핸들 처리 완료와 원래 작업 미실행을 구분한다. 다시 시도로 원본 검증 후 작업을 재개한다.
   * - S06 · 발견 없음
     - 현재 권한으로 발견하지 못함을 표시한다. 사용 중이 아니라고 표시하지 않는다. 관리자 재조회와 원래 오류를 안내한다.
   * - S07 · 결과 미확정
     - 강제 명령 재전송을 차단한다. 새로고침을 기본 동작으로 제공한다. 파일 작업 재개는 상태 확인 후 별도 수행한다.

프로세스 행 선택만으로 핸들 하나를 임의 선택하지 않는다. 다중 핸들이 있으면 상세 영역의 핸들 목록에서 단일 행을 선택한다. 사용자에게는 raw handle을 고급 상세로 제공하고 평상시에는 프로그램·파일·조회 범위를 우선 표시한다. 한 번에 모두 강제 닫기는 제공하지 않는다.

프로세스 종료 확인은 핸들 닫기 확인과 분리한다. 종료 대상 프로그램이 다른 파일도 사용 중이면 그 범위를 안내하고 취소를 기본으로 둔다. UI 시안의 프로세스 종료 버튼은 안내만 표시한다. 미확인·보호·자체 프로세스에서는 강제 버튼을 비활성화하고 이유를 보인다.

S03에는 핵심 데이터가 부족한 상태를 넣지 않는다. 조회가 만료되거나 대상이 바뀌면 실행 버튼을 비활성화하고 새로고침으로 복귀한다. 강제 동작에 대한 체크 확인은 세션·행마다 새로 받으며 기억하기 설정을 제공하지 않는다.

9. 현재 테마와 접근성
----------------------------------------------------------------------------------------

2026-10-19 설치본 HyperCommander.json의 general·appearance 항목을 읽어 시안에 적용하였다. 선택 구성표는 기본 다크, dialogBackground는 #20252e, dialogText는 #e8edf5, 대화상자 글꼴은 Malgun Gothic 11pt, 고정폭 설정은 D2Coding ligature 12px이다. 시안의 생성 과정에서 설정 파일을 저장하지 않았다.

실제 waterColorStyle의 fmstyle.lib·fmwidgets.lib를 링크하였다. Standard Qt 컨트롤과 DialogHeader·DialogFooter·Banner·KeyValueCard·Card·Button을 사용한다. Qtitan 전용 그리드는 이 작은 정보 목록에 추가하지 않는다. 기존 ProductStyle의 Qtitan 확장 proxy는 시안에서 사용하지 않으며, 제품 연결 시에는 기존 QApplication 스타일을 그대로 상속한다.

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 요소
     - 적용 규칙
   * - 색상
     - Win·Surface·Foot·Fg·Fg2·Line·Accent·Warn·Danger·Ok 토큰을 참조한다. 시안 색을 제품 코드에 hex로 복사하지 않는다.
   * - 레이아웃
     - 기준 창 1000×780 DIP, 최소 폭 900 DIP 제안. 헤더·푸터 고정, 목록 영역 확장. 작은 작업 영역은 body scroll 또는 상세 접기로 대응한다.
   * - 글꼴·확대
     - 제품의 대화상자 글꼴을 상속한다. 100%·150% DPI, 18pt 확대 글꼴 시안을 별도 렌더링한다. 하드코딩한 pixel 글꼴로 제품 설정을 무시하지 않는다.
   * - 키보드
     - Tab 순서: 조회 동작 → 목록 → 핸들 상세 → 보조 동작 → 하단 동작. Alt+R 새로고침, Alt+A 승격 재조회, Alt+T 재시도. 강제 동작은 확인 창 외 Enter 기본 동작으로 지정하지 않는다.
   * - 텍스트
     - 상태는 아이콘·문구를 함께 제공한다. 경로는 중간 말줄임과 전체 경로 복사, 파일명·프로그램명은 plain text로 처리한다.
   * - 포커스
     - 조회 결과 갱신 시 PID+생성 시각+파일 ID로 선택을 가능한 범위에서 복원한다. 실행 결과 후에도 목록이 이동하며 위험 버튼에 포커스가 새로 지정되지 않게 한다. 창 종료 시 원래 파일 패널로 반환한다.

시안은 Qt offscreen에서 실제 위젯 내용을 캡처하였으며 OS 제목 표시줄은 포함하지 않는다. 한글·라틴 글꼴을 명시적으로 로드하였다. D2Coding을 사용할 수 없는 캡처 환경에서는 Malgun Gothic으로 대체한다. 색상 대비·스크린리더·실제 Windows 키보드 포커스는 제품 연결 이후 별도 검증 대상이다.

10. 취소·제한·오류 처리
----------------------------------------------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 항목
     - 제안 초기 정책
   * - 조회 시간
     - 10초 경과 시 계속 조회 중과 부분 결과 표시, 30초 hard deadline. 실제 파일·핸들 수 측정 후 조정한다.
   * - 메모리·개수
     - 시스템 snapshot 256 MiB 상한, UI 2,000행·페이지 128행. 큰 시스템에서는 상한 도달을 partial로 명시한다. 폴더 tree를 무제한 사전 열거하지 않는다.
   * - worker 격리
     - UI thread에서 NtQueryObject·GetFileType을 호출하지 않는다. 원본의 TerminateThread 회피 코드를 이식하지 않는다. 멈춤 시 read-only 진단 인스턴스만 정리한다.
   * - 강제 동작 이후
     - 결과 미확정에 대해 성공·원상복구를 추정하지 않는다. action stage와 receipt를 보존하고 새 조회를 요구한다.
   * - 조회 취소·창 닫기
     - 조회만 중지하고 job을 사용자 응답 대기로 유지한다. 작업 취소는 별도 동작이다. mutation 실행 이후에는 요청 취소가 부작용 취소를 의미하지 않는다.
   * - 권한 거부
     - 본체는 일반 권한 유지. 승격 필요 범위를 안내하고 UAC 취소는 작업 실패가 아닌 조회·해제 요청 취소로 처리한다.
   * - 로그
     - jobId·sessionId·requestId·동작·PID+생성 시각·대상 파일 ID·nativeCode·outcome만 기록한다. nonce·capability·커널 주소는 기록하지 않는다. 경로 기록은 기존 권한 기록 정책과 보존 기간을 따른다.

재조회에서 프로세스가 사라졌으면 AlreadyGone으로 표시하고 현재 파일 상태를 다시 확인한다. 동일 PID가 다른 생성 시각으로 나타나거나 핸들 대상이 달라졌으면 TargetChanged로 거부한다. 프로그램이 즉시 다시 파일을 열면 새 결과로 표시하며 반복 강제 해제하지 않는다.

11. 구현 순서와 완료 조건
----------------------------------------------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 단계
     - 변경과 완료 조건
   * - P0 · 계약·시험 장치
     - 참조 종류·partial·capability·Unknown 계약 확정. 일회용 점유 fixture로 공유 위반·공유 허용·다중 핸들·프로세스/핸들 재사용을 재현한다.
   * - P1 · 조회·Admin 모드
     - Restart Manager와 수정한 native 조회를 통합한다. 일반 권한/승격, Named Pipe 인증, 시간 제한, stale 결과 폐기를 검증한다.
   * - P2 · UI·단일 작업
     - S01~S07, 삭제·휴지통·단일 이름 변경·이동 원본 삭제 continuation을 연결한다. 진단만으로는 mutation이 일어나지 않는지 확인한다.
   * - P3 · 선택 강제 동작
     - 한 개 핸들 닫기와 별도 프로세스 종료 확인, action gate·receipt·재검증을 구현한다. 오류 반환과 IPC 단절의 Unknown 시험을 통과한다.
   * - P4 · 일괄·자체 리소스
     - 일괄 이름 변경 journal 복구·승인 재계획, document broker의 정상 해제, 물리 압축 파일 진단을 검증한다.
   * - P5 · 제품 검증·배포
     - 지정 Kit Release 빌드, 서명·인벤토리·설치 검증, 사용자 설정·플러그인 보존. 다음 버전에 이력 추가. 현재 설계 작업에서는 제품 배포를 수행하지 않는다.

File Locksmith에서 추출하는 코드는 upstream commit을 고정하고 MIT 저작권·허가 고지를 동봉한다. 원본 WinUI·C++/WinRT 래퍼와 PowerToys 설정·telemetry를 제품 의존성으로 추가하지 않는다. 공통 타입만 commonLibs에 두고 Windows API는 UniqueLibs의 native 구현으로 제한한다.

12. 검증 행렬과 인수 기준
----------------------------------------------------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 검증군
     - 핵심 인수 기준
   * - 잠금 종류
     - 공유 위반·byte-range lock·삭제 공유 허용·다중 프로세스·중복 핸들·폴더 자체/자식 점유. 사용 중이라는 이유만으로 강제 버튼 자동 실행 없음.
   * - 조회 범위
     - RM-only 행은 핸들 닫기 불가. EXE·DLL은 수정한 이미지/모듈 조회로 검증. 보호·접근 불가·일부 결과·결과 없음 문구의 정확성.
   * - 재사용·경합
     - PID 재사용·핸들 재사용·원본 파일 교체·외부 저장·hard link·reparse 변경. 확인된 불일치는 실행 거부. 잔여 TOCTOU를 안전성 보증으로 표현하지 않음.
   * - IPC·권한
     - nonce·peer PID·생성 시각·SID·version·길이·capability 위조·만료·재전송. UAC 취소와 다른 관리자 계정 승격. 확인한 caller EXE 외 요청 거부.
   * - 강제 닫기
     - 점유용 테스트 프로세스만 사용. 프로세스는 유지되며 해당 핸들만 무효화되는지 확인. 다른 핸들의 I/O·파일 상태를 확인. 반환 실패 후 자동 재전송 없음.
   * - 중단·복구
     - 조회 hang·helper 종료·commit 전후 연결 단절·본체 종료·창 닫기·타임아웃. 부작용 미확정의 Unknown 보존 및 명령 재실행 없음.
   * - 파일 작업
     - 휴지통의 영구 삭제 fallback 없음. 이동의 재복사 없음. 이름 변경 목적지 충돌·새 revision·폴더 새 자식 확인. 완료 영수증과 카운트 중복 없음.
   * - 일괄 이름 변경
     - 임시 이름 전후 단계별 중단·복구 실패·receipt 일부 쓰기. 복구 상태 확정 전 새 destructive plan 제출 없음.
   * - UI
     - 현재 다크·라이트·150%·큰 글꼴·긴 경로·100% 작업 영역. Tab·Esc·Enter, 위험 동작 기본 버튼 금지, 상태 전환 중 선택 안정성·스크린리더 이름·파일 패널 포커스 반환.
   * - 범위 제한
     - SMB 서버 점유·커널 참조·매핑·권한 문제에서 만능 해제를 주장하지 않음. 실제 시스템 프로세스와 사용자 앱을 테스트 목적으로 종료하지 않음.

현재 검증 완료: 기존 핵심 C++ 독립 Release 빌드, 일회용 파일 공유 위반/해제 후 삭제·이름 변경, 폴더 점유 조회, 실제 테마 시안의 offscreen 렌더링. 미구현·미검증: 제품 IPC·선택 핸들 강제 닫기·제품 continuation·실제 포커스·스크린리더·잔여 경합 정책. UI 시안의 상태 전환은 예시이며 인수 시험 통과를 의미하지 않는다.

13. 기존 실험 근거와 출처
----------------------------------------------------------------------------------------

실험 자료: _build/research/file-locksmith/probe-results.json 및 module-query-results.json. 원본 commit 1b33221dfa19f30d71409eb550807883fb27283a. 일반 권한의 일회용 점유 프로세스만 사용하였다. 파일 핸들 정상 해제와 점유용 프로세스 종료 뒤 삭제·이름 변경 성공을 확인하였으며, 개별 원격 핸들 강제 닫기는 아직 실험하지 않았다.

마지막 EXE 5회 조회는 원본 native 검색 0/5, Restart Manager 5/5였다. 별도 API 실험에서 성공한 GetModuleFileNameExW 호출 후 오류 122가 남는 경우와 잘못된 local-module fallback 실패를 재현하였다. 전체 검색을 수정한 구현으로 다시 검증한 상태는 아니다. 약 1.7~1.9초의 조회 시간은 해당 PC의 예시 측정이며 제품 성능 보장이 아니다.

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - 근거
     - 출처
   * - W1 · DuplicateHandle
     - https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle
   * - W2 · RmGetList / directory 오류
     - https://learn.microsoft.com/en-us/windows/win32/api/restartmanager/nf-restartmanager-rmgetlist
   * - W3 · Handle 강제 닫기 제약
     - https://learn.microsoft.com/en-us/sysinternals/downloads/handle
   * - W4 · CompareObjectHandles
     - https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-compareobjecthandles
   * - W5 · 원격 모듈 경로 조회
     - https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getmodulefilenameexw
   * - W6 · PowerToys 검색 구현
     - https://github.com/microsoft/PowerToys/blob/1b33221dfa19f30d71409eb550807883fb27283a/src/modules/FileLocksmith/FileLocksmithLibInterop/FileLocksmith.cpp
   * - W7 · PowerToys 라이선스
     - https://github.com/microsoft/PowerToys/blob/1b33221dfa19f30d71409eb550807883fb27283a/LICENSE

14. UI 시안 이미지
----------------------------------------------------------------------------------------

.. figure:: ../../_build/file-lock-design/captures/current-main.png
   :alt: 현재 다크 · 파일 사용 정보
   :width: 100%

   현재 다크 · 파일 사용 정보. 실제 테마 위젯의 예시 데이터 화면이며 제품 기능은 미구현이다.

.. figure:: ../../_build/file-lock-design/captures/current-confirm.png
   :alt: 현재 다크 · 선택 핸들 강제 닫기 확인
   :width: 100%

   현재 다크 · 선택 핸들 강제 닫기 확인. 실제 테마 위젯의 예시 데이터 화면이며 제품 기능은 미구현이다.

.. figure:: ../../_build/file-lock-design/captures/light-main.png
   :alt: 라이트 · 파일 사용 정보
   :width: 100%

   라이트 · 파일 사용 정보. 실제 테마 위젯의 예시 데이터 화면이며 제품 기능은 미구현이다.

15. 제품 구현 기록 · 2026-10-19
----------------------------------------------------------------------------------------

구현 구조
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``nativeFileLocks``는 Restart Manager와 시스템 파일 핸들 스냅숏을 결합하고, 실행 이미지와 모듈 참조를 보완한다. PowerToys 코드를 제품에 복사하지 않고 Windows API 기반의 조회·동작 계층을 별도로 작성하였다. 파일 ID, 프로세스 생성 시각과 실행 경로를 동작 직전에 다시 확인한다. 일반 파일 핸들만 개별 닫기를 제공하며 실행 이미지·모듈 행에는 핸들 닫기를 제공하지 않는다.

``fileLockService``는 기존 ``HyperCommanderAdmin.exe --file-lock-session``을 실행한다. 별도 배포 EXE는 없다. 기본 조회는 일반 권한으로 실행하고 관리자 재조회는 사용자가 선택할 때만 승격한다. named pipe의 PID·경로·사용자 SID·프로세스 생성 시각을 검증하며, 요청에는 서버가 발급한 일회성 capability와 조회 세대를 사용한다. 동작 시도 후 스냅숏을 폐기하고 동일 요청 ID는 저장한 결과만 반환한다. 응답 손실은 완료로 간주하지 않으며 강제 명령을 자동 재전송하지 않는다.

``FileLockDialog``는 제품 ``DialogShell``과 현재 테마 위젯을 사용한다. 작업 진행 창의 **파일 사용 정보**에서 열며 프로그램 아래에 참조를 묶어서 표시한다. 프로세스 행 선택으로 핸들을 임의로 닫지 않는다. 강제 동작은 위험 확인 체크박스와 별도 실행 버튼을 사용하고 취소를 기본 버튼으로 유지한다. 요청 전 기록 저장에 실패하면 강제 동작을 실행하지 않는다.

삭제·단일 이름 변경·이동 원본 정리에서 Win32 오류 32/33이 발생하면 원래 작업을 응답 대기 상태로 유지한다. 명시적인 재시도는 실패 단계부터 진행한다. 이동은 이미 생성한 대상을 확인한 뒤 원본 정리만 재시도한다. 건너뛰기·취소는 원본과 이미 생성된 대상을 보존한다. 일괄 이름 변경은 변경 전 핸들 확보 실패 시 모든 자체 핸들을 해제하여 대기하고, 재시도할 때 폴더와 전체 원본을 다시 검증한다. 변경을 시작한 이후에는 기존 journal 복구 규칙을 유지한다.

지원 경계
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

로컬 물리 경로를 대상으로 한다. UNC·네트워크 드라이브·가상 압축 항목의 서버 점유자 조회와 강제 해제는 제공하지 않는다. 자체 프로세스·시스템·보호 프로세스는 강제 동작에서 제외한다. HyperViewer·HyperEditor의 문서 브로커를 통한 정상 해제, 일반 메모리 매핑 해제와 복사·덮어쓰기 단계의 continuation은 후속 범위이다. 휴지통 API의 공유 위반은 기존 휴지통 삭제 정책을 유지하여 처리하며 영구 삭제로 자동 전환하지 않는다.

조회된 참조만으로 실제 삭제·이름 변경 차단 여부를 확정하지 않는다. 파일 ID 재검증과 원격 핸들 닫기 사이의 핸들 값 재사용 경합은 공개 API로 원자적으로 제거할 수 없다. 강제 닫기 자체가 프로그램의 데이터 손실이나 오동작을 유발할 수 있으므로 자동 실행하지 않는다.

검증 기록
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

지정 Kit의 전체 Release 빌드와 변경 후 제품·도우미·관련 테스트 대상의 Release 빌드를 완료하였다. CTest 관련 7개 묶음은 모두 통과하였다(67.09초). 대상은 ``hc.batch_rename``, ``hc.operation_recovery``, ``hc.file_locks``, ``hc.writes``, ``hc.jobs``, ``hc.file_lock_ui``, ``hc.dialog_foundation``이다. 이후 진단 도우미 자신의 임시 파일 참조를 제외하는 필터를 추가하고 영향받는 조회·동작·UI 검사를 다시 실행하였다.

.. list-table:: 최종 바이너리 검증
   :header-rows: 1
   :widths: 26 74

   * - 항목
     - 결과와 근거
   * - 파일 점유 핵심
     - QtTest 18 passed, 0 failed. 선택 핸들만 닫고 프로세스 생존, 중복 요청 결과 반환, 변조 capability 거부, 핸들 대상 변경 거부, 통신 단절 결과 Unknown, 삭제·이름 변경·이동 재시도, 원본 교체 거부, 건너뛰기·취소의 데이터 보존. ``_build/file-lock-implementation/core-final-tests.txt``.
   * - 실제 관리자 도우미
     - 최종 ``HyperCommanderAdmin.exe``와 격리된 ``HyperCommander.exe`` 이름의 테스트 호출자를 사용한 3개 시나리오 통과(QtTest 5 passed, 초기화·종료 포함). 선택 핸들 닫기·대상 변경 거부·선택 프로세스 종료. 호출자 fixture가 제품 UI 자체를 의미하지는 않는다. ``production-helper-tests.txt``.
   * - 제품 UI 위젯
     - 다크·라이트의 실제 제품 대화상자에서 각 배율 QtTest 4 passed(초기화·종료 포함). 100%·150% 렌더링, 프로세스 행과 핸들 행 구분, 위험 확인 전 실행 비활성화, 확인 후에도 Enter는 취소, 원래 작업 변경 시 동작 비활성화, Esc는 원래 작업 응답을 임의로 생성하지 않음. ``ui-final-tests.txt``, ``ui-150-tests.txt``, ``ui/``, ``ui-150/``.

위 표의 상대 파일명은 ``_build/file-lock-implementation`` 아래에 있다. UI 캡처는 offscreen 렌더링이며 Windows 전경 창의 실제 키보드·스크린리더·포커스 반환 검증을 대신하지 않는다. Qt offscreen의 기본 글꼴 디렉터리 경고가 있어 테스트에서 설치된 Windows 글꼴을 등록하였고, 최종 캡처에서 한글 표시를 확인하였다. 실제 UAC 승격과 다른 관리자 계정의 자격 증명 입력, ReFS·이동식 드라이브별 동작은 미검증이다.

최종 배포 목록은 194개 제품 파일과 서명된 PE 59개를 검증하였다. ``tools/Install-HyperCommander.ps1 -SkipBuild``로 ``D:\Utils\HyperCommander``에 목록 파일을 포함한 195개 파일을 설치하였다. 덮어쓴 파일의 백업·설치 근거는 ``_build/install/fdb829a07e374a699ec7853c1a345292``에 있다. 설치된 본체·관리자 도우미와 최종 빌드의 SHA256 일치, 기존 사용자 설정·이력 파일 4개의 SHA256 보존을 확인하였다. 근거는 ``_build/file-lock-implementation/install-final.log``와 ``deployment-verification.json``이다. 설치 후 전경 창에서 실제 UAC 승인과 파일 작업 전체 흐름을 수동 조작한 것으로 주장하지 않는다.
