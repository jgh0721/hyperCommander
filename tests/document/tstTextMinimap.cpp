#include "stdafx.h"

/// 모듈

/// UIs

/// 기타 헤더파일
#include "text/TextMinimapHeadings.h"
#include "text/TextMinimapModel.h"
#include "text/TextMinimapRenderer.h"
#include "text/TextMinimapSource.h"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <Scintilla.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QPainter>
#include <QtTest>
#include <numeric>

namespace
{
    // In-memory document: LF-separated lines, optional wrap rows and folded lines, every byte styled 0.
    class FakeSource final : public TextMinimapSource
    {
    public:
        explicit FakeSource(const QList<QByteArray>& Lines) { SetLines(Lines); }

        void SetLines(const QList<QByteArray>& Lines)
        {
            lines_ = Lines.isEmpty() ? QList<QByteArray>{QByteArray()} : Lines;
            rows_ = QVector<int>(lines_.size(), 1);
            rebuild();
        }
        void SetRows(int Line, int Rows) { rows_[Line] = Rows; rebuild(); }
        void SetMarks(int Line, int Marks) { marks_[Line] = Marks; }
        void SetFold(int Line, int Level) { folds_[Line] = Level; }
        mutable int textCalls_ = 0;
        mutable int findCalls_ = 0;

        qintptr DocumentId() const override { return 1; }
        int LineCount() const override { return int(lines_.size()); }
        qint64 Length() const override { return text_.size(); }
        int DisplayRows() const override { return rowStarts_.back(); }
        int RowOfLine(int Line) const override { return rowStarts_[qBound(0, Line, int(lines_.size()))]; }
        int LineOfRow(int Row) const override
        {
            const auto It = std::upper_bound(rowStarts_.cbegin(), rowStarts_.cend() - 1, Row);
            return qBound(0, int(It - rowStarts_.cbegin()) - 1, int(lines_.size()) - 1);
        }
        int FirstVisibleRow() const override { return first_; }
        int RowsOnScreen() const override { return onScreen_; }
        void ScrollToRow(int Row) override { first_ = Row; }
        qint64 LineStart(int Line) const override { return starts_[qBound(0, Line, int(lines_.size()) - 1)]; }
        qint64 LineEnd(int Line) const override { const int L = qBound(0, Line, int(lines_.size()) - 1); return starts_[L] + lines_[L].size(); }
        int LineFromPosition(qint64 Position) const override
        {
            const auto It = std::upper_bound(starts_.cbegin(), starts_.cend(), Position);
            return qBound(0, int(It - starts_.cbegin()) - 1, int(lines_.size()) - 1);
        }
        int SubRowOf(qint64) const override { return 0; }
        QByteArray Text(qint64 Start, qint64 End) const override { ++textCalls_; return text_.mid(Start, qMax<qint64>(0, End - Start)); }
        void StyledText(qint64 Start, qint64 End, QByteArray& Text, QByteArray& Styles) const override
        {
            Text = text_.mid(Start, qMax<qint64>(0, End - Start));
            Styles = QByteArray(Text.size(), '\0');
        }
        qint64 EndStyled() const override { return endStyled_ < 0 ? text_.size() : endStyled_; }
        int TabWidth() const override { return 4; }
        int Marks(int Line) const override { return marks_.value(Line); }
        int FoldLevel(int Line) const override { return folds_.value(Line, SC_FOLDLEVELBASE); }
        qint64 Find(const QByteArray& Utf8, int, qint64 Start, qint64 End, qint64& MatchEnd) const override
        {
            ++findCalls_;
            if (Utf8 == "^") { // zero-length match at the next line start
                for (int Line = 0; Line < lines_.size(); ++Line)
                    if (starts_[Line] >= Start && starts_[Line] < End) { MatchEnd = starts_[Line]; return starts_[Line]; }
                return -1;
            }
            const auto Found = text_.indexOf(Utf8, Start);
            if (Found < 0 || Found + Utf8.size() > End) return -1;
            MatchEnd = Found + Utf8.size();
            return Found;
        }
        qint64 PositionAfter(qint64 Position) const override { return qMin<qint64>(text_.size(), Position + 1); }
        qint64 CaretPosition() const override { return 0; }
        void Selection(qint64& Start, qint64& End) const override { Start = End = 0; }
        bool HistoryQueryable() const override { return true; }

