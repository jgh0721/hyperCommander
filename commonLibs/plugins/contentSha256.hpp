#pragma once

#include <QtCore/qt_windows.h>
#include <bcrypt.h>
#include <QCryptographicHash>

namespace hc::plugins {
class ContentSha256 final {
public:
    ContentSha256() = default;
    ContentSha256(const ContentSha256&) = delete;
    ContentSha256& operator=(const ContentSha256&) = delete;
    ~ContentSha256() { if (handle_) BCryptDestroyHash(handle_); }
    bool Add(const void* Data, ULONG Size) {
        if (!handle_ && BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &handle_, nullptr, 0, nullptr, 0, 0) < 0) return false;
        return BCryptHashData(handle_, static_cast<PUCHAR>(const_cast<void*>(Data)), Size, 0) >= 0;
    }
    QByteArray Finish() {
        if (!handle_) return QCryptographicHash::hash({}, QCryptographicHash::Sha256);
        QByteArray Digest(32, Qt::Uninitialized);
        const auto Code = BCryptFinishHash(handle_, reinterpret_cast<PUCHAR>(Digest.data()), ULONG(Digest.size()), 0);
        BCryptDestroyHash(handle_);
        handle_ = nullptr;
        return Code < 0 ? QByteArray{} : Digest;
    }
private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};
}
