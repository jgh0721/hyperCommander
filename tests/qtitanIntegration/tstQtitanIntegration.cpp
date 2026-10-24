#include "qtitanRegression.hpp"
#include <QPainter>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QStyleFactory>
#include <QtnCommonStyle.h>
#include <QtnStyleOption.h>
#include "qtitanGroupingRegression.hpp"

class RecordPainter final : public Qtitan::GridRecordPainter
{
public:
    void paintBackground(QPainter& Painter, const Record& RecordValue, QWidget*) override
    {
        ++backgrounds_;
        Painter.fillRect(RecordValue.rect, QColor(17, 193, 97));
    }
    void paintOverlay(QPainter& Painter, const Record& RecordValue, QWidget*) override
    {
        ++overlays_;
        Painter.fillRect(QRect(RecordValue.rect.topLeft(), QSize(4, 4)), QColor(231, 23, 151));
    }
    int backgrounds_ = 0;
    int overlays_ = 0;
};

class QtitanIntegration final : public QObject
{
    Q_OBJECT
private slots:
    void externalFocusDuringCursorMove()
    {
        hc::tests::GridCountingModel Model(24);
        QWidget Window;
        QVBoxLayout Layout(&Window);
        QLineEdit Search;
        Qtitan::Grid Grid;
        Layout.addWidget(&Search);
        Layout.addWidget(&Grid);
        auto* View = hc::tests::SetupGrid(Grid, Model);
        View->setFileManagerMode(true);
        View->setFocusedRowIndex(1);
        connect(&Search, &QLineEdit::textChanged, &Window, [View] {
            View->setFocusedRowIndex(7);
        });
        Window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Window));
        Search.setFocus();
        QTRY_VERIFY(Search.hasFocus());
        QTest::keyClicks(&Search, "query");
        QCOMPARE(Search.text(), QString("query"));
        QVERIFY(Search.hasFocus());
        QCOMPARE(View->focusedRowIndex(), 7);
        Model.Touch(0, 23);
        QCoreApplication::processEvents();
        QVERIFY(Search.hasFocus());
        QLineEdit Rename(Grid.viewport());
        Rename.setGeometry(30, 90, 160, 24);
        Rename.show();
        connect(&Rename, &QLineEdit::textChanged, &Window, [View] {
            View->setFocusedRowIndex(12);
        });
        Rename.setFocus();
        QTRY_VERIFY(Rename.hasFocus());
        QTest::keyClicks(&Rename, "edit");
        QCOMPARE(Rename.text(), QString("edit"));
        QVERIFY(Rename.hasFocus());
        Grid.viewport()->setFocus();
        QTest::keyClick(Grid.viewport(), Qt::Key_Down);
        QCOMPARE(View->focusedRowIndex(), 13);
    }
    void compactStyledHeaders()
    {
        class HeaderStyle final : public Qtitan::CommonStyle
        {
        public:
            HeaderStyle() : Qtitan::CommonStyle(QStyleFactory::create("Fusion")) {}
            void drawPrimitive(QStyle::PrimitiveElement Element, const QStyleOption* Option,
                QPainter* Painter, const QWidget* Widget = nullptr) const override
            {
                if (Element == QStyle::PE_IndicatorHeaderArrow)
                {
                    ++arrows_;
                    direction_ = qstyleoption_cast<const QStyleOptionHeader*>(Option)->sortIndicator;
                    arrowRect_ = Option->rect;
                }
                Qtitan::CommonStyle::drawPrimitive(Element, Option, Painter, Widget);
            }
            mutable int arrows_ = 0;
            mutable QStyleOptionHeader::SortIndicator direction_ = QStyleOptionHeader::None;
            mutable QRect arrowRect_;
            mutable QRect requestedArrowRect_;
            QRect subElementRect(QStyle::SubElement Element, const QStyleOption* Option,
                const QWidget* Widget = nullptr) const override
            {
                const QRect Rect = Qtitan::CommonStyle::subElementRect(Element, Option, Widget);
                if (Element == QStyle::SE_HeaderArrow)
                    requestedArrowRect_ = Rect;
                return Rect;
            }
        } Style;
        QWidget Widget;
        Widget.setStyle(&Style);
        QFont Font = Widget.font();
        Font.setPixelSize(14);
        Qtitan::GridHeaderStyleOption Option;
        Option.initFrom(&Widget);
        Option.font = Font;
        Option.fontMetrics = QFontMetrics(Font);
        Option.rect = QRect(0, 0, 240, 22);
        Option.text = "Header text";
        Option.textAlignment = Qt::AlignLeft | Qt::AlignVCenter;
        Option.sortIndicatorStyled = true;
        const auto TextRect = [&] {
            return Style.subControlRect(QStyle::ComplexControl(Qtitan::CommonStyle::CC_GridHeaderSection),
                &Option, QStyle::SubControl(Qtitan::CommonStyle::SC_GridHeaderText), &Widget);
        };
        const QRect Plain = TextRect();
        QVERIFY(Plain.height() >= Option.fontMetrics.height());
        Option.captionIndent = 8;
        QCOMPARE(TextRect().left(), Plain.left() + 8);
        QImage Image(Option.rect.size(), QImage::Format_ARGB32_Premultiplied);
        Image.fill(Qt::transparent);
        QPainter Painter(&Image);
        for (const auto LayoutDirection : {Qt::LeftToRight, Qt::RightToLeft})
        for (const auto Direction : {QStyleOptionHeader::SortUp, QStyleOptionHeader::SortDown})
        {
            Option.direction = LayoutDirection;
            Option.sortIndicator = Direction;
            const int Before = Style.arrows_;
            Style.drawComplexControl(QStyle::ComplexControl(Qtitan::CommonStyle::CC_GridHeaderSection),
                &Option, &Painter, &Widget);
            QCOMPARE(Style.arrows_, Before + 1);
            QCOMPARE(Style.direction_, Direction == QStyleOptionHeader::SortUp ?
                QStyleOptionHeader::SortDown : QStyleOptionHeader::SortUp);
            QCOMPARE(Style.arrowRect_, Style.requestedArrowRect_);
            QVERIFY(TextRect().width() < Plain.width() - 8);
        }
    }
    void independentGrouping() { hc::tests::GridIndependentGroupingRegression(); }
    void fileManager() { hc::tests::GridFileManagerRegression(); }
    void cacheAndSort() { hc::tests::GridCacheAndSortRegression(); }
    void recordPainterWithFileManager()
    {
        hc::tests::GridCountingModel Model(10);
        RecordPainter Painter;
        Qtitan::Grid Grid;
        auto* View = hc::tests::SetupGrid(Grid, Model);
        View->setFileManagerMode(true);
        View->setFocusedRowIndex(2);
        View->selectRow(2);
        View->setCutRows({2});
        View->setRecordPainter(&Painter);
        Grid.show();
        QVERIFY(QTest::qWaitForWindowExposed(&Grid));
        const QImage Image = Grid.viewport()->grab().toImage();
        QVERIFY(Painter.backgrounds_ > 0);
        QCOMPARE(Painter.backgrounds_, Painter.overlays_);
        int BackgroundPixels = 0;
        int OverlayPixels = 0;
        for (int Y = 0; Y < Image.height(); ++Y)
            for (int X = 0; X < Image.width(); ++X)
            {
                const QColor Color = Image.pixelColor(X, Y);
                BackgroundPixels += Color == QColor(17, 193, 97);
                OverlayPixels += Color == QColor(231, 23, 151);
            }
        QVERIFY(BackgroundPixels > 1000);
        QVERIFY(OverlayPixels > 0);
        // The selected cells must not repaint the HC cursor/mark background over Q3.
        int SelectedCells = 0;
        const qreal Dpr = Image.devicePixelRatio();
        for (const auto& Hit : Grid.hitInfoAll())
        {
            if (Hit.info() != Qtitan::GridHitInfo::Cell || Hit.row().rowIndex() != 2)
                continue;
            const QRect Interior = Hit.rect().adjusted(2, 2, -2, -2);
            int CellPixels = 0;
            int RecordPixels = 0;
            for (int Y = qRound(Interior.top() * Dpr); Y < qRound(Interior.bottom() * Dpr); ++Y)
                for (int X = qRound(Interior.left() * Dpr); X < qRound(Interior.right() * Dpr); ++X)
                {
                    ++CellPixels;
                    RecordPixels += Image.pixelColor(X, Y) == QColor(17, 193, 97);
                }
            // Text/icon antialiasing can touch any single sample; inspect the cell area.
            QVERIFY(CellPixels > 0);
            QVERIFY(RecordPixels > CellPixels / 2);
            ++SelectedCells;
        }
        QCOMPARE(SelectedCells, 3);

        QVERIFY(View->isRowCut(2));
        QVERIFY(View->selection()->selectedRowIndexes().contains(Model.index(2, 0)));
        View->setRecordPainter(nullptr);
    }
};

QTEST_MAIN(QtitanIntegration)
#include "tstQtitanIntegration.moc"
