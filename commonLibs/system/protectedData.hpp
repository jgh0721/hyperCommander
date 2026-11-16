#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <Windows.h>
#include <wincrypt.h>
#include <QByteArray>

namespace hc::system {
// Shared by non-UI stores. Do not depend on the password manager's Widgets target
// from the privileged registry helper. DPAPI failure never falls back to plaintext.
inline QByteArray ProtectUserData(const QByteArray& Plain)
{
    if (Plain.isEmpty() || Plain.size() > 256 * 1024 * 1024) return {};
    DATA_BLOB Input{DWORD(Plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(Plain.constData()))}, Output{};
    if (!CryptProtectData(&Input, L"HyperCommander protected data", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Output)) return {};
    QByteArray Result(reinterpret_cast<char*>(Output.pbData), Output.cbData);
    SecureZeroMemory(Output.pbData, Output.cbData); LocalFree(Output.pbData); return Result;
}
inline QByteArray RevealUserData(const QByteArray& Protected)
{
    if (Protected.isEmpty() || Protected.size() > 256 * 1024 * 1024) return {};
    DATA_BLOB Input{DWORD(Protected.size()), reinterpret_cast<BYTE*>(const_cast<char*>(Protected.constData()))}, Output{};
    if (!CryptUnprotectData(&Input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Output)) return {};
    QByteArray Result(reinterpret_cast<char*>(Output.pbData), Output.cbData);
    SecureZeroMemory(Output.pbData, Output.cbData); LocalFree(Output.pbData); return Result;
}
}
