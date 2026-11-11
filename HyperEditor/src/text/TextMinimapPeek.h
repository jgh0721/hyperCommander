#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들
class TextMinimap;

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QColor>
#include <QFont>
#include <QStringList>
#include <QVector>
#include <QWidget>

// Hover preview: nine lines around the pointed row, drawn in the editor's font and style colours.
class TextMinimapPeek final : public QWidget
{
    Q_OBJECT

public:
    explicit TextMinimapPeek(TextMinimap* Owner, QWidget* Parent);
    void ShowAt(int CenterLine, const QRect& Anchor, int GlobalY, bool LeftOfAnchor);
    int CenterLine() const { return center_; }

protected:
    void paintEvent(QPaintEvent* Event) override;

private:
    struct Run
    {
        QString text_;
        QColor color_;
    };
    struct Row
    {
        int line_ = 0;
        QVector<Run> runs_;
    };
    TextMinimap* owner_;
    QVector<Row> rows_;
    QString header_;
    QString section_;
    QStringList chips_;
    int center_ = -1;
    QFont font_;
};
