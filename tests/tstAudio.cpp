#include "UniqueLibs/audio/audioQueue.hpp"
#include <QtTest>
#include <set>

using namespace hc::audio;

namespace {
std::vector<AudioTrack> Tracks(int Count)
{
    std::vector<AudioTrack> Result;
    for (int Index = 0; Index < Count; ++Index) { AudioTrack Track; Track.name_ = QString("%1.flac").arg(Index); Result.push_back(Track); }
    return Result;
}
}

// 오디오 미리 듣기의 순수 로직(계획 T01–T04).
class AudioTests final : public QObject
{
    Q_OBJECT
private slots:
    void extensions()
    {
        QCOMPARE(ParseExtensions(" .MP3, flac;*.Opus  wav,,mp3 "), (QStringList{"mp3", "flac", "opus", "wav"}));
        const auto Defaults = DefaultExtensions();
        QVERIFY(Defaults.contains("flac") && Defaults.contains("dsf") && !Defaults.contains("mid") && !Defaults.contains("xm"));
        QVERIFY(IsPlayable("a.b.FLAC", Defaults));
        QVERIFY(!IsPlayable("flac", Defaults));
        QVERIFY(!IsPlayable(".flac", Defaults));
        QVERIFY(!IsPlayable("song.", Defaults));
        QVERIFY(!IsPlayable("song.mid", Defaults));
    }
    void durations()
    {
        QCOMPARE(FormatDuration(0), QString("0:00"));
        QCOMPARE(FormatDuration(59999), QString("0:59"));
        QCOMPARE(FormatDuration(296000), QString("4:56"));
        QCOMPARE(FormatDuration(3600000), QString("1:00:00"));
        QCOMPARE(FormatDuration(3725000), QString("1:02:05"));
        QCOMPARE(FormatDuration(-1), QString("-:--"));
    }
    void nextAndPrevious()
    {
        AudioQueue Queue; Queue.Reset(Tracks(3), 1);
        QCOMPARE(Queue.CurrentIndex(), qsizetype(1));
        QCOMPARE(Queue.Next(), std::optional<qsizetype>(2));
        QCOMPARE(Queue.Previous(1000), std::optional<qsizetype>(0));
        // 3초가 지나면 같은 곡을 처음부터.
        QCOMPARE(Queue.Previous(3500), std::optional<qsizetype>(1));
        Queue.Select(2);
        QCOMPARE(Queue.Next(), std::nullopt);
        QCOMPARE(Queue.NextAfterEnd(), std::nullopt);
        Queue.SetRepeat(RepeatMode::Folder);
        QCOMPARE(Queue.Next(), std::optional<qsizetype>(0));
        Queue.SetRepeat(RepeatMode::One);
        QCOMPARE(Queue.NextAfterEnd(), std::optional<qsizetype>(2));
        QCOMPARE(Queue.Next(), std::nullopt);
        Queue.Select(0); Queue.SetRepeat(RepeatMode::Off);
        QCOMPARE(Queue.Previous(0), std::optional<qsizetype>(0));
        Queue.SetRepeat(RepeatMode::Folder);
        QCOMPARE(Queue.Previous(0), std::optional<qsizetype>(2));
        QCOMPARE(RepeatModeFromName(RepeatModeName(RepeatMode::One)), RepeatMode::One);
        QCOMPARE(RepeatModeFromName("bogus"), RepeatMode::Off);
    }
    void shuffle()
    {
        AudioQueue Queue; Queue.Reset(Tracks(12), 5);
        Queue.SetShuffle(true, 42);
        const auto Order = Queue.Order();
        QCOMPARE(Order.size(), size_t(12));
        QCOMPARE(Order.front(), qsizetype(5));
        QCOMPARE(std::set<qsizetype>(Order.begin(), Order.end()).size(), size_t(12));
        AudioQueue Same; Same.Reset(Tracks(12), 5); Same.SetShuffle(true, 42);
        QCOMPARE(Same.Order(), Order);
        // 다음 곡은 섞인 순서를 따른다.
        QCOMPARE(Queue.Next(), std::optional<qsizetype>(Order[1]));
        Queue.SetShuffle(false, 0);
        QCOMPARE(Queue.CurrentIndex(), qsizetype(5));
        QCOMPARE(Queue.Next(), std::optional<qsizetype>(6));
    }
    void emptyQueue()
    {
        AudioQueue Queue; Queue.Reset({}, 0);
        QVERIFY(Queue.IsEmpty()); QVERIFY(!Queue.Current());
        QCOMPARE(Queue.Next(), std::nullopt); QCOMPARE(Queue.Previous(0), std::nullopt);
        Queue.Reset(Tracks(2), 9);
        QCOMPARE(Queue.CurrentIndex(), qsizetype(1));
    }
};

QTEST_GUILESS_MAIN(AudioTests)
#include "tstAudio.moc"
