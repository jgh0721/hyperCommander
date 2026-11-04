#include "stdafx.h"
#include "recentDocuments.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "UniqueLibs/fileBatchOperations.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QLoggingCategory>
#include <QPromise>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QTimeZone>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        // 경로는 개인정보이므로 로그에는 코드만 남긴다(계획 6.6).
        Q_LOGGING_CATEGORY( recentLog, "hc.document.recent" )

        constexpr qint64 MaxFileBytes  = 1024 * 1024;
        constexpr int    SchemaVersion = 1;
        constexpr int    LockWaitMs    = 500;
        constexpr int    RetryMs       = 1000;
        constexpr int    MaxAttempts   = 3;
        // 쓰기 한 번은 수 ms 이다. 멈춘(죽지 않은) 프로세스가 잡은 잠금만 이 시간 뒤에 거둔다.
        constexpr int    StaleLockMs   = 10000;

        QString IsoText( const QDateTime& When )
        {
            if( !When.isValid() ) return {};
            // 지역 시각은 오프셋 없이 문자열이 되므로 고정 오프셋으로 바꿔 "+09:00" 을 남긴다.
            return When.toTimeZone( QTimeZone::fromSecondsAheadOfUtc( When.offsetFromUtc() ) ).toString( Qt::ISODateWithMs );
        }

        QDateTime ParseIso( const QJsonValue& Value )
        {
            if( !Value.isString() ) return {};
            const auto When = QDateTime::fromString( Value.toString(), Qt::ISODateWithMs );
            return When.isValid() ? When.toLocalTime() : QDateTime();
        }

        QJsonObject ToJson( const RecentDocument& Document )
        {
            QJsonObject Object{ { QStringLiteral( "path" ), Document.path_ } };
            const auto Text = [ &Object ]( const char* Name, const QString& Value ) {
                if( !Value.isEmpty() ) Object.insert( QLatin1String( Name ), Value );
            };
            const auto Number = [ &Object ]( const char* Name, int Value ) {
                if( Value > 0 ) Object.insert( QLatin1String( Name ), Value );
            };
            Text( "viewId", Document.viewId_ );
            Text( "kind", Document.kind_ );
            Text( "openedAt", IsoText( Document.openedAt_ ) );
            if( Document.pinned_ ) Object.insert( QStringLiteral( "pinned" ), true );
            Text( "pinnedAt", IsoText( Document.pinnedAt_ ) );
            Text( "encoding", Document.encoding_ );
            Text( "markdownView", Document.markdownView_ );
            Number( "pdfPage", Document.pdfPage_ );
            Text( "epubMode", Document.epubMode_ );
            Text( "imageMode", Document.imageMode_ );
            Number( "readingPage", Document.readingPage_ );
            Number( "readingPageCount", Document.readingPageCount_ );
            return Object;
        }

        bool FromJson( const QJsonObject& Object, RecentDocument& Document )
        {
            Document.path_ = Object.value( QStringLiteral( "path" ) ).toString();
            if( Document.path_.trimmed().isEmpty() ) return false;
            Document.viewId_           = Object.value( QStringLiteral( "viewId" ) ).toString();
            Document.kind_             = Object.value( QStringLiteral( "kind" ) ).toString();
            Document.openedAt_         = ParseIso( Object.value( QStringLiteral( "openedAt" ) ) );
            Document.pinned_           = Object.value( QStringLiteral( "pinned" ) ).toBool();
            Document.pinnedAt_         = ParseIso( Object.value( QStringLiteral( "pinnedAt" ) ) );
            Document.encoding_         = Object.value( QStringLiteral( "encoding" ) ).toString();
            Document.markdownView_     = Object.value( QStringLiteral( "markdownView" ) ).toString();
            Document.pdfPage_          = qMax( 0, Object.value( QStringLiteral( "pdfPage" ) ).toInt() );
            Document.epubMode_         = Object.value( QStringLiteral( "epubMode" ) ).toString();
            Document.imageMode_        = Object.value( QStringLiteral( "imageMode" ) ).toString();
            Document.readingPage_      = qMax( 0, Object.value( QStringLiteral( "readingPage" ) ).toInt() );
            Document.readingPageCount_ = qMax( 0, Object.value( QStringLiteral( "readingPageCount" ) ).toInt() );
            return true;
        }

        enum class ReadState { Missing, Ok, Corrupt, Failed };

        ReadState ReadEntries( const QString& File, QList< RecentDocument >& Entries )
        {
            Entries.clear();
            QFile Input( File );
            if( !Input.exists() ) return ReadState::Missing;
            if( !Input.open( QIODevice::ReadOnly ) ) return ReadState::Failed;
            if( Input.size() > MaxFileBytes ) return ReadState::Corrupt;
            const auto Bytes = Input.read( MaxFileBytes + 1 );
            if( Input.error() != QFileDevice::NoError ) return ReadState::Failed;
            QJsonParseError Error;
            const auto Document = QJsonDocument::fromJson( Bytes, &Error );
            if( Error.error != QJsonParseError::NoError || !Document.isObject() ) return ReadState::Corrupt;
            const auto Root = Document.object();
            const auto List = Root.value( QStringLiteral( "entries" ) );
            if( !List.isUndefined() && !List.isArray() ) return ReadState::Corrupt;
            for( const auto& Value : List.toArray() ) {
                RecentDocument Entry;
                if( Value.isObject() && FromJson( Value.toObject(), Entry ) ) Entries.append( Entry );
            }
            return ReadState::Ok;
        }

        // 손상된 파일은 새 파일로 시작하되 원본은 .bad 로 보존한다(이전 .bad 는 덮어쓴다).
        void Quarantine( const QString& File )
        {
            const auto Bad = File + QStringLiteral( ".bad" );
            QFile::remove( Bad );
            if( !QFile::rename( File, Bad ) && !QFile::copy( File, Bad ) )
                qCWarning( recentLog ) << "corrupt-file-not-preserved";
            else
                qCWarning( recentLog ) << "corrupt-file-preserved";
        }

        QString FolderText( const QString& Path )
        {
            // 문자열만 다룬다. QFileInfo 의 경로 계산은 디스크를 읽지 않는다.
            const auto Parent = QFileInfo( Path ).absolutePath();
            const auto Name   = QFileInfo( Parent ).fileName();
            // 루트 바로 아래 파일은 생략할 상위 폴더가 없으므로 루트를 그대로 보인다.
            return Name.isEmpty() ? QDir::toNativeSeparators( Parent ) : QStringLiteral( "…\\" ) + Name;
        }

        void ProbePaths( QPromise< QPair< QString, int > >& Promise, const QStringList& Paths )
        {
            QSet< QString > SlowRoots;
            for( const auto& Path : Paths ) {
                if( Promise.isCanceled() ) return;
                const auto Root = Path.left( 3 ).toUpper();
                auto State = RecentPathProbe::State::Unchecked;
                // UNC · 원격 드라이브는 확인하지 않는다. 예산을 넘긴 드라이브의 나머지 항목도 건너뛴다.
                if( !SlowRoots.contains( Root ) && hc::filetools::LocalFolder( Path ) ) {
                    QElapsedTimer Clock;
                    Clock.start();
                    const bool Exists = QFileInfo::exists( Path );
                    if( Clock.elapsed() > RecentPathProbe::BudgetMs ) SlowRoots.insert( Root );
                    State = Exists ? RecentPathProbe::State::Present : RecentPathProbe::State::Missing;
                }
                Promise.addResult( qMakePair( Path, int( State ) ) );
            }
        }
    }

    QString RecentKey( const QString& Path )
    {
        if( Path.isEmpty() ) return {};
        return QDir::cleanPath( QFileInfo( Path ).absoluteFilePath() ).toCaseFolded();
    }

    QString RecentDocument::Key() const
    {
        return RecentKey( path_ );
    }

    bool ShouldRecordRecent( const RecentRecordContext& Context )
    {
        return Context.enabled_ && !Context.temporarySource_ && Context.originalAuthority_ && !Context.recovered_
            && !Context.untitled_ && !Context.failed_ && !Context.privateReading_ && !Context.brokerCopy_;
    }

    QString RecentDescription( const RecentDocument& Document )
    {
        const auto Folder = FolderText( Document.path_ );
        auto       Kind   = Document.kind_.toLower();
        if( Kind == QLatin1String( "image" ) && ( Document.imageMode_ == QLatin1String( "comic" ) || Document.imageMode_ == QLatin1String( "webtoon" ) ) )
            Kind = Document.imageMode_;

        if( Kind == QLatin1String( "text" ) ) return RecentDocumentStore::tr( "텍스트 · %1" ).arg( Folder );
        if( Kind == QLatin1String( "markdown" ) ) {
            if( Document.markdownView_ == QLatin1String( "split" ) ) return RecentDocumentStore::tr( "Markdown · 나란히 보기" );
            if( Document.markdownView_ == QLatin1String( "preview" ) ) return RecentDocumentStore::tr( "Markdown · 미리보기" );
            if( Document.markdownView_ == QLatin1String( "source" ) ) return RecentDocumentStore::tr( "Markdown · 소스 편집" );
            return RecentDocumentStore::tr( "Markdown · %1" ).arg( Folder );
        }
        if( Kind == QLatin1String( "pdf" ) ) {
            if( Document.pdfPage_ > 0 ) return RecentDocumentStore::tr( "PDF · %1쪽" ).arg( Document.pdfPage_ );
            return RecentDocumentStore::tr( "PDF · %1" ).arg( Folder );
        }
        if( Kind == QLatin1String( "comic" ) ) {
            if( Document.readingPage_ > 0 && Document.readingPageCount_ > 0 )
                return RecentDocumentStore::tr( "만화 · %1 / %2쪽에서 이어 읽기" ).arg( Document.readingPage_ ).arg( Document.readingPageCount_ );
            if( Document.readingPage_ > 0 ) return RecentDocumentStore::tr( "만화 · %1쪽에서 이어 읽기" ).arg( Document.readingPage_ );
            return RecentDocumentStore::tr( "만화 · %1" ).arg( Folder );
        }
        if( Kind == QLatin1String( "webtoon" ) ) {
            if( Document.readingPage_ > 0 && Document.readingPageCount_ > 0 )
                return RecentDocumentStore::tr( "웹툰 · 이미지 %1 / %2에서 이어 읽기" ).arg( Document.readingPage_ ).arg( Document.readingPageCount_ );
            if( Document.readingPage_ > 0 ) return RecentDocumentStore::tr( "웹툰 · 이미지 %1에서 이어 읽기" ).arg( Document.readingPage_ );
            return RecentDocumentStore::tr( "웹툰 · %1" ).arg( Folder );
        }
        if( Kind == QLatin1String( "image" ) ) return RecentDocumentStore::tr( "이미지 · %1" ).arg( Folder );
        if( Kind == QLatin1String( "epub" ) ) {
            if( Document.epubMode_ == QLatin1String( "edit" ) ) return RecentDocumentStore::tr( "EPUB 편집 · %1" ).arg( Folder );
            return RecentDocumentStore::tr( "EPUB 읽기 · %1" ).arg( Folder );
        }
        if( Kind == QLatin1String( "hwp" ) ) return RecentDocumentStore::tr( "한글 문서 · %1" ).arg( Folder );
        return Folder;
    }

    QString RecentDateText( const QDateTime& When, const QDateTime& Now )
    {
        if( !When.isValid() ) return {};
        const auto Local = When.toLocalTime();
        const auto Day   = Local.date();
        const auto Today = Now.toLocalTime().date();
        if( Day == Today ) return RecentDocumentStore::tr( "오늘 %1" ).arg( Local.time().toString( QStringLiteral( "HH:mm" ) ) );
        if( Day == Today.addDays( -1 ) ) return RecentDocumentStore::tr( "어제" );
        if( Day.year() == Today.year() ) return RecentDocumentStore::tr( "%1월 %2일" ).arg( Day.month() ).arg( Day.day() );
        return RecentDocumentStore::tr( "%1년 %2월 %3일" ).arg( Day.year() ).arg( Day.month() ).arg( Day.day() );
    }

    // ---------------------------------------------------------------- RecentDocumentStore

    RecentDocumentStore::RecentDocumentStore( QString Directory, QObject* Parent )
        : QObject( Parent ), directory_( std::move( Directory ) )
    {
        pool_.setMaxThreadCount( 1 );
        pool_.setObjectName( QStringLiteral( "hc.document.recent" ) );
        retry_.setSingleShot( true );
        retry_.setInterval( RetryMs );
        connect( &retry_, &QTimer::timeout, this, &RecentDocumentStore::schedule );
    }

    RecentDocumentStore::~RecentDocumentStore()
    {
        closing_ = true;
        retry_.stop();
        drain();
        // 탭 · 창을 닫을 때의 기록이 남아 있을 수 있으므로 마지막으로 한 번 더 쓴다.
        if( !pending_.isEmpty() && !run( FileName(), pending_, true, false, {} ).ok_ )
            qCWarning( recentLog ) << "pending-operations-dropped" << pending_.size();
    }

    QString RecentDocumentStore::FileName() const
    {
        return QDir( directory_ ).filePath( QStringLiteral( "recent-documents.json" ) );
    }

    void RecentDocumentStore::Load()
    {
        readPending_ = true;
        schedule();
    }

    bool RecentDocumentStore::LoadSync()
    {
        drain();
        const auto Result = run( FileName(), {}, false, false, {} );
        if( !Result.ok_ ) return false;
        readPending_ = checkPending_ = false;
        stamp_ = Result.stamp_;
        adopt( Result.entries_ );
        return true;
    }

    void RecentDocumentStore::Record( RecentDocument Document )
    {
        if( Document.path_.trimmed().isEmpty() ) return;
        Document.path_ = QDir::toNativeSeparators( QDir::cleanPath( QFileInfo( Document.path_ ).absoluteFilePath() ) );
        if( !Document.openedAt_.isValid() ) Document.openedAt_ = QDateTime::currentDateTime();
        Operation Op;
        Op.kind_     = Operation::Kind::Record;
        Op.at_       = Document.openedAt_;
        Op.document_ = std::move( Document );
        mutate( std::move( Op ) );
    }

    void RecentDocumentStore::Pin( const QString& Key, bool Pinned )
    {
        Operation Op;
        Op.kind_ = Operation::Kind::Pin;
        Op.key_  = RecentKey( Key );     // 키는 다시 정규화해도 같으므로 경로를 넘겨도 된다
        Op.flag_ = Pinned;
        Op.at_   = QDateTime::currentDateTime();
        mutate( std::move( Op ) );
    }

    void RecentDocumentStore::Remove( const QString& Key )
    {
        Operation Op;
        Op.kind_ = Operation::Kind::Remove;
        Op.key_  = RecentKey( Key );
        Op.at_   = QDateTime::currentDateTime();
        mutate( std::move( Op ) );
    }

    void RecentDocumentStore::Clear( bool IncludePinned )
    {
        Operation Op;
        Op.kind_ = Operation::Kind::Clear;
        Op.flag_ = IncludePinned;
        Op.at_   = QDateTime::currentDateTime();
        mutate( std::move( Op ) );
    }

    void RecentDocumentStore::ReloadIfChanged()
    {
        checkPending_ = true;
        schedule();
    }

    bool RecentDocumentStore::WaitForIdle( int Ms )
    {
        const QDeadlineTimer Deadline( Ms );
        while( !idle() ) {
            if( Deadline.hasExpired() ) return false;
            QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
            if( !idle() ) QThread::msleep( 5 );
        }
        return true;
    }

    void RecentDocumentStore::apply( QList< RecentDocument >& Entries, const Operation& Op )
    {
        const auto Matches = [ &Op ]( const RecentDocument& Entry ) { return Entry.Key() == Op.key_; };
        switch( Op.kind_ ) {
            case Operation::Kind::Record: {
                const auto Key = Op.document_.Key();
                const auto It  = std::find_if( Entries.begin(), Entries.end(), [ &Key ]( const RecentDocument& Entry ) { return Entry.Key() == Key; } );
                if( It == Entries.end() ) { Entries.append( Op.document_ ); break; }
                // 다른 프로세스가 더 늦게 연 기록이 이미 있으면 그쪽 필드를 둔다.
                if( It->openedAt_.isValid() && Op.document_.openedAt_ < It->openedAt_ ) break;
                auto Updated      = Op.document_;
                Updated.pinned_   = It->pinned_;
                Updated.pinnedAt_ = It->pinnedAt_;
                *It               = std::move( Updated );
                break;
            }
            case Operation::Kind::Pin: {
                const auto It = std::find_if( Entries.begin(), Entries.end(), Matches );
                if( It == Entries.end() ) break;
                // 고정 여부는 pinnedAt 이 늦은 쪽을 따른다.
                if( It->pinnedAt_.isValid() && Op.at_ < It->pinnedAt_ ) break;
                It->pinned_   = Op.flag_;
                It->pinnedAt_ = Op.at_;
                break;
            }
            case Operation::Kind::Remove:
                // 지운 뒤에 다른 곳에서 다시 연 항목은 남긴다.
                Entries.erase( std::remove_if( Entries.begin(), Entries.end(), [ &Op, &Matches ]( const RecentDocument& Entry ) {
                    return Matches( Entry ) && !( Entry.openedAt_.isValid() && Entry.openedAt_ > Op.at_ );
                } ), Entries.end() );
                break;
            case Operation::Kind::Clear:
                Entries.erase( std::remove_if( Entries.begin(), Entries.end(), [ &Op ]( const RecentDocument& Entry ) {
                    return ( Op.flag_ || !Entry.pinned_ ) && !( Entry.openedAt_.isValid() && Entry.openedAt_ > Op.at_ );
                } ), Entries.end() );
                break;
        }
    }

    void RecentDocumentStore::normalize( QList< RecentDocument >& Entries )
    {
        const auto Newer = []( const RecentDocument& Left, const RecentDocument& Right ) { return Left.openedAt_ > Right.openedAt_; };
        std::stable_sort( Entries.begin(), Entries.end(), Newer );

        // 같은 키가 둘이면 늦게 연 쪽을 남기고, 고정 여부만 pinnedAt 이 늦은 쪽에서 가져온다.
        QHash< QString, qsizetype > Seen;
        QList< RecentDocument >     Unique;
        Unique.reserve( Entries.size() );
        for( auto& Entry : Entries ) {
            const auto Key = Entry.Key();
            if( const auto Found = Seen.constFind( Key ); Found != Seen.cend() ) {
                auto& Kept = Unique[ *Found ];
                if( Entry.pinnedAt_.isValid() && ( !Kept.pinnedAt_.isValid() || Entry.pinnedAt_ > Kept.pinnedAt_ ) ) {
                    Kept.pinned_   = Entry.pinned_;
                    Kept.pinnedAt_ = Entry.pinnedAt_;
                }
                continue;
            }
            Seen.insert( Key, Unique.size() );
            Unique.append( std::move( Entry ) );
        }
        Entries = std::move( Unique );

        // 고정 한도를 넘으면 가장 오래전에 고정한 항목부터 고정을 푼다(항목은 일반 한도에서 정리).
        QList< RecentDocument* > Pinned;
        for( auto& Entry : Entries ) if( Entry.pinned_ ) Pinned.append( &Entry );
        if( Pinned.size() > KeepPinned ) {
            std::stable_sort( Pinned.begin(), Pinned.end(), []( const RecentDocument* Left, const RecentDocument* Right ) { return Left->pinnedAt_ > Right->pinnedAt_; } );
            for( qsizetype Index = KeepPinned; Index < Pinned.size(); ++Index ) Pinned[ Index ]->pinned_ = false;
        }

        int Unpinned = 0;
        Entries.erase( std::remove_if( Entries.begin(), Entries.end(), [ &Unpinned ]( const RecentDocument& Entry ) {
            return !Entry.pinned_ && ++Unpinned > KeepUnpinned;
        } ), Entries.end() );

        std::stable_partition( Entries.begin(), Entries.end(), []( const RecentDocument& Entry ) { return Entry.pinned_; } );
    }

    RecentDocumentStore::Outcome RecentDocumentStore::run( const QString& File, const QList< Operation >& Operations, bool Write, bool OnlyIfChanged, const Stamp& Known )
    {
        const auto StampNow = [ &File ] {
            Stamp            Value;
            const QFileInfo Info( File );
            Value.exists_ = Info.exists();
            if( Value.exists_ ) {
                Value.size_     = Info.size();
                Value.modified_ = Info.lastModified();
            }
            return Value;
        };

        Outcome Result;
        Result.write_ = Write;
        if( !Write ) {
            Result.stamp_ = StampNow();
            if( OnlyIfChanged && Result.stamp_ == Known ) {
                Result.ok_      = true;
                Result.changed_ = false;
                return Result;
            }
            if( !Result.stamp_.exists_ ) {
                Result.ok_ = true;
                return Result;
            }
        }
        else if( !QDir().mkpath( QFileInfo( File ).absolutePath() ) ) {
            qCWarning( recentLog ) << "create-directory-failed";
            return Result;
        }

        QLockFile Lock( File + QStringLiteral( ".lock" ) );
        Lock.setStaleLockTime( StaleLockMs );
        const bool Locked = Lock.tryLock( LockWaitMs );
        if( Write && !Locked ) {
            qCInfo( recentLog ) << "lock-busy";
            return Result;
        }
        // 읽기는 잠금 없이도 된다(쓰기가 QSaveFile 로 통째로 바꾸므로). 다만 손상 파일 격리는 잠금을 잡았을 때만 한다.

        QList< RecentDocument > Entries;
        const auto              State = ReadEntries( File, Entries );
        if( State == ReadState::Failed ) {
            qCWarning( recentLog ) << "read-failed";
            return Result;
        }
        if( State == ReadState::Corrupt ) {
            Entries.clear();
            if( Locked ) Quarantine( File );
        }

        if( Write ) {
            for( const auto& Op : Operations ) apply( Entries, Op );
            normalize( Entries );
            QJsonArray List;
            for( const auto& Entry : Entries ) List.append( ToJson( Entry ) );
            auto Bytes = QJsonDocument( QJsonObject{ { QStringLiteral( "schemaVersion" ), SchemaVersion }, { QStringLiteral( "entries" ), List } } )
                             .toJson( QJsonDocument::Indented );
            Bytes.replace( "\n", "\r\n" );
            QSaveFile Output( File );
            Output.setDirectWriteFallback( false );
            if( !Output.open( QIODevice::WriteOnly ) || Output.write( Bytes ) != Bytes.size() || !Output.commit() ) {
                qCWarning( recentLog ) << "atomic-save-failed";
                return Result;
            }
        }
        else
            normalize( Entries );

        Result.entries_ = std::move( Entries );
        Result.stamp_   = StampNow();
        Result.ok_      = true;
        return Result;
    }

    void RecentDocumentStore::mutate( Operation Op )
    {
        const auto Before = entries_;
        apply( entries_, Op );
        normalize( entries_ );
        pending_.append( std::move( Op ) );
        parked_ = false;
        if( entries_ != Before ) emit Changed();
        schedule();
    }

    void RecentDocumentStore::schedule()
    {
        if( closing_ || busy_ ) return;
        if( !pending_.isEmpty() && !parked_ && !retry_.isActive() ) start( true );
        else if( readPending_ || checkPending_ ) start( false );
    }

    void RecentDocumentStore::start( bool Write )
    {
        busy_ = true;
        // 쓰기도 잠금 아래에서 파일을 다시 읽으므로, 걸려 있던 다시 읽기 요청을 함께 채운다.
        const bool Served        = readPending_ || checkPending_;
        const bool OnlyIfChanged = !readPending_;
        readPending_ = checkPending_ = false;
        if( Write ) {
            inFlight_ = pending_;
            pending_.clear();
        }
        const auto File       = FileName();
        const auto Operations = inFlight_;
        const auto Known      = stamp_;
        // 소멸자는 이 작업이 끝나기를 기다린 뒤에 객체를 없애므로 this 로 결과를 보낼 수 있다.
        pool_.start( [ this, File, Operations, Write, OnlyIfChanged, Known, Served ] {
            auto Result     = run( File, Operations, Write, OnlyIfChanged, Known );
            Result.served_  = Served;
            Result.full_    = !Write && !OnlyIfChanged;
            QMetaObject::invokeMethod( this, [ this, Result ] { finish( Result ); }, Qt::QueuedConnection );
        } );
    }

    void RecentDocumentStore::finish( const Outcome& Result )
    {
        busy_ = false;
        if( Result.write_ ) {
            if( !Result.ok_ ) {
                pending_ = inFlight_ + pending_;
                inFlight_.clear();
                if( Result.served_ ) checkPending_ = true;
                if( ++failures_ >= MaxAttempts ) {
                    // 메모리에만 두고 다음 변경 때 함께 쓴다.
                    failures_ = 0;
                    parked_   = true;
                    qCWarning( recentLog ) << "write-deferred" << pending_.size();
                }
                else if( !closing_ )
                    retry_.start();
            }
            else {
                failures_ = 0;
                inFlight_.clear();
                stamp_ = Result.stamp_;
                adopt( Result.entries_ );
            }
        }
        else if( Result.ok_ && Result.changed_ ) {
            stamp_ = Result.stamp_;
            adopt( Result.entries_, Result.full_ );
        }
        schedule();
    }

    void RecentDocumentStore::adopt( const QList< RecentDocument >& Disk, bool Announce )
    {
        // 디스크 상태 위에 아직 쓰지 못한 변경을 다시 얹어야 화면이 되돌아가지 않는다.
        auto Next = Disk;
        for( const auto& Op : inFlight_ ) apply( Next, Op );
        for( const auto& Op : pending_ ) apply( Next, Op );
        normalize( Next );
        if( Next == entries_ && !Announce ) return;
        entries_ = std::move( Next );
        if( !closing_ ) emit Changed();
    }

    void RecentDocumentStore::drain()
    {
        // 진행 중인 작업을 기다린 뒤, 대기열에 들어간 finish 호출을 지금 처리한다.
        for( int Round = 0; busy_ && Round < 100; ++Round ) {
            pool_.waitForDone();
            QCoreApplication::sendPostedEvents( this, QEvent::MetaCall );
        }
        busy_ = false;
    }

    bool RecentDocumentStore::idle() const
    {
        return !busy_ && pending_.isEmpty() && inFlight_.isEmpty() && !retry_.isActive() && !readPending_ && !checkPending_;
    }

    // ---------------------------------------------------------------- RecentPathProbe

    RecentPathProbe::RecentPathProbe( QObject* Parent )
        : QObject( Parent )
    {
    }

    RecentPathProbe::~RecentPathProbe()
    {
        Cancel();
    }

    void RecentPathProbe::Check( const QStringList& Paths )
    {
        bool Added = false;
        for( const auto& Path : Paths ) {
            const auto Key = RecentKey( Path );
            if( Key.isEmpty() || states_.contains( Key ) ) continue;
            if( std::any_of( wanted_.cbegin(), wanted_.cend(), [ &Key ]( const QString& Wanted ) { return RecentKey( Wanted ) == Key; } ) ) continue;
            wanted_.append( Path );
            Added = true;
        }
        if( Added || ( !watcher_ && !wanted_.isEmpty() ) ) launch();
    }

    void RecentPathProbe::Cancel()
    {
        if( !watcher_ ) return;
        watcher_->disconnect( this );
        watcher_->cancel();
        watcher_->deleteLater();
        watcher_ = nullptr;
    }

    void RecentPathProbe::Reset()
    {
        Cancel();
        states_.clear();
        wanted_.clear();
    }

    RecentPathProbe::State RecentPathProbe::StateOf( const QString& Path ) const
    {
        return states_.value( RecentKey( Path ), State::Unchecked );
    }

    bool RecentPathProbe::IsRunning() const
    {
        return watcher_ && !watcher_->isFinished();
    }

    void RecentPathProbe::launch()
    {
        // 확인 중에 새 경로가 오면 지금 작업을 멈추고 남은 경로 전체로 다시 시작한다.
        Cancel();
        if( wanted_.isEmpty() ) return;
        watcher_ = new QFutureWatcher< Result >( this );
        connect( watcher_, &QFutureWatcher< Result >::resultReadyAt, this, &RecentPathProbe::collect );
        watcher_->setFuture( QtConcurrent::run( QThreadPool::globalInstance(), &ProbePaths, wanted_ ) );
    }

    void RecentPathProbe::collect( int Index )
    {
        if( !watcher_ ) return;
        const auto Item = watcher_->resultAt( Index );
        const auto Key  = RecentKey( Item.first );
        wanted_.erase( std::remove_if( wanted_.begin(), wanted_.end(), [ &Key ]( const QString& Wanted ) { return RecentKey( Wanted ) == Key; } ), wanted_.end() );
        states_.insert( Key, State( Item.second ) );
        emit Checked( Item.first );
    }
}
