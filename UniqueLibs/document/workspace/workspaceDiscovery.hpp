#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

namespace hc::document::workspace
{
    /// 폴더에 있는 워크스페이스 표시(.multiroot).
    enum class MarkerKind { None, HyperEditor, MultiRoot };

    /// Folder\.multiroot 에 hypereditor.json 또는 MRE workspace.json(schema 1)이 있는가.
    /// projects.json · preview-inputs 만 있는 .multiroot 는 워크스페이스가 아니다(설계 6.3).
    [[nodiscard]] MarkerKind ReadMarker( const QString& Folder );

    /// 상위 찾기의 후보 하나.
    struct Candidate
    {
        enum class Kind { HyperEditor, MultiRoot, Known };
        QString root_;
        Kind kind_ = Kind::HyperEditor;
        QDateTime savedAt_;         // 알 수 없으면 무효
        int documentCount_ = -1;    // 지난 세션 문서 수(알 수 없으면 -1)
    };

    struct FindResult
    {
        QList< Candidate > candidates_;     // 가까운 것부터
        bool timedOut_ = false;
    };

    /// FilePath 의 폴더부터 드라이브 루트(UNC 는 공유 루트)까지 올라가며 워크스페이스 후보를 모은다(설계 6.3).
    /// KnownRoots(프로필에 상태를 둔 루트 · 최근 워크스페이스)는 디스크를 읽지 않고 경로만 비교한다.
    /// 일꾼 스레드에서 부르는 동기 함수다. Deadline 이 지나면 그때까지 찾은 것만 돌려준다(timedOut_).
    /// 끊긴 원격 드라이브는 찾지 않는다.
    [[nodiscard]] FindResult FindAncestorWorkspaces( const QString& FilePath, const QStringList& KnownRoots, int DeadlineMs = 1500 );

    /// 폴더 → 표시 캐시(여러 스레드에서 쓴다). 탐색기 트리의 ‘워크스페이스’ · ‘MRE 세션’ 표시에 쓴다.
    class MarkerCache
    {
    public:
        static MarkerCache& Instance();
        /// 캐시에 있으면 그 값, 없으면 디스크를 읽어 넣는다.
        MarkerKind Get( const QString& Folder );
        /// 캐시에 있을 때만(디스크를 읽지 않는다).
        [[nodiscard]] bool Peek( const QString& Folder, MarkerKind* Kind ) const;
        void Put( const QString& Folder, MarkerKind Kind );
        void Invalidate( const QString& Folder );
        void Clear();

    private:
        mutable QMutex mutex_;
        QHash< QString, MarkerKind > markers_;
    };

    /// 프로세스 전역: 워크스페이스 루트 → 그것에 묶인 창(설계 6.1). 같은 폴더는 한 창에만 묶인다.
    /// 창이 사라지면 저절로 풀린다.
    class Registry final : public QObject
    {
        Q_OBJECT
    public:
        static Registry& Instance();
        /// Window 를 Root 에 묶는다. 다른 창이 이미 묶여 있으면 false.
        bool Bind( const QString& Root, QObject* Window );
        void Unbind( QObject* Window );
        [[nodiscard]] QObject* WindowFor( const QString& Root ) const;
        /// Path 를 품은 루트 가운데 가장 깊은 것의 창.
        [[nodiscard]] QObject* WindowContaining( const QString& Path, QString* Root = nullptr ) const;
        [[nodiscard]] QString RootOf( const QObject* Window ) const;
        [[nodiscard]] QStringList Roots() const;

    private:
        Registry() = default;
        void purge();
        QHash< QString, QPair< QString, QPointer< QObject > > > windows_;    // 키 → (정규 루트, 창)
    };
}
