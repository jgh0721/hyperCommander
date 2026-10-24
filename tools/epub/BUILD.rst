EPUB 제품 빌드와 검증
======================

선택 엔진은 EPUB.js 0.3.93이며 서명된 HyperEpubWeb 보조 프로세스에서 실행한다.
부모 제품에는 QtWebEngine을 연결하지 않는다. HyperEditor의 편집 기능은 부모의
소스 편집기로 제공하며 리더에는 불변 리소스만 전달한다.

필수 입력
---------

* Qt 6.12.0 / Visual Studio 2026 v145 Hybrid CRT kit와 Python 3.10 이상, Node/npm.
* Git에 고정된 Watercolor 및 QtitanDataGrid 하위 저장소. Qtitan 소스 사용 권한은 별도 전제다.
* 제품 서명용 로컬 도구와 자격 증명. ``tools/CertWithEV.cmd`` 및
  ``tools/SignotaurTool.exe`` 는 Git 제외 대상으로 유지한다.
* 전체 제품 빌드는 독립 승인된 RHWP bundle/runtime과 각각의 SHA-256 pin,
  기존 제품의 이미지 코덱 및 SDK 입력을 명시해야 한다. EPUB pin으로 RHWP를 승인하지 않는다.

재현 절차
---------

변경 없는 체크아웃에서 다음 명령을 실행한다. 출력·서명 캐시는 해당 체크아웃의 ``_build`` 아래에 두고 출력에는 새 경로를 사용한다.
필요한 SDK·RHWP 경로와 pin은 반복 ``--configure-arg=-D이름=값`` 인수로 전달한다.

.. code-block:: powershell

   python tools/epub/build_product_runtime.py `
     --kit D:/Qt/Qt-6.12.0-VC2026-X64-H-MD-2026-10-10 `
     --output _build/epub-release-new `
     --build-product

스크립트는 잠금 파일에 따른 npm ci, 읽기 설정 JavaScript 검사, bundle 생성,
EPUB 승인 없는 helper 빌드, PE 서명, inventory 봉인, 서명 검사, native pin 재구성,
격리 application 배치, 선택적 전체 Release 대상 빌드와 최종 inventory 생성을 순서대로 수행한다.
로그와 ``build-receipt.json`` 에 소스 커밋·입력·helper hash·최종 pin을 기록한다.
``--allow-dirty`` 는 개발 확인용이며 영수증에 변경 상태가 기록된다.
EPUB 변경에 한정하여 기존 이미지 플러그인을 보존해야 하는 경우
``--reuse-approved-image-plugin`` 을 명시한다. 이 옵션은 독립 RHWP pin·서명·hash를
검사한 qhcmodern.dll만 재사용하고 영수증에 기록한다. 이미지 플러그인 변경에는 사용하지 않는다.

이 명령은 설치나 게시를 수행하지 않는다. Release 빌드·Qt 배치·제품 inventory
생성은 동시에 실행하지 않는다. 제품 inventory는 모든 배치와 서명이 끝난 후
다시 생성하고 검사한다. 변경된 runtime을 기존 승인 트리에 덮어쓰지 않는다.

확인 항목
---------

* ``hc_epub_settings_tests``: 전역/책별 설정, 적용·취소, 배경, 한글 조합, 읽기 기록 병합.
* ``hc_epub_resource_tests``: 프로세스 간 자원 한도·취소·비정상 종료 회수.
* ``hc_epub_workspace_tests``: 실제 EPUB2/3 렌더링·검색·책갈피·각주·이미지,
  F3/F4 공유 작업공간과 편집·다른 이름 저장·재열기. 필요한 환경 변수는 테스트 소스에 정의한다.
* ``test_edit_preservation.py <저장 검증 산출물 폴더>``: 수정하지 않은 압축 레코드와 ZIP 메타데이터 보존.
* ``Test-ProductRuntime.ps1``: 고정 profile·pin·경로·해시·서명과 누락/추가 파일 검사.

설치는 ``tools/Install-HyperCommander.ps1 -SkipBuild -SourceDirectory <검사한 스냅샷>``
으로 수행한다. 설치 전 제품 프로세스를 정상 종료하고 설정·플러그인 hash 보존을 확인한다.
Portable ZIP은 검사한 동일 스냅샷으로 만들고 모든 inventory 항목의 크기·hash를 대조한다.
스냅샷에서 New-PortablePackage를 직접 실행할 때는 ``-SymbolsList`` 로 해당 빌드의
``hc-symbols-Release.txt`` 를 명시한다.

지원 범위
---------

DRM 없는 수동형 reflowable EPUB 2/3을 지원하며 XML·ZIP 이름은 UTF-8로 한정한다.
암호화·서명·고정 레이아웃·활성 콘텐츠·XML DTD/엔터티는 현재 거부한다. 사용자 CSS 재정의는 css-tree 3.2.1로 검사하는 표시 전용
부분 집합이다. 외부 URL과 동적 선택자는 허용하지 않는다.

편집은 XHTML·CSS·OPF 원문과 같은 형식의 PNG/JPEG/GIF 이미지 교체를 지원한다.
소스 초안은 실행 취소와 종료 확인을 제공하며 자동 복구는 지원하지 않는다.
EPUB 입력은 64 MiB, 렌더링 리소스는 32 MiB로 제한하며 한 사용자 세션에서
2권을 실행하고 대기를 포함해 8건까지 받는다. 소스 편집은 리소스당 2 MiB,
교체 이미지는 8 MiB·한 변 4096 px·총 4,194,304 px 이내로 제한한다.
저장은 기본 구조·XML·일부 참조·교체 이미지 검사 후 다른 이름으로만 허용한다.
원본 경로와 동일 파일을 가리키는 하드링크를 거부한다. CSS 전체 문법과 EPUBCheck를
대체하지 않으므로 출판용 표준 적합성 검증은 별도로 수행한다.

생성 자료와 자동 입력 이벤트 검증은 실제 서적·실제 한글 IME 후보 창·화면 읽기
프로그램의 수동 검수를 대신하지 않는다. 세부 판정은 ``qualification-policy.json`` 에 기록한다.
