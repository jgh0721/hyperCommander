# Toolbar icon development notes

이 디렉터리의 SVG는 툴바 액션 버튼 전용 자산입니다.

## 단일 메타데이터 소스

- 단일 소스 파일: `resources/icons/toolbar/manifest.json`
- 앱 런타임: `src/core/ToolbarIcons.cpp`가 위 manifest를 읽어 액션 메타를 구성합니다.
- 개발용 연락시트: `scripts/Generate-ToolbarPreview.ps1`가 같은 manifest로 `resources/preview.html`을 생성합니다.

## 추가 규칙

새 툴바 버튼(액션)을 추가할 때는 아래 4가지를 **항상 함께** 업데이트합니다.

1. `resources/icons/toolbar/manifest.json`
   - `action key`, `icon-name`, `tooltip`, `checkable`, 섹션/그룹 배치 등록
2. `resources/icons/toolbar/*.svg`
   - `icon-name.svg` 추가
3. 해당 툴바 코드
   - `MV_CREATE_TOOLBAR_ACTION()` 또는 `MV_APPLY_TOOLBAR_ACTION()` 사용
4. `resources/preview.html`
   - 스크립트로 재생성

```powershell
Set-Location "D:\Dev\MultiViewer"
powershell -ExecutionPolicy Bypass -File ".\scripts\Generate-ToolbarPreview.ps1"
```

## 검증 정책

- 등록되지 않은 `action key` 사용 시 경고를 출력합니다.
- 등록은 되었지만 SVG 리소스가 없으면 경고를 출력합니다.
- 위 두 경우 모두 공통 대체 아이콘 `missing-icon.svg`를 사용합니다.

## 상태 규칙

- Light / Dark
- Normal / Active / Disabled
- Off / On
- Hover 상태는 `Active`로 취급합니다.

## 현재 범위

- 우선 `QAction` 기반 툴바 버튼만 아이콘화합니다.
- `QComboBox`, `QSpinBox`, 사용자 정의 `QToolButton` 같은 입력 위젯은 추후 범위입니다.

