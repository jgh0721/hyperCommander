#pragma once

#include "commonLibs/storage/storage.hpp"
#include <QObject>
#include <QRegularExpression>
#include <QStringList>
#include <qt_windows.h>
#include <optional>
#include <iterator>

namespace hc::ui
{
    enum class ErrorDomain { Application, Win32, HResult };
    enum class ErrorSeverity { Error, Information };
    enum class ErrorKind { Unknown, AccessDenied, NotFound, InUse, DiskFull, Cancelled, Timeout, Changed, InvalidInput, Unsupported, LimitExceeded, Unavailable };

    // Presentation only: never changes the operation's error, retry policy or outcome.
    struct ErrorPresentation
    {
        QString code_, description_, recovery_, operation_, path_, provider_, actionCode_;
        std::optional<storage::ErrorCategory> category_;
        bool retryable_ = false;
        ErrorDomain domain_ = ErrorDomain::Application;
        ErrorSeverity severity_ = ErrorSeverity::Error;
        ErrorKind kind_ = ErrorKind::Unknown;
        std::optional<quint32> nativeCode_;

        QString KindText() const
        {
            switch (kind_) {
            case ErrorKind::AccessDenied: return QObject::tr("접근 권한");
            case ErrorKind::NotFound: return QObject::tr("대상 없음");
            case ErrorKind::InUse: return QObject::tr("사용 중");
            case ErrorKind::DiskFull: return QObject::tr("저장 공간 부족");
            case ErrorKind::Cancelled: return QObject::tr("취소");
            case ErrorKind::Timeout: return QObject::tr("시간 초과");
            case ErrorKind::Changed: return QObject::tr("대상 변경");
            case ErrorKind::InvalidInput: return QObject::tr("잘못된 요청");
            case ErrorKind::Unsupported: return QObject::tr("지원하지 않는 작업");
            case ErrorKind::LimitExceeded: return QObject::tr("처리 한도 초과");
            case ErrorKind::Unavailable: return QObject::tr("사용 불가");
            default: return QObject::tr("기타 오류");
            }
        }
        QString Summary() const { return recovery_.isEmpty() ? description_ : description_ + '\n' + recovery_; }
        QString Details() const
        {
            if (code_.isEmpty()) return {};
            QStringList Lines{QObject::tr("오류 종류: %1").arg(KindText()), QObject::tr("오류 코드: %1").arg(code_)};
            if (nativeCode_) Lines << (domain_ == ErrorDomain::HResult
                ? QObject::tr("HRESULT: 0x%1").arg(*nativeCode_, 8, 16, QChar('0'))
                : QObject::tr("Windows 오류: %1").arg(*nativeCode_));
            if (!operation_.isEmpty()) Lines << QObject::tr("작업: %1").arg(operation_);
            if (!path_.isEmpty()) Lines << QObject::tr("대상: %1").arg(path_);
            if (!provider_.isEmpty()) Lines << QObject::tr("공급자: %1").arg(provider_);
            return Lines.join('\n');
        }
        QString Text() const { return code_.isEmpty() ? QString() : severity_ == ErrorSeverity::Information ? Summary() : Summary() + '\n' + Details(); }
    };