        int first_ = 0;
        int onScreen_ = 30;
        qint64 endStyled_ = -1;

    private:
        void rebuild()
        {
            text_.clear(); starts_.clear(); rowStarts_ = {0};
            for (int Line = 0; Line < lines_.size(); ++Line) {
                starts_.append(text_.size());
                text_ += lines_[Line];
                if (Line + 1 < lines_.size()) text_ += '\n';
                rowStarts_.append(rowStarts_.back() + rows_[Line]);
            }
        }
        QList<QByteArray> lines_;
        QVector<int> rows_;
        QVector<qint64> starts_;
        QVector<int> rowStarts_;
        QByteArray text_;
        QHash<int, int> marks_, folds_;
    };

    QList<QByteArray> numbered(int Count, const QByteArray& Prefix = "line ")
    {
        QList<QByteArray> Lines;
        for (int Index = 0; Index < Count; ++Index)
            Lines.append(Prefix + QByteArray::number(Index));
        return Lines;
    }

    QElapsedTimer started()
    {
        QElapsedTimer Clock;
        Clock.start();
        return Clock;
    }
}

class TextMinimapTests final : public QObject
{
    Q_OBJECT

private slots:
    void strokeRules()
    {
        QCOMPARE(TextMinimapStrokeOf("", 4), TextMinimapStroke{});
        QCOMPARE(TextMinimapStrokeOf("    ", 4), TextMinimapStroke{});
        QCOMPARE(TextMinimapStrokeOf("    abc", 4), (TextMinimapStroke{4, 7}));
        QCOMPARE(TextMinimapStrokeOf("\tab", 4), (TextMinimapStroke{4, 6}));
        QCOMPARE(TextMinimapStrokeOf(QByteArray(" \tx"), 8), (TextMinimapStroke{8, 9}));
        // Hangul counts two columns.
        QCOMPARE(TextMinimapStrokeOf(QStringLiteral("한글 a").toUtf8(), 4), (TextMinimapStroke{0, 6}));
        // Capped at 128 columns.
        QCOMPARE(int(TextMinimapStrokeOf(QByteArray(300, 'x'), 4).end_), TextMinimapMaxColumns);
        // Only the first 512 bytes are read: text after 600 spaces is not seen.
        QVERIFY(TextMinimapStrokeOf(QByteArray(600, ' ') + "tail", 4).Empty());
    }

    void changeStates()
    {
        QCOMPARE(TextMinimapChangeOf(0), TextMinimapChange::None);
        QCOMPARE(TextMinimapChangeOf(1 << SC_MARKNUM_HISTORY_MODIFIED), TextMinimapChange::Modified);
        QCOMPARE(TextMinimapChangeOf(1 << SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED), TextMinimapChange::Modified);
        QCOMPARE(TextMinimapChangeOf(1 << SC_MARKNUM_HISTORY_SAVED), TextMinimapChange::Saved);
        QCOMPARE(TextMinimapChangeOf((1 << SC_MARKNUM_HISTORY_SAVED) | (1 << SC_MARKNUM_HISTORY_MODIFIED)), TextMinimapChange::Modified);
        QCOMPARE(TextMinimapChangeOf(1 << SC_MARKNUM_HISTORY_REVERTED_TO_ORIGIN), TextMinimapChange::None);
        // Other markers (EPUB problems) do not count.
        QCOMPARE(TextMinimapChangeOf((1 << 10) | (1 << 11)), TextMinimapChange::None);
    }

