#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일
#include "text/TextMinimapSettings.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들
class TextMinimapSource;

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QByteArray>
#include <QByteArrayView>
#include <QElapsedTimer>
#include <QPair>
#include <QVector>

// ── 윤곽 ────────────────────────────────────────────────────────────────

// First non-blank column and last content column of one line, both capped at 128 display columns.
// Same rule as HyperCompare's BuildOverviewDocument(): at most 512 bytes decoded, wide characters count two columns.
struct TextMinimapStroke
{
    quint8 indent_ = 0;
    quint8 end_ = 0;
    bool Empty() const { return end_ == 0; }
    bool operator==(const TextMinimapStroke&) const = default;
};

constexpr int TextMinimapPrefixBytes = 512;
constexpr int TextMinimapMaxColumns = 128;

int TextMinimapCharColumns(char32_t Code);
TextMinimapStroke TextMinimapStrokeOf(QByteArrayView Utf8, int TabWidth);

// ── 변경 이력 ──────────────────────────────────────────────────────────

enum class TextMinimapChange : quint8 { None, Modified, Saved };
// History markers (SCI_MARKERGET bits 21..24): unsaved changes and changes reverted to a modified state count as
// Modified; Saved stays Saved; a change reverted to the original text is not shown.
TextMinimapChange TextMinimapChangeOf(int Marks);

// Sorted, disjoint inclusive line ranges. Candidate lines for history queries and the like.
class TextMinimapLineSet
{
public:
    void Add(int First, int Last);
    // Keeps ranges in step with an edit that added (Delta > 0) or removed (Delta < 0) lines after Line.
    void Shift(int Line, int Delta);
    void Clear() { ranges_.clear(); }
    bool Empty() const { return ranges_.isEmpty(); }
    int LineCount() const;
    const QVector<QPair<int, int>>& Ranges() const { return ranges_; }

private:
    QVector<QPair<int, int>> ranges_;
};

// ── 형태 판정 ──────────────────────────────────────────────────────────

struct TextMinimapMode
{
    enum class Column { Hidden, Document, Outline };
    Column column_ = Column::Hidden;
    bool overview_ = false;
    bool narrow_ = false;
    bool large_ = false;
};

// ViewportWidth is the editor's logical viewport width before the minimap strip is taken. A narrow editor hides the
// column; it comes back only at NarrowWidth + TextMinimapNarrowHysteresis so resizing near the limit does not flicker.
constexpr int TextMinimapNarrowHysteresis = 24;
TextMinimapMode TextMinimapResolveMode(const TextMinimapSettings& Settings, int Lines, qint64 Bytes, bool LimitedPreview,
                                       int ViewportWidth, bool WasNarrow);

// ── 좌표 ───────────────────────────────────────────────────────────────

// Document thumbnail: RowHeight logical pixels per display row. When the thumbnail is taller than the widget it
// scrolls with the editor: slider top = scroll ratio × (height − slider height).
struct TextMinimapDocumentGeometry
{
    double rowHeight_ = 2;
    double mapTop_ = 0;
    double sliderTop_ = 0;
    double sliderHeight_ = 0;
    bool scrolls_ = false;
    double YOfRow(double Row) const { return Row * rowHeight_ - mapTop_; }
    double RowAt(double Y) const { return (Y + mapTop_) / rowHeight_; }
};
TextMinimapDocumentGeometry TextMinimapDocumentGeometryFor(int DisplayRows, int FirstRow, int RowsOnScreen, int Height, int RowHeight);

// Outline and overview: the whole document fits the height, at most MaxPerRow pixels per row.
struct TextMinimapFitGeometry
{
    double padding_ = 0;
    double perRow_ = 0;
    double sliderTop_ = 0;
    double sliderHeight_ = 0;
    double YOfRow(double Row) const { return padding_ + Row * perRow_; }
    double RowAt(double Y) const { return perRow_ > 0 ? (Y - padding_) / perRow_ : 0; }
};
TextMinimapFitGeometry TextMinimapFitGeometryFor(int DisplayRows, int FirstRow, int RowsOnScreen, int Height, double Padding,
                                                 double MaxPerRow, double MinSlider);

// ── 윤곽 구간 ──────────────────────────────────────────────────────────

// Outline bins over display rows, built in time slices on the UI thread (Scintilla is not thread safe).
class TextMinimapOutline
{
public:
    struct Bin
    {
        TextMinimapStroke stroke_;
        bool done_ = false;
    };
    static constexpr int SamplesPerBin = 48;

    void Reset(int BinCount, int DisplayRows);
    // Works until Budget runs out; returns true when every bin is done.
    bool Step(const TextMinimapSource& Source, const QElapsedTimer& Clock, qint64 BudgetMs);
    // Marks the bins covering [FirstRow, LastRow] for rebuilding.
    void Invalidate(int FirstRow, int LastRow);
    bool Complete() const { return next_ >= bins_.size() && dirty_.isEmpty(); }
    int BinCount() const { return int(bins_.size()); }
    int DisplayRows() const { return rows_; }
    const QVector<Bin>& Bins() const { return bins_; }

private:
    void build(const TextMinimapSource& Source, int Index);
    QVector<Bin> bins_;
    QVector<int> dirty_;
    int rows_ = 0;
    int next_ = 0;
};

// ── 검색 ───────────────────────────────────────────────────────────────

// Counts and collects matches for the find bar and the minimap in one pass, in time slices. Zero-length matches are
// skipped like the find bar does; the next search starts one character later so UTF-8 boundaries hold.
class TextMinimapSearch
{
public:
    static constexpr int StoredLimit = 10000;
    static constexpr int DensityBins = 1024;
    struct Hit
    {
        qint64 start_ = 0;
        qint64 end_ = 0;
        int line_ = 0;
    };
    struct Key
    {
        qintptr document_ = 0;
        quint64 revision_ = 0;
        QByteArray text_;
        int flags_ = 0;
        qint64 start_ = 0;
        qint64 end_ = -1;
        bool operator==(const Key&) const = default;
    };

    void Start(const Key& Search, qint64 DocumentLength, int LineCount);
    void Clear();
    // Returns true when the whole range was searched.
    bool Step(const TextMinimapSource& Source, const QElapsedTimer& Clock, qint64 BudgetMs);
    bool Active() const { return !key_.text_.isEmpty(); }
    bool Complete() const { return complete_; }
    const Key& CurrentKey() const { return key_; }
    int Count() const { return count_; }
    bool Truncated() const { return count_ > hits_.size(); }
    const QVector<Hit>& Hits() const { return hits_; }
    // Matches per line bin for documents with more matches than StoredLimit.
    const QVector<int>& Density() const { return density_; }
    int DensityLines() const { return lines_; }

private:
    Key key_;
    qint64 cursor_ = 0;
    qint64 end_ = 0;
    int lines_ = 1;
    int count_ = 0;
    bool complete_ = false;
    QVector<Hit> hits_;
    QVector<int> density_;
};

// ── 문제 ───────────────────────────────────────────────────────────────

struct TextMinimapProblem
{
    int line_ = 0;
    bool error_ = false;
};
// Moves problem lines with an edit; problems inside removed lines stay on the line that absorbed them.
void TextMinimapShiftProblems(QVector<TextMinimapProblem>& Problems, int Line, int Delta);
