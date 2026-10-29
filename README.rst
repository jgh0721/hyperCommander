==============
HyperCommander
==============

Windows용 2패널 파일 관리자. Qt 기반으로 파일 탐색·작업, 문서 열람·편집,
파일 비교 기능을 제공한다. AI의 도움으로 제작하였다.

구성 요소
=========

* **HyperCommander**: 파일 탐색 및 관리
* **HyperCompare**: 파일·폴더 비교
* **HyperViewer / HyperEditor**: 문서 열람 및 편집

주요 기능
=========

* 2패널·탭 기반 탐색, 즐겨찾기, 파일 검색
* 파일 복사·이동·삭제·이름 변경, 네트워크 경로 탐색
* 압축 파일 탐색·생성·해제 및 플러그인 연동
* 텍스트·이미지·PDF·EPUB 등 다양한 형식의 열람
* 단축키, 테마, 파일 색상 및 열 구성 설정

개발 환경
=========

* Windows x64, Visual Studio 2026 (v145)
* Qt 6.12.0, Hybrid CRT
* CMake 4.2 이상

의존 라이브러리
---------------

* QtitanDataGrid 
* waterColorStyle( https://github.com/jgh0721/waterColorStyle )
* PDFium ( https://github.com/bblanchon/pdfium-binaries )
* Scintilla/Lexilla ( https://scintilla.org/ )
* rhwp ( https://github.com/edwardkim/rhwp )
* 7-zip ( https://7-zip.org/ )
* libfmt ( https://github.com/fmtlib/fmt )

빌드 방법
=========

저장소 루트에서 PowerShell로 실행한다. Qt 설치 경로는
``CMakePresets.json``의 ``QTDIR``과 일치해야 한다.

.. code-block:: powershell

   $qtCmake = 'D:\Qt\Qt-6.12.0-VC2026-X64-H-MD-2026-10-10\bin\qt-cmake.bat'
   & $qtCmake --preset vc2026-qt612
   & $qtCmake --build --preset vc2026-release

Debug 빌드는 ``vc2026-debug`` 프리셋을 사용한다.

라이선스
========

외부 라이브러리와 플러그인의 라이선스는 각 구성 요소의 고지 파일을 참조한다.