    void lineSet()
    {
        TextMinimapLineSet Set;
        Set.Add(10, 12);
        Set.Add(14, 15);
        Set.Add(13, 13); // bridges both
        QCOMPARE(Set.Ranges(), (QVector<QPair<int, int>>{{10, 15}}));
        Set.Add(30, 30);
        Set.Shift(20, 5); // lines inserted below 20
        QCOMPARE(Set.Ranges(), (QVector<QPair<int, int>>{{10, 15}, {35, 35}}));
        Set.Shift(12, 2); // inside the first range: it grows
        QCOMPARE(Set.Ranges(), (QVector<QPair<int, int>>{{10, 17}, {37, 37}}));
        Set.Shift(5, -10); // lines 6..15 removed and merged into line 5
        QCOMPARE(Set.Ranges(), (QVector<QPair<int, int>>{{5, 7}, {27, 27}}));
        QCOMPARE(Set.LineCount(), 4);
    }

    void modeThresholds()
    {
        TextMinimapSettings Settings;
        const auto Mode = [&](int Lines, qint64 Bytes, bool Limited, int Width, bool WasNarrow) {
            return TextMinimapResolveMode(Settings, Lines, Bytes, Limited, Width, WasNarrow);
        };
        using C = TextMinimapMode::Column;
        QCOMPARE(Mode(19999, 1000, false, 800, false).column_, C::Document);
        QCOMPARE(Mode(20000, 1000, false, 800, false).column_, C::Outline);
        QCOMPARE(Mode(20001, 1000, false, 800, false).column_, C::Outline);
        QCOMPARE(Mode(10, Settings.largeBytes_ - 1, false, 800, false).column_, C::Document);
        QCOMPARE(Mode(10, Settings.largeBytes_, false, 800, false).column_, C::Outline);
        QCOMPARE(Mode(10, 1000, true, 800, false).column_, C::Outline);
        QVERIFY(Mode(10, 1000, false, 800, false).overview_);
        // Narrow: < 480 hides the column; coming back needs 480 + 24.
        QCOMPARE(Mode(10, 1000, false, 480, false).column_, C::Document);
        QCOMPARE(Mode(10, 1000, false, 479, false).column_, C::Hidden);
        QVERIFY(Mode(10, 1000, false, 479, false).overview_);
        QCOMPARE(Mode(10, 1000, false, 503, true).column_, C::Hidden);
        QCOMPARE(Mode(10, 1000, false, 504, true).column_, C::Document);
        Settings.kind_ = TextMinimapSettings::Kind::Overview;
        QCOMPARE(Mode(10, 1000, false, 800, false).column_, C::Hidden);
        QVERIFY(Mode(10, 1000, false, 800, false).overview_);
        Settings.kind_ = TextMinimapSettings::Kind::Document;
        QCOMPARE(Mode(50000, 1000, false, 800, false).column_, C::Document);
        QVERIFY(!Mode(50000, 1000, false, 800, false).overview_);
        QCOMPARE(Mode(10, Settings.largeBytes_, false, 800, false).column_, C::Outline);
        Settings.enabled_ = false;
        QCOMPARE(Mode(10, 1000, false, 800, false).column_, C::Hidden);
        QVERIFY(!Mode(10, 1000, false, 800, false).overview_);
    }

    void geometry()
    {
        // 100 rows × 2 px fit into 400 px: no scrolling.
        auto Fit = TextMinimapDocumentGeometryFor(100, 10, 20, 400, 2);
        QVERIFY(!Fit.scrolls_);
        QCOMPARE(Fit.sliderTop_, 20.0);
        QCOMPARE(Fit.RowAt(Fit.YOfRow(42)), 42.0);
        // 1000 rows × 2 px in 400 px: the thumbnail scrolls; slider top = ratio × (height − slider height).
        auto Long = TextMinimapDocumentGeometryFor(1000, 490, 20, 400, 2);
        QVERIFY(Long.scrolls_);
        QCOMPARE(Long.sliderHeight_, 40.0);
        QVERIFY(qAbs(Long.sliderTop_ - 0.5 * (400 - 40)) < 0.001);
        auto End = TextMinimapDocumentGeometryFor(1000, 980, 20, 400, 2);
        QVERIFY(qAbs(End.sliderTop_ + End.sliderHeight_ - 400) < 0.001);
        // Outline fit caps short documents at 3 px per row.
        auto Short = TextMinimapFitGeometryFor(10, 0, 30, 600, 3, 3.0, 6);
        QCOMPARE(Short.perRow_, 3.0);
        auto Tall = TextMinimapFitGeometryFor(100000, 0, 30, 606, 3, 3.0, 6);
        QVERIFY(Tall.perRow_ < 0.01);
        QCOMPARE(Tall.sliderHeight_, 6.0);
    }

