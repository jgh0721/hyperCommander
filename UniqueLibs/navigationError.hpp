#pragma once
#include "UniqueLibs/errorPresentation.hpp"

#include "commonLibs/storage/storage.hpp"
#include <QDir>
#include <QObject>
#include <QString>
#include <qt_windows.h>
#include <iterator>

namespace hc::ui
{
    struct NavigationErrorText
    {
        QString summary_;
        QString recovery_;
        QString details_;
    };

    inline NavigationErrorText DescribeNavigationError(const storage::Error& Error, const QString& Path)
    {
        using storage::ErrorCategory;
        NavigationErrorText Text;
        auto NativePath = QDir::toNativeSeparators(Path);
        const auto Code = QString::fromStdString(Error.code_);
        const bool NetworkFailure = Code == "win32:53" || Code == "win32:59" || Code == "win32:64"
            || Code == "win32:67" || Code == "win32:1201" || Code == "win32:1219" || Code == "win32:1222"
            || Code == "win32:1231" || Code == "win32:1232" || Code == "win32:1326";
        const bool Remote = NativePath.startsWith("\\\\") || NetworkFailure;
        const bool Redirected = NativePath.compare("\\\\tsclient", Qt::CaseInsensitive) == 0
            || NativePath.startsWith("\\\\tsclient\\", Qt::CaseInsensitive);
        if (Error.category_ == ErrorCategory::Cancelled) {
            Text.summary_ = QObject::tr("폴더 목록 조회를 취소했습니다.");
            Text.recovery_ = QObject::tr("다시 시도를 누르면 같은 폴더를 다시 조회합니다.");
        } else if (Error.category_ == ErrorCategory::AccessDenied) {
            Text.summary_ = QObject::tr("이 폴더에 접근할 권한이 없습니다.");
            Text.recovery_ = Remote ? QObject::tr("연결 계정의 공유 폴더 접근 권한을 확인한 뒤 다시 시도하세요.")
                : QObject::tr("폴더 접근 권한을 확인하세요. 관리자 권한으로도 시스템 보호 폴더에 접근하지 못할 수 있습니다.");
        } else if (Redirected) {
            Text.summary_ = QObject::tr("원격 데스크톱에서 공유한 드라이브나 폴더를 열지 못했습니다.");
            Text.recovery_ = QObject::tr("원격 데스크톱 연결이 유지되고 있는지 확인하세요. 연결 옵션의 ‘로컬 리소스 → 자세히 → 드라이브’에서 공유할 드라이브를 선택한 뒤 다시 연결하세요.");
        } else if (Error.category_ == ErrorCategory::NotFound) {
            Text.summary_ = QObject::tr("폴더를 찾을 수 없습니다.");
            Text.recovery_ = QObject::tr("폴더가 이동하거나 삭제되었는지 확인하고, 경로를 다시 입력하세요.");
        } else if (Error.category_ == ErrorCategory::BudgetExceeded) {
            Text.summary_ = QObject::tr("폴더 목록이 조회 한도를 초과했습니다.");
            Text.recovery_ = QObject::tr("필요한 하위 폴더의 경로를 직접 입력해 여세요.");
        } else if (Error.category_ == ErrorCategory::Unsupported) {
            Text.summary_ = QObject::tr("이 위치에서는 폴더 목록 조회를 지원하지 않습니다.");
            Text.recovery_ = QObject::tr("이전 위치로 돌아가거나 다른 폴더 경로를 입력하세요.");
        } else if (Remote) {
            Text.summary_ = QObject::tr("네트워크 폴더에 연결하지 못했습니다.");
            Text.recovery_ = QObject::tr("서버와 네트워크 연결 상태, 공유 이름을 확인한 뒤 다시 시도하세요.");
        } else {
            Text.summary_ = QObject::tr("폴더 목록을 읽지 못했습니다.");
            Text.recovery_ = QObject::tr("드라이브 연결 상태와 폴더 경로를 확인한 뒤 다시 시도하세요.");
        }
        Text.details_ = QObject::tr("경로: %1\n오류 코드: %2").arg(NativePath, Code);
        if (!Error.nativePath_.empty() && QString::fromStdString(Error.nativePath_) != Path)
            Text.details_ += QObject::tr("\n실패한 항목: %1").arg(QDir::toNativeSeparators(QString::fromStdString(Error.nativePath_)));
        const auto Presented = DescribeError(Error);
        Text.details_ += '\n' + Presented.Summary();
        return Text;
    }
}
