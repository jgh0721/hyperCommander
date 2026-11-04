#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QJsonObject>
#include <QString>
#include <functional>

class QWidget;

namespace hc::document
{
    class EpubProcessView;

    /// Body menu of the EPUB reader (prototype MODES.epub ctx.body). The reader process reports the right click
    /// (EpubProcessView::ContextMenuRequested); the host shows this menu at the reported point.
    /// Request: contextRequest of the reader snapshot. EditAt opens the editor at a reading locator and chapter;
    /// an empty function disables '편집 모드에서 이 위치 열기'.
    void ShowEpubReaderContextMenu( EpubProcessView* View, QWidget* Parent, const QJsonObject& Request,
                                    std::function< void( const QString& Locator, int Chapter ) > EditAt );
}
