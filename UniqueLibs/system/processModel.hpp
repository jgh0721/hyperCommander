#pragma once

// 프로세스 트리 모델과 필터 프록시. 행 키는 (PID, 생성 시각)이며, 갱신은 행 단위 신호로 적용해
// 커서·선택·펼침을 유지한다(설계 7.2).

#include <QAbstractItemModel>
#include <QHash>
#include <QIcon>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QVector>

#include <memory>
#include <unordered_map>

#include "commonLibs/system/imageInfo.hpp"
#include "commonLibs/system/processInfo.hpp"
#include "commonLibs/system/processSnapshot.hpp"
#include "systemSupport.hpp"

namespace hc::sys {

struct ProcessNode
{
    hc::system::ProcessKey    key;
    hc::system::ProcessSample sample;
    ProcessNode*              parent = nullptr;
    QVector<ProcessNode*>     children;
    int                       row = 0;               // 형제 목록 안의 위치(parent() 호출을 O(1)로 유지)
    double                    cpu = -1.0;
    double                    io  = 0.0;
    bool                      detailsRequested = false, detailsLoaded = false;
    QString                   name;                  // 표시 이름(생성 때 한 번 만든다)
    QString                   image, commandLine, user, userSid, description, company, version, package;
    hc::system::Integrity     integrity = hc::system::Integrity::Unknown;
    quint32                   priorityClass = 0;
    bool                      elevated = false, wow64 = false, packaged = false, critical = false, protectedProcess = false, opened = false;
    QIcon                     icon;
    int                       newCycles = 0, exitCycles = 0;
    bool                      exiting = false;
    bool                      pseudo = false;          // 인터럽트 행
    bool                      dirty  = false;          // 이번 표본에서 보이는 값이 바뀌었다
    QVector<float>            cpuHistory, privateHistory, ioHistory;
};

class ProcessTreeModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum Column { Name, Pid, Cpu, PrivateBytes, WorkingSet, User, Description, Company, Threads, Handles, Session, Priority, Integrity, CommandLine, ImagePath, Io, ColumnCount };
    enum Role
    {
        SortRole = Qt::UserRole + 101,
        PidRole,
        CreateTimeRole,
        CategoryRole,
        SearchRole,
        ImagePathRole,
        DimRole,
    };

    explicit ProcessTreeModel( QObject* Parent = nullptr );
    ~ProcessTreeModel() override;

    QModelIndex index( int Row, int Column, const QModelIndex& Parent = {} ) const override;
    QModelIndex parent( const QModelIndex& Child ) const override;
    int         rowCount( const QModelIndex& Parent = {} ) const override;
    int         columnCount( const QModelIndex& Parent = {} ) const override;
    QVariant    data( const QModelIndex& Index, int Role ) const override;
    QVariant    headerData( int Section, Qt::Orientation Orientation, int Role ) const override;
    Qt::ItemFlags flags( const QModelIndex& Index ) const override;

    void Apply( const hc::system::Snapshot& Value );
    void SetFlat( bool Flat );
    bool Flat() const { return flat_; }
    void SetSettings( const Settings& Value );
    void SetServices( const QHash<quint32, QStringList>& ByPid );
    void RefreshColors();

