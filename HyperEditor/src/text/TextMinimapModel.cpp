#include "stdafx.h"
#include "text/TextMinimapModel.h"

/// 모듈

/// UIs

/// 기타 헤더파일
#include "text/TextMinimapSource.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <Scintilla.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QChar>
#include <QString>
#include <algorithm>

// ── 윤곽 ────────────────────────────────────────────────────────────────

int TextMinimapCharColumns(char32_t Code)
{
    const auto Category = QChar::category(Code);
    if (Category == QChar::Mark_NonSpacing || Category == QChar::Mark_Enclosing || Code == 0x200d)
        return 0;
    const bool Wide = (Code >= 0x1100 && Code <= 0x115f) || (Code >= 0x2e80 && Code <= 0xa4cf) ||
        (Code >= 0xac00 && Code <= 0xd7a3) || (Code >= 0xf900 && Code <= 0xfaff) ||
        (Code >= 0xfe10 && Code <= 0xfe6f) || (Code >= 0xff01 && Code <= 0xff60) ||
        (Code >= 0xffe0 && Code <= 0xffe6) || Code >= 0x1f300;
    return Wide ? 2 : 1;
}

TextMinimapStroke TextMinimapStrokeOf(QByteArrayView Utf8, int TabWidth)
{
    const auto Prefix = QString::fromUtf8(Utf8.first(qMin<qsizetype>(Utf8.size(), TextMinimapPrefixBytes))).toUcs4();
    const int Tab = qMax(1, TabWidth);
    int Column = 0, Indent = 0, End = 0;
    bool Content = false;
    for (const auto Code : Prefix) {
        if (Code == '\r' || Code == '\n' || Column >= TextMinimapMaxColumns)
            break;
        if (Code == '\t')
            Column = qMin(TextMinimapMaxColumns, (Column / Tab + 1) * Tab);
        else
            Column = qMin(TextMinimapMaxColumns, Column + TextMinimapCharColumns(Code));
        if (!Content && QChar::isSpace(Code))
            Indent = Column;
        else if (!QChar::isSpace(Code)) {
            Content = true;
            End = Column;
        }
    }
    if (!Content)
        return {};
    return {quint8(qMin(Indent, End)), quint8(End)};
}

// ── 변경 이력 ──────────────────────────────────────────────────────────

TextMinimapChange TextMinimapChangeOf(int Marks)
{
    if (Marks & ((1 << SC_MARKNUM_HISTORY_MODIFIED) | (1 << SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED)))
        return TextMinimapChange::Modified;
    if (Marks & (1 << SC_MARKNUM_HISTORY_SAVED))
        return TextMinimapChange::Saved;
    return TextMinimapChange::None;
}

void TextMinimapLineSet::Add(int First, int Last)
{
    if (Last < First)
        std::swap(First, Last);
    First = qMax(0, First);
    if (Last < 0)
        return;
    QVector<QPair<int, int>> Merged;
    Merged.reserve(ranges_.size() + 1);
    bool Placed = false;
    for (const auto& Range : ranges_) {
        if (Range.second + 1 < First) { Merged.append(Range); continue; }
        if (Last + 1 < Range.first) {
            if (!Placed) { Merged.append({First, Last}); Placed = true; }
            Merged.append(Range);
            continue;
        }
        First = qMin(First, Range.first);
        Last = qMax(Last, Range.second);
    }
    if (!Placed)
        Merged.append({First, Last});
    std::sort(Merged.begin(), Merged.end());
    ranges_ = std::move(Merged);
}

void TextMinimapLineSet::Shift(int Line, int Delta)
{
    if (Delta == 0 || ranges_.isEmpty())
        return;
    const auto Map = [Line, Delta](int Value) {
        if (Delta > 0)
            return Value > Line ? Value + Delta : Value;
        const int Removed = -Delta;
        if (Value <= Line) return Value;
        if (Value <= Line + Removed) return Line;
        return Value - Removed;
    };
    const auto Old = std::exchange(ranges_, {});
    for (const auto& Range : Old) {
        // A range that spans the insertion point grows to cover the new lines (a superset is fine for candidates).
        const int First = Map(Range.first);
        const int Last = Delta > 0 && Range.first <= Line && Range.second > Line ? Range.second + Delta : Map(Range.second);
        Add(First, Last);
    }
}

int TextMinimapLineSet::LineCount() const
{
    int Count = 0;
    for (const auto& Range : ranges_)
        Count += Range.second - Range.first + 1;
    return Count;
}

