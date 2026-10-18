#pragma once

#include <QWidget>

/// Win32 화면 캡처 방지 유틸리티
namespace ScreenProtect {

enum class ProtectionMode {
	Off,
	Monitor,
	ExcludeFromCapture
};

/// SetWindowDisplayAffinity 를 사용하여 캡처 방지 설정
void setProtectionMode(QWidget* window, ProtectionMode mode);

/// 하위 호환용 on/off 설정
void setProtection(QWidget* window, bool enabled);

/// 현재 보호 모드 조회
ProtectionMode protectionMode(QWidget* window);

/// 현재 보호 상태 조회
bool isProtected(QWidget* window);

} // namespace ScreenProtect