    const ProcessNode* Node( const QModelIndex& Index ) const;
    const ProcessNode* Find( const hc::system::ProcessKey& Key ) const;
    QModelIndex        IndexOf( const hc::system::ProcessKey& Key, int Column = 0 ) const;
    // 끝내기 순서: 하위부터. Root 자신을 마지막에 둔다.
    QVector<const ProcessNode*> Descendants( const hc::system::ProcessKey& Root ) const;
    QStringList        Services( quint32 Pid ) const { return services_.value( Pid ); }
    Category           CategoryOf( const ProcessNode& Node ) const;
    const hc::system::SystemTotals& Totals() const { return totals_; }
    int                ProcessCount() const { return int( nodes_.size() ); }
    const QString&     CurrentSid() const { return currentSid_; }
    bool               HasPid( quint32 Pid ) const { return byPid_.contains( Pid ); }
    void               RequestDetails( const QVector<hc::system::ProcessKey>& Keys );
    // 관리자 도우미가 연결된 뒤 표준 권한으로 읽지 못한 프로세스의 상세 정보를 다시 읽는다.
    void               RequeryRestricted();

signals:
    // 한 번의 표본·상세 적용을 묶는다. 보기는 이 사이에 갱신을 미룬다(Qtitan beginUpdate/endUpdate).
    void sigBatchStarted();
    void sigBatchFinished();
    void sigNodesInserted( const QVector<hc::system::ProcessKey>& Keys );
    void sigTotalsChanged();
    void sigDetailsLoaded();

private:
    struct DetailResult
    {
        hc::system::ProcessKey      key;
        hc::system::ProcessDetails  details;
        hc::system::VersionInfo     version;
        QImage                      icon;
        bool                        found = false;
    };
    QModelIndex indexOfNode( const ProcessNode* Node, int Column = 0 ) const;
    QVector<ProcessNode*>& siblings( ProcessNode* Parent ) { return Parent ? Parent->children : roots_; }
    const QVector<ProcessNode*>& siblings( const ProcessNode* Parent ) const { return Parent ? Parent->children : roots_; }
    ProcessNode* desiredParent( const ProcessNode* Node ) const;
    void detach( ProcessNode* Node );
    void attach( ProcessNode* Node, ProcessNode* Parent );
    void removeNode( ProcessNode* Node );
    void startDetails();
    void applyDetails( const QVector<DetailResult>& Results );
    QString searchText( const ProcessNode& Node ) const;
    void    emitDirty( const ProcessNode* Parent );
    void    updateColors();

    std::unordered_map<hc::system::ProcessKey, std::unique_ptr<ProcessNode>, hc::system::ProcessKeyHash> nodes_;
    QVector<ProcessNode*>           roots_;
    QHash<quint32, ProcessNode*>    byPid_;
    QHash<quint32, QStringList>     services_;
    QHash<QString, QIcon>           icons_;
    QVector<hc::system::ProcessKey> pendingDetails_;
    bool                            detailsRunning_ = false;
    bool                            flat_ = false;
    bool                            firstApply_ = true;
    Settings                        settings_;
    hc::system::SystemTotals        totals_;
    std::unique_ptr<ProcessNode>    interrupts_;
    quint32                         servicesPid_ = 0;
    QString                         currentSid_;
    QColor                          colors_[8];      // 분류 색(Category 순서). 테마·설정이 바뀔 때만 다시 만든다
    QColor                          dim_;
};

// 이름·설명·명령줄·사용자·PID 범위로 거른다. 일치 항목의 조상은 남기고 흐리게 표시한다.
class ProcessFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    enum Scope { ScopeName = 1, ScopeDescription = 2, ScopeCommandLine = 4, ScopeUser = 8, ScopePid = 16, ScopeAll = 31 };
    explicit ProcessFilterProxy( QObject* Parent = nullptr );
    void    SetText( const QString& Text );
    QString Text() const { return text_; }
    void    SetScopes( int Scopes );
    int     Scopes() const { return scopes_; }
    void    SetKeepAncestors( bool Keep );
    bool    KeepAncestors() const { return keepAncestors_; }
    // 현재 사용자의 프로세스만 보인다(설정 allUsers 끔). 상세 정보를 읽기 전에는 보인다.
    void    SetOwnOnly( bool OwnOnly );
    bool    OwnOnly() const { return ownOnly_; }
    int     MatchCount() const;
    // 상세 정보(사용자·설명·명령줄)가 들어온 뒤 거르기를 다시 한다. 값이 바뀔 때마다 다시 거르지 않는다.
    void    Refilter() { invalidateFilter(); }
    bool    Filtering() const { return !text_.isEmpty() || ownOnly_; }
    bool    Matches( const QModelIndex& SourceIndex ) const;
    QVariant data( const QModelIndex& Index, int Role ) const override;

protected:
    bool filterAcceptsRow( int Row, const QModelIndex& Parent ) const override;

private:
    QString text_;
    int     scopes_ = ScopeName | ScopeDescription | ScopeCommandLine;
    bool    keepAncestors_ = true;
    bool    ownOnly_ = false;
    bool    owned( const QModelIndex& SourceIndex ) const;
};

} // namespace hc::sys
