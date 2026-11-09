#pragma once

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/columns/columnContracts.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QPointer>
#include <QJsonObject>
#include <QSet>

class CGridView;
class QWidget;
namespace hc::columns
{
    struct ViewSnapshot
    {
        QPointer<CGridView> view_;
        QJsonObject workspace_, scroll_;
        QStringList selection_;
        QString focus_;
    };
    void CaptureView(CGridView* View, QVector<ViewSnapshot>& Previous);
    void RestoreViews(const QVector<ViewSnapshot>& Previous);
    bool SyncOpenViews(QWidget* Window, const QHash<QUuid, ColumnProfile>& Changed,
        const QSet<CGridView*>& Skip, QVector<ViewSnapshot>& Previous);
}