// ── 형태 판정 ──────────────────────────────────────────────────────────

TextMinimapMode TextMinimapResolveMode(const TextMinimapSettings& Settings, int Lines, qint64 Bytes, bool LimitedPreview,
                                       int ViewportWidth, bool WasNarrow)
{
    TextMinimapMode Mode;
    if (!Settings.enabled_)
        return Mode;
    const bool HugeBytes = Bytes >= Settings.largeBytes_;
    Mode.large_ = Lines >= Settings.largeLines_ || HugeBytes || LimitedPreview;
    Mode.narrow_ = ViewportWidth < Settings.narrowWidth_ + (WasNarrow ? TextMinimapNarrowHysteresis : 0);
    switch (Settings.kind_) {
    case TextMinimapSettings::Kind::Overview:
        Mode.column_ = TextMinimapMode::Column::Hidden;
        Mode.overview_ = true;
        break;
    case TextMinimapSettings::Kind::Document:
        // Very large bytes or the limited preview never get the thumbnail: its per-line reads are bounded, but
        // idle styling of such a buffer is not.
        Mode.column_ = HugeBytes || LimitedPreview ? TextMinimapMode::Column::Outline : TextMinimapMode::Column::Document;
        break;
    case TextMinimapSettings::Kind::Outline:
        Mode.column_ = TextMinimapMode::Column::Outline;
        break;
    case TextMinimapSettings::Kind::Auto:
    default:
        Mode.column_ = Mode.large_ ? TextMinimapMode::Column::Outline : TextMinimapMode::Column::Document;
        Mode.overview_ = true;
        break;
    }
    if (Mode.narrow_ && Mode.column_ != TextMinimapMode::Column::Hidden) {
        Mode.column_ = TextMinimapMode::Column::Hidden;
        Mode.overview_ = true;
    }
    return Mode;
}

// ── 좌표 ───────────────────────────────────────────────────────────────

TextMinimapDocumentGeometry TextMinimapDocumentGeometryFor(int DisplayRows, int FirstRow, int RowsOnScreen, int Height, int RowHeight)
{
    TextMinimapDocumentGeometry Geometry;
    Geometry.rowHeight_ = qMax(1, RowHeight);
    const int Rows = qMax(1, DisplayRows);
    const double Content = Rows * Geometry.rowHeight_;
    Geometry.sliderHeight_ = qBound(6.0, RowsOnScreen * Geometry.rowHeight_, qMax(6.0, double(Height)));
    const int MaxFirst = qMax(0, Rows - RowsOnScreen);
    const double Ratio = MaxFirst > 0 ? qBound(0.0, double(FirstRow) / MaxFirst, 1.0) : 0.0;
    Geometry.scrolls_ = Content > Height;
    Geometry.mapTop_ = Geometry.scrolls_ ? Ratio * (Content - Height) : 0.0;
    Geometry.sliderTop_ = Geometry.YOfRow(FirstRow);
    return Geometry;
}

TextMinimapFitGeometry TextMinimapFitGeometryFor(int DisplayRows, int FirstRow, int RowsOnScreen, int Height, double Padding,
                                                 double MaxPerRow, double MinSlider)
{
    TextMinimapFitGeometry Geometry;
    Geometry.padding_ = Padding;
    const double Available = qMax(1.0, Height - 2 * Padding);
    Geometry.perRow_ = qMin(MaxPerRow, Available / qMax(1, DisplayRows));
    Geometry.sliderTop_ = Geometry.YOfRow(FirstRow);
    Geometry.sliderHeight_ = qMax(MinSlider, RowsOnScreen * Geometry.perRow_);
    return Geometry;
}

// ── 윤곽 구간 ──────────────────────────────────────────────────────────

void TextMinimapOutline::Reset(int BinCount, int DisplayRows)
{
    bins_ = QVector<Bin>(qBound(0, BinCount, 4096));
    dirty_.clear();
    rows_ = qMax(1, DisplayRows);
    next_ = 0;
}

void TextMinimapOutline::Invalidate(int FirstRow, int LastRow)
{
    if (bins_.isEmpty())
        return;
    const int Count = int(bins_.size());
    const int First = qBound(0, int(qint64(qMax(0, FirstRow)) * Count / rows_), Count - 1);
    const int Last = qBound(First, int(qint64(qMax(0, LastRow)) * Count / rows_), Count - 1);
    for (int Index = First; Index <= Last; ++Index) {
        if (Index >= next_ || dirty_.contains(Index))
            continue;
        bins_[Index].done_ = false;
        dirty_.append(Index);
    }
}

