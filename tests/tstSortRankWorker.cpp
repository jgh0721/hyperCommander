#include "commonLibs/columns/sortRankWorker.hpp"
#include <QtTest>
#include <random>

using namespace hc::columns;

// 작업 스레드 순위가 UI 경로(SortRanks · SortLess)와 같은 순서인지, 묶음 크기 · 도착 순서 · 세대와 무관한지 본다.
class SortRankWorkerTests : public QObject
{
    Q_OBJECT
    struct Received { quint64 generation_ = 0; int rows_ = -1; QVector<int> ranks_; int calls_ = 0; };
    static QVector<SortRow> sampleRows(int Count, quint32 Seed)
    {
        std::mt19937 Random(Seed);
        const QStringList Stems{"file", "File", "파일", "가나다", "report", "Report", "a", "B", "z", "image", "ëclair", "Éclair", "10", "9"};
        QVector<SortRow> Rows;
        for (int Index = 0; Index < Count; ++Index) {
            SortRow Row;
            Row.id_ = QStringLiteral("C:\\base\\%1\\%2").arg(Random() % 7).arg(Index);
            Row.directory_ = Random() % 9 == 0;
            Row.parent_ = Index == 0 && Seed % 2;
            const auto Name = Stems[int(Random() % Stems.size())] + QString::number(Random() % 120) + (Random() % 3 ? QStringLiteral(".txt") : QString());
            // 깊이(숫자) → 이름(문자열) → 크기(숫자, 일부 값 없음): 평면 보기의 이름 정렬과 같은 모양이다.
            Row.values_.append(CellValue::Ready(quint64(Random() % 4)));
            Row.values_.append(CellValue::Ready(Name));
            Row.values_.append(Random() % 5 ? CellValue::Ready(quint64(Random() % 1000)) : CellValue::Missing(ValueState::Empty));
            Rows.append(Row);
        }
        return Rows;
    }
    static QVector<int> reference(const QVector<SortRow>& Rows, const QVector<Qt::SortOrder>& Orders, const SortTextOptions& Options)
    {
        return SortRanks(Rows, Orders, Options.locale_, Options.numeric_, Options.case_);
    }
private slots:
    void matchesUiOrder_data()
    {
        QTest::addColumn<bool>("locale"); QTest::addColumn<bool>("numeric"); QTest::addColumn<bool>("caseSensitive");
        QTest::addColumn<int>("batch"); QTest::addColumn<bool>("descending");
        for (const int Batch : {1, 256, 5000})
            for (const bool Locale : {true, false})
                for (const bool Descending : {false, true}) {
                    QTest::addRow("batch%d-%s-%s", Batch, Locale ? "locale" : "plain", Descending ? "desc" : "asc") << Locale << true << false << Batch << Descending;
                }
        QTest::addRow("case-sensitive") << true << true << true << 256 << false;
        QTest::addRow("no-numeric") << true << false << false << 256 << false;
        QTest::addRow("plain-case-sensitive") << false << false << true << 64 << true;
    }
    void matchesUiOrder()
    {
        QFETCH(bool, locale); QFETCH(bool, numeric); QFETCH(bool, caseSensitive); QFETCH(int, batch); QFETCH(bool, descending);
        const auto Rows = sampleRows(batch == 1 ? 600 : 12000, quint32(batch + locale * 3 + descending * 5));
        const auto Order = descending ? Qt::DescendingOrder : Qt::AscendingOrder;
        const QVector<Qt::SortOrder> Orders{Order, Order, Qt::AscendingOrder};
        SortTextOptions Options{locale, numeric, caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive};
        Received Last;
        SortRankWorker Worker([&](quint64 Generation, int Count, QVector<int> Ranks) {
            Last.generation_ = Generation; Last.rows_ = Count; Last.ranks_ = std::move(Ranks); ++Last.calls_; });
        for (int First = 0; First < Rows.size(); First += batch) {
            const auto Size = std::min<qsizetype>(batch, Rows.size() - First);
            Worker.Submit(7, First == 0, Orders, Options, First, Rows.mid(First, Size));
        }
        QTRY_COMPARE_WITH_TIMEOUT(Last.rows_, int(Rows.size()), 30000);
        QCOMPARE(Last.generation_, quint64(7));
        QCOMPARE(Last.ranks_, reference(Rows, Orders, Options));
        // 묶음이 쌓이면 마지막 묶음까지 병합한 뒤 한 번만 알릴 수 있다. 알림 수는 묶음 수를 넘지 않는다.
        QVERIFY(Last.calls_ <= (Rows.size() + batch - 1) / batch);
    }
    void newGenerationDiscardsOld()
    {
        const auto Rows = sampleRows(20000, 11);
        const QVector<Qt::SortOrder> Orders{Qt::AscendingOrder, Qt::AscendingOrder, Qt::AscendingOrder};
        QList<quint64> Generations; Received Last;
        SortRankWorker Worker([&](quint64 Generation, int Count, QVector<int> Ranks) {
            Generations.append(Generation); Last.generation_ = Generation; Last.rows_ = Count; Last.ranks_ = std::move(Ranks); });
        Worker.Submit(1, true, Orders, {}, 0, Rows);
        const auto Second = Rows.mid(0, 500);
        Worker.Submit(2, true, Orders, {}, 0, Second);
        QTRY_COMPARE_WITH_TIMEOUT(Last.generation_, quint64(2), 30000);
        QTRY_VERIFY(Worker.Idle());
        QCoreApplication::processEvents();
        QVERIFY(!Generations.contains(1));
        QCOMPARE(Last.rows_, 500);
        QCOMPARE(Last.ranks_, reference(Second, Orders, {}));
    }
    void mismatchAsksForRebuild()
    {
        const auto Rows = sampleRows(300, 5);
        const QVector<Qt::SortOrder> Orders{Qt::AscendingOrder, Qt::AscendingOrder, Qt::AscendingOrder};
        Received Last;
        SortRankWorker Worker([&](quint64 Generation, int Count, QVector<int> Ranks) {
            Last.generation_ = Generation; Last.rows_ = Count; Last.ranks_ = std::move(Ranks); ++Last.calls_; });
        Worker.Submit(3, true, Orders, {}, 0, Rows.mid(0, 100));
        QTRY_COMPARE(Last.rows_, 100);
        // 앞서 보낸 행 수와 다른 First는 상태가 어긋난 것이다. 빈 순위로 전체 재전송을 요청한다.
        Worker.Submit(3, false, Orders, {}, 150, Rows.mid(150, 50));
        QTRY_COMPARE(Last.calls_, 2);
        QVERIFY(Last.ranks_.isEmpty());
        // 정렬 규칙이 바뀐 이어 붙이기도 같다.
        Worker.Submit(4, true, Orders, {}, 0, Rows.mid(0, 10));
        QTRY_COMPARE(Last.rows_, 10);
        Worker.Submit(4, false, {Qt::DescendingOrder, Qt::AscendingOrder, Qt::AscendingOrder}, {}, 10, Rows.mid(10, 10));
        QTRY_COMPARE(Last.calls_, 4);
        QVERIFY(Last.ranks_.isEmpty());
    }
    void clearStopsDelivery()
    {
        const auto Rows = sampleRows(30000, 9);
        const QVector<Qt::SortOrder> Orders{Qt::AscendingOrder, Qt::AscendingOrder, Qt::AscendingOrder};
        int Calls = 0;
        SortRankWorker Worker([&](quint64, int, QVector<int>) { ++Calls; });
        Worker.Submit(5, true, Orders, {}, 0, Rows);
        Worker.Clear(6);
        QTRY_VERIFY_WITH_TIMEOUT(Worker.Idle(), 30000);
        QTest::qWait(50);
        QCOMPARE(Calls, 0);
        // 해제 뒤 새 세대는 다시 정상으로 계산한다.
        Received Last;
        SortRankWorker Next([&](quint64 Generation, int Count, QVector<int> Ranks) { Last.generation_ = Generation; Last.rows_ = Count; Last.ranks_ = std::move(Ranks); });
        Next.Submit(1, true, Orders, {}, 0, Rows.mid(0, 50));
        Next.Clear(2);
        Next.Submit(3, true, Orders, {}, 0, Rows.mid(0, 40));
        QTRY_COMPARE(Last.rows_, 40);
        QCOMPARE(Last.generation_, quint64(3));
    }
    void treeOrder()
    {
        // 평면 트리: 부모 폴더 → 그 안의 폴더 → 파일, 같은 부모 안에서 이름 방향. 폴더 먼저 · 부모 먼저는 방향과 무관하다.
        const QList<QPair<QString, bool>> Items{{"z.txt", false}, {"a\\x.txt", false}, {"c", true}, {"a\\b\\y.txt", false},
            {"a", true}, {"c\\w.txt", false}, {"a\\b", true}, {"B.txt", false}};
        QVector<SortRow> Rows;
        for (const auto& [Path, Folder] : Items) {
            SortRow Row; Row.id_ = Path; Row.directory_ = Folder; Row.tree_ = Path.split('\\');
            Row.values_.append(CellValue::Ready(Path.section('\\', -1))); Rows.append(Row);
        }
        for (const bool Descending : {false, true}) {
            Received Last;
            SortRankWorker Worker([&](quint64 Generation, int Count, QVector<int> Ranks) { Last.generation_ = Generation; Last.rows_ = Count; Last.ranks_ = std::move(Ranks); });
            Worker.Submit(1, true, {Descending ? Qt::DescendingOrder : Qt::AscendingOrder}, {}, 0, Rows.mid(0, 4));
            Worker.Submit(1, false, {Descending ? Qt::DescendingOrder : Qt::AscendingOrder}, {}, 4, Rows.mid(4));
            QTRY_COMPARE(Last.rows_, int(Rows.size()));
            QStringList Ordered(Rows.size());
            for (int Row = 0; Row < Rows.size(); ++Row) Ordered[Last.ranks_[Row]] = Rows[Row].id_;
            const QStringList Expected = Descending ? QStringList{"c", "c\\w.txt", "a", "a\\b", "a\\b\\y.txt", "a\\x.txt", "z.txt", "B.txt"}
                                                    : QStringList{"a", "a\\b", "a\\b\\y.txt", "a\\x.txt", "c", "c\\w.txt", "B.txt", "z.txt"};
            QCOMPARE(Ordered, Expected);
        }
    }
    void pathOrder()
    {
        // 평면 경로 순: 상대 부모 폴더를 구간 단위로 비교(상위 폴더가 하위 폴더보다 먼저) → 같은 폴더 안에서 폴더 먼저 → 이름.
        // "Books2"가 "Books\Sub"보다 뒤인 것은 문자열 비교가 아니라 구간 비교이기 때문이다. 방향은 폴더 순서와 이름에 함께 적용된다.
        const QList<QPair<QString, bool>> Items{{"Code\\b.txt", false}, {"Books2\\a.txt", false}, {"z.txt", false}, {"Books\\Sub\\c.txt", false},
            {"Books", true}, {"Books\\b.epub", false}, {"Code\\A.md", false}, {"Books\\Sub", true}, {"a.txt", false}, {"Books\\a.epub", false},
            {"Code", true}, {"books\\x.txt", false}};
        QVector<SortRow> Rows;
        for (const auto& [Path, Folder] : Items) {
            SortRow Row; Row.id_ = Path; Row.directory_ = Folder; Row.folder_ = Path.contains('\\') ? Path.section('\\', 0, -2) : QString{};
            // 이름 → 크기: 경로 비교는 이름 기준(0번) 앞에 끼어든다.
            Row.values_.append(CellValue::Ready(Path.section('\\', -1))); Row.values_.append(CellValue::Ready(quint64(1)));
            Rows.append(Row);
        }
        for (const bool Locale : {true, false})
            for (const bool Descending : {false, true}) {
                const auto Order = Descending ? Qt::DescendingOrder : Qt::AscendingOrder;
                Received Last;
                SortRankWorker Worker([&](quint64 Generation, int Count, QVector<int> Ranks) { Last.generation_ = Generation; Last.rows_ = Count; Last.ranks_ = std::move(Ranks); });
                // 새 폴더가 뒤 묶음에 처음 나와도 앞 묶음의 순서와 이어져야 한다.
                const SortTextOptions Options{Locale, true, Qt::CaseInsensitive};
                Worker.Submit(1, true, {Order, Qt::AscendingOrder}, Options, 0, Rows.mid(0, 3), 0);
                Worker.Submit(1, false, {Order, Qt::AscendingOrder}, Options, 3, Rows.mid(3, 5), 0);
                Worker.Submit(1, false, {Order, Qt::AscendingOrder}, Options, 8, Rows.mid(8), 0);
                QTRY_COMPARE(Last.rows_, int(Rows.size()));
                QStringList Ordered(Rows.size());
                for (int Row = 0; Row < Rows.size(); ++Row) Ordered[Last.ranks_[Row]] = Rows[Row].id_;
                // 대소문자만 다른 "books"는 "Books" 바로 뒤의 별개 폴더다(대소문자 구분 파일 시스템).
                const QStringList Ascending{"Books", "Code", "a.txt", "z.txt", "Books\\Sub", "Books\\a.epub", "Books\\b.epub", "books\\x.txt",
                    "Books\\Sub\\c.txt", "Books2\\a.txt", "Code\\A.md", "Code\\b.txt"};
                const QStringList Expected = Descending ? QStringList{"Code\\b.txt", "Code\\A.md", "Books2\\a.txt", "Books\\Sub\\c.txt", "books\\x.txt",
                    "Books\\Sub", "Books\\b.epub", "Books\\a.epub", "Code", "Books", "z.txt", "a.txt"} : Ascending;
                QCOMPARE(Ordered, Expected);
            }
        // 경로 순을 끄면(PathIndex -1) 폴더가 섞여 이름순이다.
        Received Last;
        SortRankWorker Worker([&](quint64 Generation, int Count, QVector<int> Ranks) { Last.generation_ = Generation; Last.rows_ = Count; Last.ranks_ = std::move(Ranks); });
        Worker.Submit(2, true, {Qt::AscendingOrder, Qt::AscendingOrder}, {}, 0, Rows);
        QTRY_COMPARE(Last.rows_, int(Rows.size()));
        QCOMPARE(Last.ranks_, reference(Rows, {Qt::AscendingOrder, Qt::AscendingOrder}, {}));
    }
    void destroyedWorkerIsSilent()
    {
        const auto Rows = sampleRows(40000, 3);
        int Calls = 0;
        {
            SortRankWorker Worker([&](quint64, int, QVector<int>) { ++Calls; });
            Worker.Submit(1, true, {Qt::AscendingOrder, Qt::AscendingOrder, Qt::AscendingOrder}, {}, 0, Rows);
        }
        QTest::qWait(300);
        QCOMPARE(Calls, 0);
    }
};

QTEST_GUILESS_MAIN(SortRankWorkerTests)
#include "tstSortRankWorker.moc"