    namespace error_detail
    {
        inline ErrorKind Kind(storage::ErrorCategory Category)
        {
            using C = storage::ErrorCategory;
            switch (Category) {
            case C::AccessDenied: return ErrorKind::AccessDenied;
            case C::NotFound: return ErrorKind::NotFound;
            case C::Cancelled: return ErrorKind::Cancelled;
            case C::StaleRevision: return ErrorKind::Changed;
            case C::InvalidRequest: return ErrorKind::InvalidInput;
            case C::Unsupported: return ErrorKind::Unsupported;
            case C::BudgetExceeded: return ErrorKind::LimitExceeded;
            case C::Unavailable: return ErrorKind::Unavailable;
            default: return ErrorKind::Unknown;
            }
        }
        inline ErrorKind NativeKind(quint32 Code)
        {
            switch (Code) {
            case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: return ErrorKind::AccessDenied;
            case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return ErrorKind::NotFound;
            case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return ErrorKind::InUse;
            case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ErrorKind::DiskFull;
            case ERROR_CANCELLED: case ERROR_OPERATION_ABORTED: return ErrorKind::Cancelled;
            case ERROR_TIMEOUT: case WAIT_TIMEOUT: return ErrorKind::Timeout;
            case ERROR_INVALID_PARAMETER: case ERROR_INVALID_NAME: return ErrorKind::InvalidInput;
            case ERROR_NOT_SUPPORTED: return ErrorKind::Unsupported;
            case ERROR_FILENAME_EXCED_RANGE: case ERROR_BUFFER_OVERFLOW: return ErrorKind::LimitExceeded;
            case ERROR_FILE_INVALID: return ErrorKind::Changed;
            default: return ErrorKind::Unknown;
            }
        }
        inline void Defaults(ErrorPresentation& P)
        {
            switch (P.kind_) {
            case ErrorKind::AccessDenied:
                P.description_ = QObject::tr("대상에 접근할 권한이 없습니다."); P.recovery_ = QObject::tr("파일과 폴더의 접근 권한을 확인하세요."); break;
            case ErrorKind::NotFound:
                P.description_ = QObject::tr("파일 또는 폴더를 찾을 수 없습니다."); P.recovery_ = QObject::tr("대상이 이동하거나 삭제되었는지 확인하세요."); break;
            case ErrorKind::InUse:
                P.description_ = QObject::tr("다른 프로그램에서 대상을 사용하고 있습니다."); P.recovery_ = QObject::tr("사용 중인 파일을 닫은 뒤 다시 시도하세요."); break;
            case ErrorKind::DiskFull:
                P.description_ = QObject::tr("저장 공간이 부족합니다."); P.recovery_ = QObject::tr("대상 드라이브의 여유 공간을 확보하세요."); break;
            case ErrorKind::Cancelled: P.description_ = QObject::tr("작업을 취소했습니다."); break;
            case ErrorKind::Timeout:
                P.description_ = QObject::tr("작업 응답 시간이 초과되었습니다."); P.recovery_ = QObject::tr("연결 상태와 작업 결과를 확인하세요."); break;
            case ErrorKind::Changed:
                P.description_ = QObject::tr("작업을 시작한 뒤 대상이 변경되었습니다."); P.recovery_ = QObject::tr("목록을 새로 고치고 현재 파일 상태를 확인하세요."); break;
            case ErrorKind::InvalidInput: P.description_ = QObject::tr("요청한 작업의 입력이 올바르지 않습니다."); P.recovery_ = QObject::tr("이름과 경로, 작업 옵션을 확인하세요."); break;
            case ErrorKind::Unsupported: P.description_ = QObject::tr("이 대상에서는 요청한 작업을 지원하지 않습니다."); break;
            case ErrorKind::LimitExceeded: P.description_ = QObject::tr("작업이 지원하는 크기 또는 경로 한도를 초과했습니다."); P.recovery_ = QObject::tr("대상 범위와 경로 길이를 확인하세요."); break;
            case ErrorKind::Unavailable: P.description_ = QObject::tr("요청한 대상이나 서비스를 사용할 수 없습니다."); break;
            default: P.description_ = QObject::tr("요청한 작업을 완료하지 못했습니다."); P.recovery_ = QObject::tr("아래 오류 코드와 대상 정보를 확인하세요."); break;
            }
        }
        inline void Native(ErrorPresentation& P, quint32 Code, ErrorDomain Domain)
        {
            P.nativeCode_ = Code; P.domain_ = Domain;
            const auto WinCode = Domain == ErrorDomain::Win32 ? std::optional<quint32>(Code)
                : (Code & 0xffff0000u) == 0x80070000u ? std::optional<quint32>(Code & 0xffffu) : std::nullopt;
            if (WinCode) { const auto K = NativeKind(*WinCode); if (K != ErrorKind::Unknown) P.kind_ = K; }
            Defaults(P);
            const auto Previous = GetLastError(); wchar_t Message[4096]{};
            const auto Count = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, WinCode.value_or(Code), 0, Message, DWORD(std::size(Message)), nullptr);
            SetLastError(Previous);
            if (Count) P.description_ = QString::fromWCharArray(Message, Count).trimmed();
        }
        inline bool ParseNative(ErrorPresentation& P, const QString& Code)
        {
            // Only documented native encodings. Plugin result codes and arbitrary
            // numeric suffixes are NOT Win32 error codes.
            static const QRegularExpression Win("^(?:win32:|(?:elevation|directory)-[a-z-]+:)([0-9]+)$");
            static const QRegularExpression Hr("^(?:hresult:|native-recycle-[a-z-]+-failed-)(0[xX][0-9a-fA-F]+|[0-9]+)$");
            static const QRegularExpression Rename(QStringLiteral("^rename-failed: Windows ([0-9]+) · (.+)$"));
            auto Match = Win.match(Code); auto Domain = ErrorDomain::Win32;
            if (!Match.hasMatch()) { Match = Hr.match(Code); Domain = ErrorDomain::HResult; }
            if (!Match.hasMatch()) { Match = Rename.match(Code); Domain = ErrorDomain::Win32; if (Match.hasMatch() && P.path_.isEmpty()) P.path_ = Match.captured(2); }
            if (!Match.hasMatch()) return false;
            const auto Number = Match.captured(1); bool Valid = false;
            const auto Value = Number.toULongLong(&Valid, Number.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
            if (!Valid || Value > 0xffffffffULL) return false;
            Native(P, quint32(Value), Domain); return true;
        }
    }

