#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDateTime>
#include <QFutureWatcher>
#include <QHash>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

namespace hc::document
{
    /// 최근 항목 한 개. 같은 파일은 Key() 로 묶는다.
    struct RecentDocument
    {
        QString   path_, viewId_, kind_;         // kind_: text|markdown|pdf|image|comic|webtoon|epub|hwp
        QDateTime openedAt_;
        bool      pinned_ = false;
        QDateTime pinnedAt_;                     // 고정 상태를 마지막으로 바꾼 시각(해제 포함), 프로세스 간 병합 기준
        QString   encoding_;                     // '다시 읽기'로 지정한 인코딩일 때만 채운다
        QString   markdownView_;                 // preview|source|split
        int       pdfPage_ = 0;                  // 1부터
        QString   epubMode_;                     // read|edit
        QString   imageMode_;                    // single|comic|webtoon
        int       readingPage_ = 0, readingPageCount_ = 0;

        QString Key() const;
        bool operator==( const RecentDocument& ) const = default;
    };

    /// 병합 키. 정규 경로 조회( canonicalFilePath )는 SMB 에서 막힐 수 있어 쓰지 않는다.
    QString RecentKey( const QString& Path );

    /// 기록 제외 판정 입력(계획 6.3).
    struct RecentRecordContext
    {
        bool enabled_ = true, temporarySource_ = false, originalAuthority_ = true, recovered_ = false,
             untitled_ = false, failed_ = false, privateReading_ = false, brokerCopy_ = false;
    };
    bool ShouldRecordRecent( const RecentRecordContext& Context );

    /// 둘째 줄 설명(계획 6.1 표). 예: "텍스트 · …\샘플", "PDF · 3쪽".
    QString RecentDescription( const RecentDocument& Document );
    /// "오늘 14:20" · "어제" · "11월 2일" · "2025년 11월 2일".
    QString RecentDateText( const QDateTime& When, const QDateTime& Now );

    /// recent-documents.json 저장소. 변경은 메모리에 바로 반영하고, 디스크에는 작업 단위로
    /// 한 작업자 스레드가 잠금 아래 다시 읽어 적용한다. 그래서 두 프로세스가 서로의 변경을 잃지 않는다.
    class RecentDocumentStore final : public QObject
    {
        Q_OBJECT
    public:
        static constexpr int KeepUnpinned = 30, KeepPinned = 10;

        explicit RecentDocumentStore( QString Directory, QObject* Parent = nullptr );
        ~RecentDocumentStore() override;

        QString FileName() const;
        /// 비동기로 읽고 끝나면 Changed 를 낸다.
        void Load();
        /// 호출한 스레드에서 바로 읽는다(시작 직후 · 테스트). 파일이 없어도 true.
        bool LoadSync();
        /// 고정 항목 먼저, 그다음 openedAt 내림차순.
        QList< RecentDocument > Entries() const { return entries_; }
        void Record( RecentDocument Document );
        void Pin( const QString& Key, bool Pinned );
        void Remove( const QString& Key );
        void Clear( bool IncludePinned );
        /// 파일 수정 시각 · 크기가 마지막으로 읽거나 쓴 때와 다를 때만 다시 읽는다(비동기).
        void ReloadIfChanged();
        /// 대기 중인 작업이 모두 끝날 때까지 이벤트를 처리하며 기다린다(테스트).
        bool WaitForIdle( int Ms );

    signals:
        void Changed();

    private:
        struct Operation
        {
            enum class Kind { Record, Pin, Remove, Clear };
            Kind           kind_ = Kind::Record;
            RecentDocument document_;
            QString        key_;
            bool           flag_ = false;
            QDateTime      at_;
        };
        struct Stamp
        {
            bool      exists_ = false;
            qint64    size_   = -1;
            QDateTime modified_;
            bool operator==( const Stamp& ) const = default;
        };
        struct Outcome
        {
            // served_: 쓰기를 시작할 때 걸려 있던 다시 읽기 요청을 이 쓰기가 대신했는가
            // full_: Load() 로 요청한 읽기(목록이 같아도 Changed 를 낸다)
            bool                    write_ = false, ok_ = false, changed_ = true, served_ = false, full_ = false;
            QList< RecentDocument > entries_;
            Stamp                   stamp_;
        };

        static Outcome run( const QString& File, const QList< Operation >& Operations, bool Write, bool OnlyIfChanged, const Stamp& Known );
        static void    apply( QList< RecentDocument >& Entries, const Operation& Op );
        static void    normalize( QList< RecentDocument >& Entries );

        void mutate( Operation Op );
        void schedule();
        void start( bool Write );
        void finish( const Outcome& Result );
        void adopt( const QList< RecentDocument >& Disk, bool Announce = false );
        void drain();
        bool idle() const;

        QString                 directory_;
        QList< RecentDocument > entries_;
        QList< Operation >      pending_, inFlight_;
        Stamp                   stamp_;
        QTimer                  retry_;
        int                     failures_ = 0;
        bool                    busy_ = false, parked_ = false, readPending_ = false, checkPending_ = false, closing_ = false;
        QThreadPool             pool_;
    };

    /// 없는 파일을 흐리게 보이기 위한 백그라운드 확인(계획 6.5). 고정 · 이동식 드라이브만 확인하고,
    /// UNC · 원격 드라이브와 예산(항목당 200 ms)을 넘긴 드라이브의 나머지 항목은 '확인 안 함'으로 둔다.
    class RecentPathProbe final : public QObject
    {
        Q_OBJECT
    public:
        enum class State { Unchecked, Present, Missing };
        static constexpr int BudgetMs = 200;

        explicit RecentPathProbe( QObject* Parent = nullptr );
        ~RecentPathProbe() override;

        /// 아직 확인하지 않은 경로만 확인한다.
        void Check( const QStringList& Paths );
        /// 진행 중인 확인을 멈춘다. 이미 얻은 결과는 유지한다.
        void Cancel();
        /// 결과를 모두 잊는다(다시 보일 때 새로 확인).
        void Reset();
        State StateOf( const QString& Path ) const;
        bool IsRunning() const;

    signals:
        void Checked( const QString& Path );

    private:
        using Result = QPair< QString, int >;
        void collect( int Index );
        void launch();

        QHash< QString, State >    states_;       // RecentKey → 상태. 확인하지 않기로 한 경로도 Unchecked 로 넣는다
        QStringList                wanted_;
        QFutureWatcher< Result >*  watcher_ = nullptr;
    };
}

Q_DECLARE_METATYPE( hc::document::RecentDocument )
