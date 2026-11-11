#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들
class TextMinimapSource;

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QByteArrayView>
#include <QElapsedTimer>
#include <QString>
#include <QVector>

struct TextMinimapHeading
{
    int line_ = 0;
    int level_ = 1;
    QString text_;
};

// Collects headings for the minimap labels in time slices. Markdown reads ATX and Setext headings outside fenced
// code; HTML/XML reads one-line <h1>..<h6>; other languages use fold header lines (levels 0 and 1) when the lexer
// computes folding. Each line read is limited to the first 512 bytes.
class TextMinimapHeadings
{
public:
    enum class Kind { None, Markdown, Markup, Fold };
    static Kind KindForLanguage(const QString& Language);
    // Pure line rules, shared with the unit tests.
    static int MarkdownAtxLevel(QByteArrayView Line, QString* Title);
    static int MarkdownSetextLevel(QByteArrayView Line);
    static bool MarkdownFence(QByteArrayView Line, char* Marker);
    static int MarkupLevel(QByteArrayView Line, QString* Title);

    void Start(Kind Scan, int LineCount);
    bool Step(const TextMinimapSource& Source, const QElapsedTimer& Clock, qint64 BudgetMs);
    bool Complete() const { return next_ >= lines_; }
    Kind ScanKind() const { return kind_; }
    // The last complete scan; a running scan replaces it when it finishes.
    const QVector<TextMinimapHeading>& Headings() const { return headings_; }

private:
    Kind kind_ = Kind::None;
    int lines_ = 0;
    int next_ = 0;
    char fence_ = 0;
    QString previous_;
    int previousLine_ = -1;
    QVector<TextMinimapHeading> pending_;
    QVector<TextMinimapHeading> headings_;
};
