#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace hc::document::workspace
{
    /// 문서 화면 위치. HotExit 매니페스트의 position 과 같은 모양(anchor · caret 은 문자 위치, firstLine 은 0 부터).
    /// MRE 세션을 가져온 것은 줄 · 열(1 부터)만 있다 — caretLine_ > 0 이면 그쪽을 쓴다.
    struct Position
    {
        qint64 anchor_ = -1, caret_ = -1;
        int firstLine_ = -1, firstSubLine_ = 0, xOffset_ = 0;
        int caretLine_ = -1, caretColumn_ = -1;
        [[nodiscard]] bool IsLineColumn() const { return caretLine_ > 0; }
        [[nodiscard]] bool IsValid() const { return caret_ >= 0 || caretLine_ > 0; }
    };

    struct DocumentEntry
    {
        QString path_;          // 절대 경로(읽을 때 지금 루트로 풀어 둔다)
        bool outside_ = false;  // 루트 밖
        QString kind_;          // text · markdown · rst · pdf · image · epub · hwp …
        std::optional< Position > position_;
        QJsonObject view_;      // 문서 화면의 추가 상태(보기 방식 등)
    };

    struct ExplorerState
    {
        QStringList expanded_;  // 절대 경로
        QString selected_, topItem_;
        bool operator==( const ExplorerState& ) const = default;
    };

    /// <루트>\.multiroot\hypereditor.json (schema 1, 설계 6.4). 경로는 루트 기준 상대 경로로 적고, 읽을 때는 늘
    /// **지금 루트** 기준으로 푼다 — 폴더를 통째로 옮겨도 복원된다. 모르는 최상위 키는 extra_ 에 남겨 다시 쓴다.
    struct State
    {
        static constexpr int kSchema = 1;
        int schema_ = kSchema;
        QDateTime savedAt_;
        QString root_;                      // 진단용(기록된 루트)
        QList< DocumentEntry > documents_;
        QString active_;                    // 절대 경로
        ExplorerState explorer_;
        QByteArray dockLayout_;
        int dockVersion_ = 0;
        QByteArray geometry_;
        QJsonObject findInFiles_;
        QJsonObject extra_;                 // 모르는 최상위 키
        bool newerSchema_ = false;          // 더 새 판이 쓴 파일: 읽기만 하고 덮어쓰지 않는다
        [[nodiscard]] bool IsEmpty() const { return documents_.isEmpty() && explorer_.expanded_.isEmpty() && dockLayout_.isEmpty() && geometry_.isEmpty(); }
    };

    [[nodiscard]] QJsonObject ToJson( const State& Value, const QString& Root );
    /// 깨진 값은 MRE solWorkspaceSession.cpp 의 규칙처럼 다듬는다(음수 위치 버림 · 같은 문서 중복 제거).
    [[nodiscard]] State FromJson( const QJsonObject& Json, const QString& Root );

    enum class Location { Folder, Profile };

    struct LoadResult
    {
        State state_;
        bool found_ = false;
        Location from_ = Location::Folder;
        QString error_;
    };

    struct SaveResult
    {
        bool ok_ = false;
        Location to_ = Location::Folder;
        bool fellBack_ = false;     // 폴더에 못 써 프로필에 썼다(한 번 알린다)
        QString error_;
    };

    /// 상태 파일의 자리를 정하고 읽고 쓴다(설계 6.4). 설정 폴더는 호출자가 준다(전역 설정을 읽지 않는다).
    class Store
    {
    public:
        explicit Store( QString ConfigDirectory );
        [[nodiscard]] static QString FolderStatePath( const QString& Root );
        [[nodiscard]] QString ProfileStatePath( const QString& Root ) const;
        /// 폴더 · 프로필 둘 다 읽어 savedAt 이 새 쪽을 고른다.
        [[nodiscard]] LoadResult Load( const QString& Root ) const;
        /// Preferred 자리에 원자적으로(QSaveFile) 쓴다. 폴더에 못 쓰면 프로필에 쓰고 fellBack_.
        /// 그 자리의 파일이 더 새 판이면 쓰지 않는다(error_ = "newer-schema").
        SaveResult Save( const QString& Root, const State& Value, Location Preferred ) const;
        /// 프로필에 상태를 둔 워크스페이스 루트들(상위 찾기의 알려진 루트).
        [[nodiscard]] QStringList ProfileRoots() const;

    private:
        QString configDirectory_;
    };

    /// MRE 의 <루트>\.multiroot\workspace.json(schema 1)을 한 번 가져온다(설계 6.6). 기록된 workspaceRoot 로 시작하는
    /// 경로는 지금 루트로 다시 잇고, 나머지는 바깥 문서로 둔다. dockLayout · 창 위치 · 분할 · 배율 · externalFiles 는 버린다.
    [[nodiscard]] std::optional< State > ImportMultiRoot( const QString& Root );
    [[nodiscard]] QString MultiRootSessionPath( const QString& Root );

    /// MRE projects.json(version 2) 한 줄(solSphinxProjectRegistry.cpp:24-44).
    struct ProjectRecord
    {
        QString projectId_, rootPath_, confPath_, sourcePath_, rootDoc_, buildPath_;
        bool mystMarkdown_ = false;
        bool operator==( const ProjectRecord& ) const = default;
    };
    [[nodiscard]] QString ProjectsCachePath( const QString& Root );
    /// conf.py 가 사라진 줄은 버린다. 판이 다르거나 깨졌으면 빈 목록.
    [[nodiscard]] QList< ProjectRecord > ReadProjectsCache( const QString& Root );
    /// MRE 와 같은 형식으로 원자적으로 쓴다(MRE 는 비원자 쓰기였다).
    bool WriteProjectsCache( const QString& Root, const QList< ProjectRecord >& Projects );
}