    void outlineBins()
    {
        auto Lines = numbered(1000);
        Lines[500] = "        indented";
        FakeSource Source(Lines);
        TextMinimapOutline Outline;
        Outline.Reset(100, Source.DisplayRows());
        auto Clock = started();
        QVERIFY(Outline.Step(Source, Clock, 1000));
        QVERIFY(Outline.Complete());
        QCOMPARE(Outline.BinCount(), 100);
        QVERIFY(std::all_of(Outline.Bins().cbegin(), Outline.Bins().cend(), [](const auto& Bin) { return Bin.done_ && !Bin.stroke_.Empty(); }));
        // Bin 50 covers lines 500..509 and keeps the smallest indentation of its samples.
        QCOMPARE(int(Outline.Bins()[50].stroke_.indent_), 0);
        // Invalidating rows rebuilds only the bins covering them.
        Source.textCalls_ = 0;
        Outline.Invalidate(500, 509);
        QVERIFY(!Outline.Complete());
        Clock = started();
        QVERIFY(Outline.Step(Source, Clock, 1000));
        QVERIFY(Source.textCalls_ > 0 && Source.textCalls_ <= 10);
        // Sampling caps the reads per bin.
        FakeSource Big(numbered(100000));
        Outline.Reset(100, Big.DisplayRows());
        Big.textCalls_ = 0;
        Clock = started();
        QVERIFY(Outline.Step(Big, Clock, 10000));
        QVERIFY(Big.textCalls_ <= 100 * (TextMinimapOutline::SamplesPerBin + 1));
        // A zero budget makes no progress beyond the first check.
        Outline.Reset(100, Big.DisplayRows());
        QElapsedTimer Expired;
        Expired.start();
        QVERIFY(!Outline.Step(Big, Expired, -1));
    }

    void displayRows()
    {
        FakeSource Source(numbered(10));
        Source.SetRows(3, 4); // wrapped line
        Source.SetRows(9, 0); // last line folded away
        QCOMPARE(Source.DisplayRows(), 9 + 3);
        QCOMPARE(Source.RowOfLine(4), 7);
        QCOMPARE(Source.LineOfRow(5), 3);
        QCOMPARE(Source.RowOfLine(Source.LineCount()), Source.DisplayRows());
    }

    void searchCollector()
    {
        FakeSource Source({"abab ab", "nothing", "ab"});
        TextMinimapSearch Search;
        TextMinimapSearch::Key Key;
        Key.text_ = "ab";
        Search.Start(Key, Source.Length(), Source.LineCount());
        auto Clock = started();
        QVERIFY(Search.Step(Source, Clock, 1000));
        QCOMPARE(Search.Count(), 4);
        QCOMPARE(Search.Hits().size(), 4);
        QCOMPARE(Search.Hits().last().line_, 2);
        QVERIFY(!Search.Truncated());
        // Zero-length matches are skipped and the search still ends.
        Key.text_ = "^";
        Search.Start(Key, Source.Length(), Source.LineCount());
        Clock = started();
        QVERIFY(Search.Step(Source, Clock, 1000));
        QCOMPARE(Search.Count(), 0);
        // A selection range limits the search.
        Key.text_ = "ab";
        Key.start_ = 3;
        Key.end_ = 8;
        Search.Start(Key, Source.Length(), Source.LineCount());
        Clock = started();
        QVERIFY(Search.Step(Source, Clock, 1000));
        QCOMPARE(Search.Count(), 1);
    }

