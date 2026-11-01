#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <vector>

namespace hc::storage { class IContentSource; }

// 오디오 미리 듣기의 순수 로직: 재생 대상 판정, 시간 표기, 대기열.
// UI·재생 엔진과 무관하므로 단위 시험에서 그대로 쓴다.
namespace hc::audio
{
    enum class RepeatMode { Off, Folder, One };

    inline QString RepeatModeName( RepeatMode Mode )
    {
        return Mode == RepeatMode::Folder ? QStringLiteral( "folder" ) : Mode == RepeatMode::One ? QStringLiteral( "one" ) : QStringLiteral( "off" );
    }
    inline RepeatMode RepeatModeFromName( const QString& Name )
    {
        return Name == "folder" ? RepeatMode::Folder : Name == "one" ? RepeatMode::One : RepeatMode::Off;
    }

    // 키트 FFmpeg로 재생을 확인한 형식(2026-11-01 실측)과 디코더만 확인한 ape·mpc·dsf·dff.
    // MIDI·트래커는 디코더가 없어 넣지 않는다.
    inline QString DefaultExtensionText()
    {
        return QStringLiteral( "mp3,m4a,aac,flac,wav,ogg,oga,opus,wma,ape,wv,aif,aiff,mka,mp2,ac3,dts,amr,caf,mpc,dsf,dff" );
    }
    inline QStringList ParseExtensions( const QString& Text )
    {
        QStringList Result;
        for( auto Part : Text.split( QRegularExpression( "[,;\\s]+" ), Qt::SkipEmptyParts ) )
        {
            Part = Part.trimmed().toLower();
            while( Part.startsWith( '.' ) || Part.startsWith( '*' ) ) Part.remove( 0, 1 );
            if( !Part.isEmpty() && Part.size() <= 16 && !Result.contains( Part ) ) Result.append( Part );
        }
        return Result;
    }
    inline QStringList DefaultExtensions() { return ParseExtensions( DefaultExtensionText() ); }
    inline bool IsPlayable( const QString& FileName, const QStringList& Extensions )
    {
        const auto Dot = FileName.lastIndexOf( '.' );
        if( Dot <= 0 || Dot + 1 >= FileName.size() ) return false;
        return Extensions.contains( FileName.mid( Dot + 1 ).toLower() );
    }

    // m:ss, 한 시간 이상은 h:mm:ss. 길이를 모르면 -:--.
    inline QString FormatDuration( qint64 Ms )
    {
        if( Ms < 0 ) return QStringLiteral( "-:--" );
        const auto Total = Ms / 1000, Hours = Total / 3600, Minutes = Total / 60 % 60, Seconds = Total % 60;
        return Hours > 0 ? QString( "%1:%2:%3" ).arg( Hours ).arg( Minutes, 2, 10, QChar( '0' ) ).arg( Seconds, 2, 10, QChar( '0' ) )
                         : QString( "%1:%2" ).arg( Total / 60 ).arg( Seconds, 2, 10, QChar( '0' ) );
    }

    struct AudioTrack
    {
        QString                                         name_;          // 표시 이름(파일 이름)
        QString                                         path_;          // 패널이 보여 준 전체 경로
        QString                                         key_;           // 패널 행과 맞추는 선택 키
        QString                                         folder_;        // 위치 보기 대상 폴더
        QString                                         localPath_;     // Local·Smb 원본 경로. 비어 있으면 source_를 실체화한다
        std::shared_ptr<hc::storage::IContentSource>    source_;
        std::uint64_t                                   size_ = 0;
    };

    // 재생 시작 시점의 패널 화면 순서 스냅샷. order_는 재생 순서(무작위 반영)이고
    // tracks_는 화면 순서를 유지한다.
    class AudioQueue
    {
    public:
        void Reset( std::vector<AudioTrack> Tracks, qsizetype Start )
        {
            tracks_ = std::move( Tracks );
            current_ = tracks_.empty() ? -1 : std::clamp<qsizetype>( Start, 0, Size() - 1 );
            rebuildOrder( seed_ );
        }
        void Clear() { tracks_.clear(); order_.clear(); current_ = -1; }
        bool IsEmpty() const { return tracks_.empty(); }
        qsizetype Size() const { return qsizetype( tracks_.size() ); }
        qsizetype CurrentIndex() const { return current_; }
        const AudioTrack* Current() const { return At( current_ ); }
        const AudioTrack* At( qsizetype Index ) const { return Index >= 0 && Index < Size() ? &tracks_[ size_t( Index ) ] : nullptr; }
        const std::vector<AudioTrack>& Tracks() const { return tracks_; }
        const std::vector<qsizetype>& Order() const { return order_; }
        void Select( qsizetype Index ) { if( At( Index ) ) current_ = Index; }

        RepeatMode Repeat() const { return repeat_; }
        void SetRepeat( RepeatMode Mode ) { repeat_ = Mode; }
        bool Shuffle() const { return shuffle_; }
        // 켤 때 현재 곡을 첫 자리에 두고 나머지를 섞는다. 끌 때는 화면 순서로 돌아간다.
        void SetShuffle( bool Enabled, quint32 Seed )
        {
            shuffle_ = Enabled; seed_ = Seed; rebuildOrder( Seed );
        }

        // 사용자의 "다음 곡". 마지막이면 반복 폴더일 때만 처음으로 간다.
        std::optional<qsizetype> Next() const
        {
            const auto At = position();
            if( At < 0 ) return std::nullopt;
            if( At + 1 < qsizetype( order_.size() ) ) return order_[ size_t( At + 1 ) ];
            if( repeat_ == RepeatMode::Folder ) return order_.front();
            return std::nullopt;
        }
        // 곡이 끝났을 때. 반복 한 곡은 같은 곡을 다시 연다.
        std::optional<qsizetype> NextAfterEnd() const
        {
            if( repeat_ == RepeatMode::One && current_ >= 0 ) return current_;
            return Next();
        }
        // 3초가 지났거나 첫 곡이면 같은 곡을 처음부터, 아니면 이전 곡.
        std::optional<qsizetype> Previous( qint64 PositionMs ) const
        {
            const auto At = position();
            if( At < 0 ) return std::nullopt;
            if( PositionMs > 3000 ) return current_;
            if( At > 0 ) return order_[ size_t( At - 1 ) ];
            if( repeat_ == RepeatMode::Folder ) return order_.back();
            return current_;
        }

    private:
        qsizetype position() const
        {
            const auto Found = std::find( order_.begin(), order_.end(), current_ );
            return Found == order_.end() ? -1 : qsizetype( Found - order_.begin() );
        }
        void rebuildOrder( quint32 Seed )
        {
            order_.resize( tracks_.size() );
            std::iota( order_.begin(), order_.end(), qsizetype( 0 ) );
            if( !shuffle_ || current_ < 0 ) return;
            std::erase( order_, current_ );
            std::mt19937 Random( Seed );
            std::shuffle( order_.begin(), order_.end(), Random );
            order_.insert( order_.begin(), current_ );
        }

        std::vector<AudioTrack>     tracks_;
        std::vector<qsizetype>      order_;
        qsizetype                   current_ = -1;
        RepeatMode                  repeat_ = RepeatMode::Off;
        bool                        shuffle_ = false;
        quint32                     seed_ = 0;
    };
}
