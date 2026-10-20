#include "qtitanRegression.hpp"
#include <QPainter>
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
