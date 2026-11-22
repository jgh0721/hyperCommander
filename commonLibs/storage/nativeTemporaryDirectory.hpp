#pragma once

/// 공통 라이브러리
#include "nativePath.hpp"
#include "storage.hpp"

/// 플랫폼/프레임워크 헤더파일
#include <QDir>
#include <QTemporaryDir>
#include <QUuid>
#include <Windows.h>

namespace hc::storage::native
{
    class TemporaryDirectory final
    {
    public:
        TemporaryDirectory(const QString& Parent, const QString& Prefix) : parent_(Parent)
        {
            const QDir Directory(Parent);
            const auto Root = QDir::toNativeSeparators(Parent.left(3));
            const bool Remote = Parent.startsWith("//") || Parent.startsWith("\\\\")
                || (Root.size() == 3 && Root[1] == ':' && GetDriveTypeW(Root.toStdWString().c_str()) == DRIVE_REMOTE);
            if (!Remote) {
                QTemporaryDir Local(Directory.filePath(Prefix + "XXXXXX"));
                if (Local.isValid()) {
                    path_ = Local.path();
                    Local.setAutoRemove(false);
                }
                return;
            }

            // Qt's owner-only directory sends a client SID as the explicit owner.
            // SMB servers can reject that SID even when the share allows creation.
            // Choose an exclusive name and let the server inherit permissions.
            for (unsigned Attempt = 0; Attempt < 16; ++Attempt) {
                auto Candidate = Directory.filePath(Prefix + QUuid::createUuid().toString(QUuid::Id128));
                const auto Native = ExtendedPath(Candidate.toStdWString());
                if (CreateDirectoryW(Native.c_str(), nullptr)) {
                    path_ = std::move(Candidate);
                    return;
                }
                nativeError_ = GetLastError();
                if (nativeError_ != ERROR_ALREADY_EXISTS && nativeError_ != ERROR_FILE_EXISTS)
                    return;
            }
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
        ~TemporaryDirectory() { Remove(); }

        bool IsValid() const { return !path_.isEmpty(); }
        const QString& Path() const { return path_; }
        QString FilePath(const QString& Name) const { return QDir(path_).filePath(Name); }
        Error Failure(const char* Code, ProviderId Id) const
        {
            Error Result{ErrorCategory::Unavailable, nativeError_ ? "win32:" + std::to_string(nativeError_) : Code, std::move(Id)};
            Result.nativePath_ = parent_.toStdString();
            return Result;
        }
        bool Remove()
        {
            if (path_.isEmpty()) return true;
            return QDir(std::exchange(path_, QString{})).removeRecursively();
        }

    private:
        QString parent_, path_;
        DWORD nativeError_ = ERROR_SUCCESS;
    };
}