void TextMinimapOutline::build(const TextMinimapSource& Source, int Index)
{
    const int Count = int(bins_.size());
    const int FirstRow = int(qint64(Index) * rows_ / Count);
    const int LastRow = qMax(FirstRow, int(qint64(Index + 1) * rows_ / Count) - 1);
    const int FirstLine = Source.LineOfRow(FirstRow);
    const int LastLine = qMax(FirstLine, Source.LineOfRow(LastRow));
    // Rounded up so a bin never reads more than SamplesPerBin lines.
    const int Step = qMax(1, (LastLine - FirstLine + SamplesPerBin) / SamplesPerBin);
    const int Tab = Source.TabWidth();
    int Indent = TextMinimapMaxColumns, End = 0;
    for (int Line = FirstLine; Line <= LastLine; Line += Step) {
        const qint64 Start = Source.LineStart(Line);
        const auto Stroke = TextMinimapStrokeOf(Source.Text(Start, qMin(Source.LineEnd(Line), Start + TextMinimapPrefixBytes)), Tab);
        if (Stroke.Empty())
            continue;
        Indent = qMin(Indent, int(Stroke.indent_));
        End = qMax(End, int(Stroke.end_));
    }
    bins_[Index] = {End > 0 ? TextMinimapStroke{quint8(qMin(Indent, End)), quint8(End)} : TextMinimapStroke{}, true};
}

bool TextMinimapOutline::Step(const TextMinimapSource& Source, const QElapsedTimer& Clock, qint64 BudgetMs)
{
    while (!dirty_.isEmpty() || next_ < bins_.size()) {
        if (Clock.elapsed() >= BudgetMs)
            return false;
        const int Index = !dirty_.isEmpty() ? dirty_.takeFirst() : next_++;
        build(Source, Index);
    }
    return true;
}

// ── 검색 ───────────────────────────────────────────────────────────────

void TextMinimapSearch::Start(const Key& Search, qint64 DocumentLength, int LineCount)
{
    key_ = Search;
    cursor_ = qMax<qint64>(0, Search.start_);
    end_ = Search.end_ < 0 ? DocumentLength : qMin(DocumentLength, Search.end_);
    lines_ = qMax(1, LineCount);
    count_ = 0;
    complete_ = key_.text_.isEmpty();
    hits_.clear();
    density_ = QVector<int>(DensityBins, 0);
}

void TextMinimapSearch::Clear()
{
    key_ = {};
    hits_.clear();
    density_.clear();
    count_ = 0;
    complete_ = true;
}

bool TextMinimapSearch::Step(const TextMinimapSource& Source, const QElapsedTimer& Clock, qint64 BudgetMs)
{
    if (complete_)
        return true;
    constexpr qint64 Window = 256 * 1024;
    while (cursor_ < end_) {
        if (Clock.elapsed() >= BudgetMs)
            return false;
        // Windows end on a line end: a match never spans lines (the find bar is single-line), so none is cut.
        qint64 WindowEnd = qMin(end_, Source.LineEnd(Source.LineFromPosition(qMin(end_, cursor_ + Window))));
        if (WindowEnd <= cursor_)
            WindowEnd = end_;
        qint64 MatchEnd = -1;
        const qint64 Found = Source.Find(key_.text_, key_.flags_, cursor_, WindowEnd, MatchEnd);
        if (Found < 0) {
            cursor_ = WindowEnd;
            continue;
        }
        if (MatchEnd <= Found) {
            const qint64 After = Source.PositionAfter(Found);
            cursor_ = After > Found ? After : Found + 1;
            continue;
        }
        const int Line = Source.LineFromPosition(Found);
        if (hits_.size() < StoredLimit)
            hits_.append({Found, MatchEnd, Line});
        ++density_[qBound(0, int(qint64(Line) * DensityBins / lines_), DensityBins - 1)];
        ++count_;
        cursor_ = MatchEnd;
    }
    complete_ = true;
    return true;
}

// ── 문제 ───────────────────────────────────────────────────────────────

void TextMinimapShiftProblems(QVector<TextMinimapProblem>& Problems, int Line, int Delta)
{
    if (Delta == 0)
        return;
    for (auto& Problem : Problems) {
        if (Delta > 0) {
            if (Problem.line_ > Line) Problem.line_ += Delta;
        } else if (Problem.line_ > Line) {
            Problem.line_ = Problem.line_ <= Line - Delta ? Line : Problem.line_ + Delta;
        }
    }
}