    void searchLimit()
    {
        QList<QByteArray> Lines;
        for (int Index = 0; Index < 6000; ++Index)
            Lines.append("x x");
        FakeSource Source(Lines);
        TextMinimapSearch Search;
        TextMinimapSearch::Key Key;
        Key.text_ = "x";
        Search.Start(Key, Source.Length(), Source.LineCount());
        auto Clock = started();
        QVERIFY(Search.Step(Source, Clock, 100000));
        QCOMPARE(Search.Count(), 12000);
        QCOMPARE(Search.Hits().size(), TextMinimapSearch::StoredLimit);
        QVERIFY(Search.Truncated());
        const auto& Density = Search.Density();
        QCOMPARE(std::accumulate(Density.cbegin(), Density.cend(), 0), 12000);
        // Sliced: an exhausted budget stops and the next slice continues.
        Search.Start(Key, Source.Length(), Source.LineCount());
        QElapsedTimer Expired;
        Expired.start();
        QVERIFY(!Search.Step(Source, Expired, -1));
        Clock = started();
        QVERIFY(Search.Step(Source, Clock, 100000));
        QCOMPARE(Search.Count(), 12000);
    }

    void problemShift()
    {
        QVector<TextMinimapProblem> Problems{{2, true}, {10, false}, {20, true}};
        TextMinimapShiftProblems(Problems, 5, 3);
        QCOMPARE(Problems[0].line_, 2);
        QCOMPARE(Problems[1].line_, 13);
        TextMinimapShiftProblems(Problems, 8, -10); // lines 9..18 removed
        QCOMPARE(Problems[1].line_, 8);
        QCOMPARE(Problems[2].line_, 13);
    }

    void markdownHeadings()
    {
        QString Title;
        QCOMPARE(TextMinimapHeadings::MarkdownAtxLevel("## 2. 텍스트 편집 ##", &Title), 2);
        QCOMPARE(Title, QStringLiteral("2. 텍스트 편집"));
        QCOMPARE(TextMinimapHeadings::MarkdownAtxLevel("#NoSpace", &Title), 0);
        QCOMPARE(TextMinimapHeadings::MarkdownAtxLevel("    # code", &Title), 0);
        QCOMPARE(TextMinimapHeadings::MarkdownAtxLevel("####### seven", &Title), 0);
        QCOMPARE(TextMinimapHeadings::MarkdownSetextLevel("====="), 1);
        QCOMPARE(TextMinimapHeadings::MarkdownSetextLevel("---  "), 2);
        QCOMPARE(TextMinimapHeadings::MarkdownSetextLevel("-- -"), 0);
        FakeSource Source({"# Title", "", "Intro", "=====", "```", "# not a heading", "```", "## Second", "~~~", "## also code", "~~~", "### Third"});
        TextMinimapHeadings Headings;
        Headings.Start(TextMinimapHeadings::Kind::Markdown, Source.LineCount());
        auto Clock = started();
        QVERIFY(Headings.Step(Source, Clock, 1000));
        const auto& Found = Headings.Headings();
        QCOMPARE(Found.size(), 4);
        QCOMPARE(Found[0].line_, 0);
        QCOMPARE(Found[1].line_, 2);
        QCOMPARE(Found[1].level_, 1);
        QCOMPARE(Found[1].text_, QStringLiteral("Intro"));
        QCOMPARE(Found[2].text_, QStringLiteral("Second"));
        QCOMPARE(Found[3].level_, 3);
    }