    inline ErrorPresentation DescribeError(const QString& Code, storage::ErrorCategory Category = storage::ErrorCategory::Internal,
        const QString& Operation = {}, const QString& Path = {})
    {
        ErrorPresentation P; P.code_ = Code; P.kind_ = error_detail::Kind(Category); P.operation_ = Operation; P.path_ = Path;
        if (Code.isEmpty()) return P;
        auto NativeCode = Code;
        for (const auto* Prefix : {"delete-failure:", "file-in-use:", "automatic-name-failed:"})
            if (NativeCode.startsWith(QLatin1StringView(Prefix))) { NativeCode = NativeCode.mid(qstrlen(Prefix)); break; }
        if (!error_detail::ParseNative(P, NativeCode)) error_detail::Defaults(P);
        const auto Set = [&](ErrorKind Kind, const QString& Description, const QString& Recovery = {}) {
            P.kind_ = Kind; P.description_ = Description; P.recovery_ = Recovery;
        };
        // Stable product identifiers have explicit explanations; originals remain
        // in code_ for diagnostics, journals, scripts and support.
        if (Code == "job-journal-busy" || Code == "archive-create-settings-lock" || Code == "archive-create-settings-lock-wait")
            Set(ErrorKind::InUse, QObject::tr("다른 작업이 기록 또는 설정을 사용하고 있습니다."), QObject::tr("진행 중인 작업이 끝난 뒤 다시 시도하세요."));
        else if (Code == "job-journal-remove" || Code == "job-outcomes-remove" || Code == "job-writes-remove")
            Set(ErrorKind::Unavailable, QObject::tr("작업 이력 파일을 삭제하지 못했습니다."), QObject::tr("이력 폴더의 접근 권한과 파일 사용 상태를 확인하세요."));
        else if (Code == "archive-create-output-inside-input")
            Set(ErrorKind::InvalidInput, QObject::tr("압축 파일의 저장 위치가 압축할 폴더 안에 있습니다."), QObject::tr("압축 대상 폴더 밖의 저장 위치를 선택하세요."));
        else if (Code == "invalid-extract-request") Set(ErrorKind::InvalidInput, QObject::tr("압축 해제 요청의 대상이나 저장 위치가 올바르지 않습니다."));
        else if (Code == "plugin-review-required" || Code == "plugin-execution-not-approved")
            Set(ErrorKind::AccessDenied, QObject::tr("이 플러그인의 실행이 승인되지 않았습니다."), QObject::tr("플러그인 설정에서 파일과 권한을 검사하고 승인하세요."));
        else if (Code == "plugin-permission-denied")
            Set(ErrorKind::AccessDenied, QObject::tr("플러그인에 필요한 권한이 승인되지 않았습니다."), QObject::tr("플러그인 설정의 승인 권한을 확인하세요."));
        else if (Code == "plugin-host-capability")
            Set(ErrorKind::Unsupported, QObject::tr("현재 호스트에서 이 플러그인의 형식 또는 기능을 지원하지 않습니다."));
        else if (Code == "reparse-entry-denied") Set(ErrorKind::Unsupported, QObject::tr("연결 또는 재분석 지점은 안전을 위해 삭제하지 않습니다."));
        else if (Code == "revision-changed" || Code == "SaveRecoveryPreviewChanged")
            Set(ErrorKind::Changed, QObject::tr("확인 이후 파일 또는 복구 기록이 변경되었습니다."), QObject::tr("목록이나 기록을 다시 읽고 현재 상태를 확인하세요."));
        else if (Code.startsWith("elevation-outcome-unknown"))
            Set(ErrorKind::Unavailable, QObject::tr("도우미와 연결이 끊겨 변경 결과를 확정하지 못했습니다."), QObject::tr("대상·원본과 작업 기록을 확인하세요. 자동 재실행하지 않습니다."));
        else if (Code == "elevation-cleanup-pending")
            Set(ErrorKind::Unavailable, QObject::tr("도우미의 임시 파일 정리 완료를 아직 확인하지 못했습니다."), QObject::tr("작업 기록과 보존 파일을 확인하세요."));
        else if (Code.startsWith("security-status-save-failed"))
            Set(ErrorKind::Unavailable, QObject::tr("보안 작업 결과 기록을 저장하지 못했습니다."), QObject::tr("원래 권한 백업은 유지됩니다. 현재 권한과 복구 결과를 확인하세요."));
        else if (Code.startsWith("permission-history-save-failed"))
            Set(ErrorKind::Unavailable, QObject::tr("권한 작업은 처리했지만 요청 이력을 저장하지 못했습니다."), QObject::tr("작업 결과와 보안 백업을 확인하세요."));
        else if (Code == "write-recovery-original-missing") Set(ErrorKind::NotFound, QObject::tr("원본 위치와 백업을 찾을 수 없습니다."), QObject::tr("보존된 파일을 확인하세요."));
        else if (Code == "destination-added-hidden-attribute" || Code == "destination-attributes-mismatch")
            Set(ErrorKind::Changed, QObject::tr("파일 내용은 복사되었지만 대상의 파일 속성이 원본과 다릅니다."), QObject::tr("대상의 속성 지원 및 정책을 확인하세요. 이동 작업의 원본은 유지됩니다."));
        else if (Code == "recovery-committed-checkpoints") P.description_ = QObject::tr("중단 전 목적지 저장 완료 기록(건):");
        else if (Code == "recovery-artifact-preserved") P.description_ = QObject::tr("확인을 위해 보존한 파일:");
        else if (Code == "conflict-destination-preserved") P.description_ = QObject::tr("이름을 변경하여 보관한 기존 대상 파일:");
        if (Code == "recovery-committed-checkpoints" || Code == "recovery-artifact-preserved" || Code == "conflict-destination-preserved") { P.recovery_.clear(); P.severity_ = ErrorSeverity::Information; }
        if (Code.startsWith("automatic-name-failed:"))
            P.recovery_ = QObject::tr("자동 이름 변경을 완료하지 못해 해당 항목을 중단했습니다. 기존 대상을 덮어쓰지 않습니다.");
        return P;
    }
    inline ErrorPresentation DescribeError(const storage::Error& Error, const QString& Operation = {}, const QString& Path = {})
    {
        auto P = DescribeError(QString::fromStdString(Error.code_), Error.category_, Operation,
            Path.isEmpty() ? QString::fromStdString(Error.nativePath_) : Path);
        P.provider_ = QString::fromStdString(Error.provider_.value_);
        P.category_ = Error.category_; P.retryable_ = Error.retryable_; P.actionCode_ = QString::fromStdString(Error.userAction_);
        return P;
    }
    // Document advice uses the same structured result; native errors always
    // retain their Windows classification and description.
    inline ErrorPresentation DescribeDocumentError(const QString& Code)
    {
        auto P = DescribeError(Code);
        if (P.domain_ != ErrorDomain::Application || Code.isEmpty()) return P;
        if (Code.contains("external") || Code.contains("stale") || Code.contains("changed"))
            { P.kind_ = ErrorKind::Changed; P.description_ = QObject::tr("원본이 변경되었습니다. 다시 읽거나 다른 이름으로 저장하십시오."); P.recovery_.clear(); return P; }
        if (Code.contains("cancel")) { P.kind_ = ErrorKind::Cancelled; P.description_ = QObject::tr("작업을 취소했습니다."); P.recovery_.clear(); return P; }
        if (Code.contains("missing") || Code.contains("not-found")) { P.kind_ = ErrorKind::NotFound; P.description_ = QObject::tr("파일을 찾을 수 없습니다."); P.recovery_.clear(); return P; }
        if (Code.contains("denied") || Code.contains("access")) { P.kind_ = ErrorKind::AccessDenied; P.description_ = QObject::tr("파일에 접근할 권한이 없습니다."); P.recovery_.clear(); return P; }
        if (Code.contains("budget") || Code.contains("byte-limit")) { P.kind_ = ErrorKind::LimitExceeded; P.description_ = QObject::tr("텍스트는 16 MiB, 문서 입력은 64 MiB 이하를 지원합니다."); P.recovery_.clear(); return P; }
        if (Code.contains("utf8") || Code.contains("eol")) { P.kind_ = ErrorKind::InvalidInput; P.description_ = QObject::tr("문자의 인코딩과 파일 형식을 확인하십시오."); P.recovery_.clear(); return P; }
        if (Code.contains("recovery")) { P.kind_ = ErrorKind::Unavailable; P.description_ = QObject::tr("복구 사본 작업에 실패했습니다. 기존 사본을 유지합니다."); P.recovery_.clear(); return P; }
        if (Code.contains("original-save")) { P.kind_ = ErrorKind::Unsupported; P.description_ = QObject::tr("원본 저장 연결이 없습니다. 다른 이름으로 저장하십시오."); P.recovery_.clear(); return P; }
        if (Code.contains("cleanup")) { P.kind_ = ErrorKind::Unavailable; P.description_ = QObject::tr("저장은 완료했으며 정리 작업을 다시 확인해야 합니다."); P.recovery_.clear(); return P; }
        return P;
    }
    inline ErrorPresentation DescribeWin32Error(quint32 Code, const QString& Operation = {}, const QString& Path = {})
    { return DescribeError(QStringLiteral("win32:%1").arg(Code), storage::ErrorCategory::Internal, Operation, Path); }
    inline ErrorPresentation DescribeHResult(quint32 Code, const QString& Operation = {}, const QString& Path = {})
    { return DescribeError(QStringLiteral("hresult:0x%1").arg(Code, 8, 16, QChar('0')), storage::ErrorCategory::Internal, Operation, Path); }
    inline QString ErrorText(const storage::Error& Error) { return DescribeError(Error).Text(); }
    inline QString ErrorText(const QString& Code) { return DescribeError(Code).Text(); }
    inline QString ErrorText(const std::string& Code) { return ErrorText(QString::fromStdString(Code)); }
}