    void markupAndFoldHeadings()
    {
        QString Title;
        QCOMPARE(TextMinimapHeadings::MarkupLevel(R"(    <h2 id="ch03-1">1. 새벽 <em>기차</em></h2>)", &Title), 2);
        QCOMPARE(Title, QStringLiteral("1. 새벽 기차"));
        QCOMPARE(TextMinimapHeadings::MarkupLevel("<h2>unclosed", &Title), 0);
        QCOMPARE(TextMinimapHeadings::KindForLanguage("Markdown"), TextMinimapHeadings::Kind::Markdown);
        QCOMPARE(TextMinimapHeadings::KindForLanguage("HTML"), TextMinimapHeadings::Kind::Markup);
        QCOMPARE(TextMinimapHeadings::KindForLanguage("C++"), TextMinimapHeadings::Kind::Fold);
        QCOMPARE(TextMinimapHeadings::KindForLanguage("None"), TextMinimapHeadings::Kind::None);
        FakeSource Source({"class A", "{", "    void F()", "    {", "    }", "};"});
        Source.SetFold(0, SC_FOLDLEVELBASE | SC_FOLDLEVELHEADERFLAG);
        Source.SetFold(2, (SC_FOLDLEVELBASE + 1) | SC_FOLDLEVELHEADERFLAG);
        TextMinimapHeadings Headings;
        Headings.Start(TextMinimapHeadings::Kind::Fold, Source.LineCount());
        auto Clock = started();
        QVERIFY(Headings.Step(Source, Clock, 1000));
        QCOMPARE(Headings.Headings().size(), 2);
        QCOMPARE(Headings.Headings()[1].text_, QStringLiteral("void F()"));
        QCOMPARE(Headings.Headings()[1].level_, 2);
    }

    void rendererTiles()
    {
        FakeSource Source(numbered(300, "text "));
        TextMinimapRenderer Renderer;
        TextMinimapRenderer::Options Options;
        Options.width_ = 100;
        QImage Canvas(100, 300, QImage::Format_ARGB32_Premultiplied);
        Canvas.fill(Qt::white);
        const auto Paint = [&] {
            QPainter Painter(&Canvas);
            const auto Geometry = TextMinimapDocumentGeometryFor(Source.DisplayRows(), 0, 30, 300, 2);
            Renderer.PaintDocument(Painter, Source, Geometry, 300, Options, [](int) { return QColor(Qt::black); }, QColor(Qt::gray));
        };
        Paint();
        // 150 visible rows (300 px / 2 px) need tiles 0..2.
        QCOMPARE(Renderer.CachedTiles(), 3);
        QCOMPARE(Canvas.pixelColor(5, 1), QColor(Qt::black));   // "text 0" at row 0
        Renderer.InvalidateLines(70, 70);
        QCOMPARE(Renderer.CachedTiles(), 2);
        Paint();
        QCOMPARE(Renderer.CachedTiles(), 3);
        // A wrap change in one line redraws its tile through the row count, without invalidation.
        Source.SetRows(10, 3);
        Canvas.fill(Qt::white);
        Paint();
        // "text 10" spreads over three rows (3 columns each): row 11 starts with its 4th character 't' at x 4.
        QCOMPARE(Canvas.pixelColor(4, 2 * 11 + 1), QColor(Qt::black));
        QCOMPARE(Canvas.pixelColor(5, 2 * 13 + 1), QColor(Qt::black)); // line 11 now starts at row 13
        // Lines past ENDSTYLED use the pending colour until styling reaches them.
        Source.endStyled_ = 0;
        Renderer.Clear();
        Canvas.fill(Qt::white);
        Paint();
        QCOMPARE(Canvas.pixelColor(5, 1), QColor(Qt::gray));
        Source.endStyled_ = -1;
        Canvas.fill(Qt::white);
        Paint();
        QCOMPARE(Canvas.pixelColor(5, 1), QColor(Qt::black));
        Renderer.InvalidateFrom(0);
        QCOMPARE(Renderer.CachedTiles(), 0);
    }
};

int main(int Argc, char** Argv)
{
    QApplication App(Argc, Argv);
    TextMinimapTests Tests;
    return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstTextMinimap.moc"
