#include "stdafx.h"
#include "processModel.hpp"

/// 고유 라이브러리
#include "UniqueLibs/systemSessionService.hpp"

/// 내부 라이브러리
#include "UniqueLibs/nativeIconExtractor.hpp"

/// 프레임워크
#include <QApplication>
#include <QStyle>

#include <algorithm>
#include <functional>

namespace hc::sys {

namespace {
constexpr int HistoryLength = 60;

void Push( QVector<float>& History, float Value )
{
    if( History.size() >= HistoryLength )
        History.removeFirst();
    History.append( Value );
}

QIcon GenericIcon( bool System )
{
    static const QIcon Program = QApplication::style()->standardIcon( QStyle::SP_FileIcon );
    static const QIcon Machine = QApplication::style()->standardIcon( QStyle::SP_ComputerIcon );
    return System ? Machine : Program;
}
} // namespace

ProcessTreeModel::ProcessTreeModel( QObject* Parent )
    : QAbstractItemModel( Parent ), settings_( Settings::Load() ), currentSid_( QString::fromStdWString( hc::system::CurrentUserSid() ) )
{
    updateColors();
}

void ProcessTreeModel::updateColors()
{
    for( int Index = 0; Index < 8; ++Index )
        colors_[Index] = CategoryColor( Category( Index ), settings_.colors );
    dim_ = DimColor();
}

ProcessTreeModel::~ProcessTreeModel() = default;

void ProcessTreeModel::SetSettings( const Settings& Value )
{
    const bool Interrupts = settings_.showInterrupts != Value.showInterrupts;
    settings_ = Value;
    if( Interrupts && interrupts_ && !Value.showInterrupts && interrupts_->parent )
        detach( interrupts_.get() );
    RefreshColors();
}

void ProcessTreeModel::SetServices( const QHash<quint32, QStringList>& ByPid )
{
    QSet<quint32> Changed;
    for( auto It = ByPid.cbegin(); It != ByPid.cend(); ++It )
        if( !services_.contains( It.key() ) ) Changed.insert( It.key() );
    for( auto It = services_.cbegin(); It != services_.cend(); ++It )
        if( !ByPid.contains( It.key() ) ) Changed.insert( It.key() );
    services_ = ByPid;
    if( Changed.isEmpty() )
        return;
    emit sigBatchStarted();
    for( const auto Pid : Changed )
        if( auto* Node = byPid_.value( Pid ); Node && Node->row >= 0 )
            emit dataChanged( indexOfNode( Node, 0 ), indexOfNode( Node, ColumnCount - 1 ), { Qt::BackgroundRole } );
    emit sigBatchFinished();
}

void ProcessTreeModel::RefreshColors()
{
    updateColors();
    emit sigBatchStarted();
    std::function<void( const ProcessNode* )> Notify = [&]( const ProcessNode* Parent ) {
        const auto& List = siblings( Parent );
        if( List.isEmpty() )
            return;
        emit dataChanged( createIndex( 0, 0, List.first() ), createIndex( int( List.size() ) - 1, ColumnCount - 1, List.last() ),
                          { Qt::BackgroundRole, Qt::ForegroundRole } );
        for( const auto* Child : List )
            if( !Child->children.isEmpty() )
                Notify( Child );
    };
    Notify( nullptr );
    emit sigBatchFinished();
}

QModelIndex ProcessTreeModel::index( int Row, int Column, const QModelIndex& Parent ) const
{
    if( Column < 0 || Column >= ColumnCount || ( Parent.isValid() && Parent.column() != 0 ) )
        return {};
    const auto* Owner = Parent.isValid() ? static_cast<const ProcessNode*>( Parent.internalPointer() ) : nullptr;
    const auto& List = siblings( Owner );
    if( Row < 0 || Row >= List.size() )
        return {};
    return createIndex( Row, Column, List[Row] );
}

QModelIndex ProcessTreeModel::indexOfNode( const ProcessNode* Node, int Column ) const
{
    if( !Node )
        return {};
    return createIndex( Node->row, Column, const_cast<ProcessNode*>( Node ) );
}

QModelIndex ProcessTreeModel::parent( const QModelIndex& Child ) const
{
    if( !Child.isValid() )
        return {};
    const auto* Node = static_cast<const ProcessNode*>( Child.internalPointer() );
    return Node && Node->parent ? indexOfNode( Node->parent ) : QModelIndex{};
}

int ProcessTreeModel::rowCount( const QModelIndex& Parent ) const
{
    if( Parent.isValid() && Parent.column() != 0 )
        return 0;
    const auto* Owner = Parent.isValid() ? static_cast<const ProcessNode*>( Parent.internalPointer() ) : nullptr;
    return int( siblings( Owner ).size() );
}

int ProcessTreeModel::columnCount( const QModelIndex& ) const { return ColumnCount; }

Qt::ItemFlags ProcessTreeModel::flags( const QModelIndex& Index ) const
{
    return Index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
}

const ProcessNode* ProcessTreeModel::Node( const QModelIndex& Index ) const
{
    return Index.isValid() ? static_cast<const ProcessNode*>( Index.internalPointer() ) : nullptr;
}

const ProcessNode* ProcessTreeModel::Find( const hc::system::ProcessKey& Key ) const
{
    const auto Found = nodes_.find( Key );
    return Found != nodes_.end() ? Found->second.get() : nullptr;
}

QModelIndex ProcessTreeModel::IndexOf( const hc::system::ProcessKey& Key, int Column ) const
{
    return indexOfNode( Find( Key ), Column );
}

QVector<const ProcessNode*> ProcessTreeModel::Descendants( const hc::system::ProcessKey& Root ) const
{
    QVector<const ProcessNode*> Result;
    const auto* Start = Find( Root );
    if( !Start )
        return Result;
    std::function<void( const ProcessNode* )> Visit = [&]( const ProcessNode* Node ) {
        for( const auto* Child : Node->children )
            if( !Child->pseudo && !Child->exiting )
                Visit( Child );
        Result.append( Node );
    };
    if( flat_ )
    {
        // 목록 모드에서도 부모 PID 관계로 하위를 찾는다.
        std::function<void( const ProcessNode* )> Walk = [&]( const ProcessNode* Node ) {
            for( const auto& [Key, Item] : nodes_ )
                if( !Item->pseudo && !Item->exiting && Item->sample.parentPid == Node->key.pid && Item->key.createTime >= Node->key.createTime && Item.get() != Node )
                    Walk( Item.get() );
            Result.append( Node );
        };
        Walk( Start );
        return Result;
    }
    Visit( Start );
    return Result;
}

Category ProcessTreeModel::CategoryOf( const ProcessNode& Node ) const
{
    if( Node.pseudo || Node.key.pid <= 4 )
        return Category::System;
    if( Node.exiting )
        return Category::Exiting;
    if( Node.newCycles > 0 )
        return Category::New;
    if( Node.sample.suspended )
        return Category::Suspended;
    if( ( servicesPid_ && Node.sample.parentPid == servicesPid_ ) || services_.contains( Node.key.pid ) )
        return Category::Service;
    if( Node.packaged )
        return Category::Packaged;
    if( !Node.userSid.isEmpty() && Node.userSid == currentSid_ )
        return Category::Own;
    return Category::None;
}

ProcessNode* ProcessTreeModel::desiredParent( const ProcessNode* Node ) const
{
    if( flat_ )
        return nullptr;
    if( Node->pseudo )
        return byPid_.value( 0 );
    if( Node->key.pid == 0 || Node->sample.parentPid == 0 )
        return nullptr;
    auto* Parent = byPid_.value( Node->sample.parentPid );
    // 부모가 끝나 PID가 재사용되었으면 최상위로 올린다(생성 시각이 자식보다 늦은 부모).
    if( !Parent || Parent == Node || Parent->exiting || Parent->key.createTime > Node->key.createTime )
        return nullptr;
    for( auto* Up = Parent; Up; Up = Up->parent )
        if( Up == Node )
            return nullptr;
    return Parent;
}

void ProcessTreeModel::detach( ProcessNode* Node )
{
    auto& List = siblings( Node->parent );
    const int Row = Node->row;
    if( Row < 0 || Row >= List.size() || List[Row] != Node )
        return;
    beginRemoveRows( indexOfNode( Node->parent ), Row, Row );
    List.removeAt( Row );
    for( int Index = Row; Index < List.size(); ++Index )
        List[Index]->row = Index;
    endRemoveRows();
    Node->parent = nullptr;
    Node->row = -1;
}

void ProcessTreeModel::attach( ProcessNode* Node, ProcessNode* Parent )
{
    auto& List = siblings( Parent );
    const int Row = int( List.size() );
    beginInsertRows( indexOfNode( Parent ), Row, Row );
    List.append( Node );
    Node->parent = Parent;
    Node->row = Row;
    endInsertRows();
}

void ProcessTreeModel::removeNode( ProcessNode* Node )
{
    // 자식은 최상위로 올린다(부모가 끝난 프로세스와 같은 규칙).
    const auto Children = Node->children;
    for( auto* Child : Children )
    {
        detach( Child );
        attach( Child, nullptr );
    }
    detach( Node );
    if( byPid_.value( Node->key.pid ) == Node )
        byPid_.remove( Node->key.pid );
    nodes_.erase( Node->key );
}

void ProcessTreeModel::SetFlat( bool Flat )
{
    if( flat_ == Flat )
        return;
    beginResetModel();
    flat_ = Flat;
    roots_.clear();
    for( auto& [Key, Node] : nodes_ )
    {
        Node->children.clear();
        Node->parent = nullptr;
    }
    if( interrupts_ )
    {
        interrupts_->children.clear();
        interrupts_->parent = nullptr;
        interrupts_->row = -1;
    }
    QVector<ProcessNode*> Ordered;
    for( auto& [Key, Node] : nodes_ )
        Ordered.append( Node.get() );
    std::sort( Ordered.begin(), Ordered.end(), []( const ProcessNode* L, const ProcessNode* R ) { return L->key.createTime < R->key.createTime; } );
    if( interrupts_ && settings_.showInterrupts && !flat_ )
        Ordered.append( interrupts_.get() );
    for( auto* Node : Ordered )
    {
        auto* Parent = desiredParent( Node );
        auto& List = siblings( Parent );
        Node->parent = Parent;
        Node->row = int( List.size() );
        List.append( Node );
    }
    endResetModel();
}

QString ProcessTreeModel::searchText( const ProcessNode& Node ) const
{
    return Node.name + '\n' + Node.description + '\n' + Node.commandLine + '\n' + Node.user + '\n'
           + QString::number( Node.key.pid );
}

void ProcessTreeModel::Apply( const hc::system::Snapshot& Value )
{
    emit sigBatchStarted();
    totals_ = Value.totals;
    QSet<ProcessNode*> Seen;
    QVector<ProcessNode*> Created;
    for( const auto& Entry : Value.processes )
    {
        const auto& Key = Entry.sample.key;
        auto Found = nodes_.find( Key );
        ProcessNode* Node = nullptr;
        if( Found == nodes_.end() )
        {
            auto Item = std::make_unique<ProcessNode>();
            Item->key = Key;
            Item->name = QString::fromStdWString( hc::system::ProcessDisplayName( Entry.sample ) );
            Item->newCycles = firstApply_ ? 0 : settings_.highlightCycles;
            Item->row = -1;
            Node = Item.get();
            nodes_.emplace( Key, std::move( Item ) );
            Created.append( Node );
        }
        else
        {
            Node = Found->second.get();
            // 보이는 값이 바뀐 행만 알린다. 대부분의 프로세스는 한 표본 동안 그대로다.
            const auto& Old = Node->sample;
            Node->dirty = Node->newCycles > 0 || std::abs( Node->cpu - Entry.cpu ) >= 0.05 || std::abs( Node->io - Entry.ioPerSec ) >= 1.0
                          || Old.privateBytes != Entry.sample.privateBytes || Old.workingSet != Entry.sample.workingSet
                          || Old.threadCount != Entry.sample.threadCount || Old.handleCount != Entry.sample.handleCount
                          || Old.suspended != Entry.sample.suspended || Old.basePriority != Entry.sample.basePriority;
            if( Node->newCycles > 0 )
                --Node->newCycles;
        }
        Node->sample = Entry.sample;
        Node->cpu = Entry.cpu;
        Node->io = Entry.ioPerSec;
        if( !Value.baseline )
        {
            Push( Node->cpuHistory, float( std::max( 0.0, Entry.cpu ) ) );
            Push( Node->privateHistory, float( Entry.sample.privateBytes ) );
            Push( Node->ioHistory, float( Entry.ioPerSec ) );
        }
        Seen.insert( Node );
        if( Entry.sample.imageName.size() == 12 && _wcsicmp( Entry.sample.imageName.c_str(), L"services.exe" ) == 0 )
            servicesPid_ = Key.pid;
    }
    // 끝난 프로세스는 강조 주기 동안 남긴 뒤 지운다.
    QVector<ProcessNode*> Removed;
    for( auto& [Key, Node] : nodes_ )
    {
        if( Seen.contains( Node.get() ) )
            continue;
        if( !Node->exiting )
        {
            Node->dirty = true;
            Node->exiting = true;
            Node->exitCycles = settings_.highlightCycles;
            Node->cpu = -1.0;
        }
        else
            --Node->exitCycles;
        if( Node->exitCycles <= 0 )
            Removed.append( Node.get() );
    }
    for( auto* Node : Removed )
        removeNode( Node );
    for( auto* Node : Created )
        if( !byPid_.contains( Node->key.pid ) || byPid_.value( Node->key.pid )->exiting )
            byPid_.insert( Node->key.pid, Node );
    for( auto& [Key, Node] : nodes_ )
        if( !Node->exiting )
            byPid_.insert( Key.pid, Node.get() );
    // 부모가 먼저 붙도록 생성 시각 순서로 붙인다.
    std::sort( Created.begin(), Created.end(), []( const ProcessNode* L, const ProcessNode* R ) { return L->key.createTime < R->key.createTime; } );
    for( auto* Node : Created )
        attach( Node, desiredParent( Node ) );
    if( settings_.showInterrupts && !flat_ )
    {
        if( !interrupts_ )
        {
            interrupts_ = std::make_unique<ProcessNode>();
            interrupts_->pseudo = true;
            interrupts_->row = -1;
            interrupts_->description = tr( "하드웨어 인터럽트·DPC" );
        }
        interrupts_->dirty = true;
        interrupts_->cpu = Value.baseline ? -1.0 : totals_.interruptCpu;
        if( !Value.baseline )
            Push( interrupts_->cpuHistory, float( totals_.interruptCpu ) );
        if( !interrupts_->parent && byPid_.value( 0 ) )
            attach( interrupts_.get(), byPid_.value( 0 ) );
    }
    // 값이 바뀐 행만 이어진 구간으로 묶어 알린다.
    emitDirty( nullptr );
    emit sigBatchFinished();
    QVector<hc::system::ProcessKey> Keys;
    for( const auto* Node : Created )
        Keys.append( Node->key );
    firstApply_ = false;
    if( !Keys.isEmpty() )
    {
        emit sigNodesInserted( Keys );
        RequestDetails( Keys );
    }
    emit sigTotalsChanged();
}

void ProcessTreeModel::emitDirty( const ProcessNode* Parent )
{
    const auto& List = siblings( Parent );
    for( int First = 0; First < List.size(); )
    {
        if( !List[First]->dirty )
        {
            ++First;
            continue;
        }
        int Last = First;
        while( Last + 1 < List.size() && List[Last + 1]->dirty )
            ++Last;
        emit dataChanged( createIndex( First, 0, List[First] ), createIndex( Last, ColumnCount - 1, List[Last] ) );
        First = Last + 1;
    }
    for( auto* Child : List )
    {
        Child->dirty = false;
        if( !Child->children.isEmpty() )
            emitDirty( Child );
    }
}

void ProcessTreeModel::RequeryRestricted()
{
    QVector<hc::system::ProcessKey> Keys;
    for( auto& [Key, Node] : nodes_ )
        if( Node->detailsLoaded && Key.pid > 4 && ( !Node->opened || Node->userSid.isEmpty() ) )
        {
            Node->detailsRequested = false;
            Keys.append( Key );
        }
    RequestDetails( Keys );
}

void ProcessTreeModel::RequestDetails( const QVector<hc::system::ProcessKey>& Keys )
{
    for( const auto& Key : Keys )
    {
        auto Found = nodes_.find( Key );
        if( Found == nodes_.end() || Found->second->detailsRequested || Key.pid <= 4 )
            continue;
        Found->second->detailsRequested = true;
        pendingDetails_.append( Key );
    }
    startDetails();
}

void ProcessTreeModel::startDetails()
{
    if( detailsRunning_ || pendingDetails_.isEmpty() )
        return;
    detailsRunning_ = true;
    const auto Batch = pendingDetails_.mid( 0, 48 );
    pendingDetails_.remove( 0, Batch.size() );
    QSet<QString> KnownIcons;
    for( auto It = icons_.cbegin(); It != icons_.cend(); ++It )
        KnownIcons.insert( It.key() );
    RunAsync( this,
              [Batch, KnownIcons, Helper = AdminSessionConnected()] {
                  QVector<DetailResult> Results;
                  QSet<QString> Extracted = KnownIcons;
                  for( const auto& Key : Batch )
                  {
                      DetailResult Result;
                      Result.key = Key;
                      Result.found = hc::system::QueryProcessDetails( Key.pid, Key.createTime, Result.details );
                      // 표준 권한으로 열지 못했거나 토큰을 읽지 못했으면 연결된 관리자 도우미에 묻는다(도우미를 새로 띄우지 않는다).
                      if( Helper && ( !Result.found || !Result.details.opened || !Result.details.tokenRead ) )
                      {
                          const auto Reply = AdminSession().RequestIfConnected( "process.details", { { "pid", qint64( Key.pid ) }, { "created", QString::number( Key.createTime ) } } );
                          if( Reply["outcome"].toString() == "Completed" )
                          {
                              auto& Details = Result.details;
                              Details.opened = true;
                              Details.imagePath = Reply["image"].toString().toStdWString();
                              Details.commandLine = Reply["commandLine"].toString().toStdWString();
                              Details.userName = Reply["user"].toString().toStdWString();
                              Details.userSid = Reply["userSid"].toString().toStdWString();
                              Details.tokenRead = !Details.userSid.empty();
                              Details.integrity = hc::system::Integrity( Reply["integrity"].toInt() );
                              Details.elevated = Reply["elevated"].toBool();
                              Details.wow64 = Reply["wow64"].toBool();
                              Details.priorityClass = quint32( Reply["priorityClass"].toInteger() );
                              Details.critical = Reply["critical"].toBool();
                              Details.protectedProcess = Reply["protected"].toBool();
                              Details.packaged = Reply["packaged"].toBool();
                              Details.packageName = Reply["package"].toString().toStdWString();
                              Result.found = true;
                          }
                      }
                      if( Result.found && !Result.details.imagePath.empty() )
                      {
                          hc::system::ReadVersionInfo( Result.details.imagePath, Result.version );
                          const auto Path = QString::fromStdWString( Result.details.imagePath );
                          if( !Extracted.contains( Path.toLower() ) )
                          {
                              hc::icons::Request Request;
                              Request.kind_ = hc::icons::Kind::Item;
                              Request.path_ = Path;
                              Request.pixels_ = 32;
                              Result.icon = hc::icons::ExtractNativeIcon( Request ).image_;
                              Extracted.insert( Path.toLower() );
                          }
                      }
                      Results.append( std::move( Result ) );
                  }
                  return Results;
              },
              [this]( const QVector<DetailResult>& Results ) {
                  applyDetails( Results );
                  detailsRunning_ = false;
                  startDetails();
              } );
}

void ProcessTreeModel::applyDetails( const QVector<DetailResult>& Results )
{
    emit sigBatchStarted();
    for( const auto& Result : Results )
    {
        auto Found = nodes_.find( Result.key );
        if( Found == nodes_.end() )
            continue;
        auto* Node = Found->second.get();
        Node->detailsLoaded = true;
        Node->opened = Result.found && Result.details.opened;
        if( Result.found )
        {
            const auto& Details = Result.details;
            Node->image = QString::fromStdWString( Details.imagePath );
            Node->commandLine = QString::fromStdWString( Details.commandLine );
            Node->user = QString::fromStdWString( Details.userName );
            Node->userSid = QString::fromStdWString( Details.userSid );
            Node->integrity = Details.integrity;
            Node->priorityClass = Details.priorityClass;
            Node->elevated = Details.elevated;
            Node->wow64 = Details.wow64;
            Node->packaged = Details.packaged;
            Node->package = QString::fromStdWString( Details.packageName );
            Node->critical = Details.critical;
            Node->protectedProcess = Details.protectedProcess;
            Node->description = QString::fromStdWString( Result.version.description );
            Node->company = QString::fromStdWString( Result.version.company );
            Node->version = QString::fromStdWString( Result.version.version );
            const auto IconKey = Node->image.toLower();
            if( !Result.icon.isNull() )
                icons_.insert( IconKey, QIcon( QPixmap::fromImage( Result.icon ) ) );
            Node->icon = icons_.value( IconKey );
        }
        if( Node->row >= 0 )
            emit dataChanged( indexOfNode( Node, 0 ), indexOfNode( Node, ColumnCount - 1 ) );
    }
    emit sigBatchFinished();
    emit sigDetailsLoaded();
}

QVariant ProcessTreeModel::headerData( int Section, Qt::Orientation Orientation, int Role ) const
{
    if( Orientation != Qt::Horizontal || Role != Qt::DisplayRole )
        return {};
    switch( Section )
    {
        case Name:         return tr( "프로세스" );
        case Pid:          return tr( "PID" );
        case Cpu:          return tr( "CPU" );
        case PrivateBytes: return tr( "개인 바이트" );
        case WorkingSet:   return tr( "작업 집합" );
        case User:         return tr( "사용자" );
        case Description:  return tr( "설명" );
        case Company:      return tr( "회사" );
        case Threads:      return tr( "스레드" );
        case Handles:      return tr( "핸들" );
        case Session:      return tr( "세션" );
        case Priority:     return tr( "우선순위" );
        case Integrity:    return tr( "무결성" );
        case CommandLine:  return tr( "명령줄" );
        case ImagePath:    return tr( "이미지 경로" );
        case Io:           return tr( "I/O" );
        default:           return {};
    }
}

QVariant ProcessTreeModel::data( const QModelIndex& Index, int Role ) const
{
    const auto* Node = this->Node( Index );
    if( !Node )
        return {};
    const int Column = Index.column();
    switch( Role )
    {
        case Qt::DisplayRole:
        case Qt::EditRole:
            if( Node->pseudo )
            {
                if( Column == Name ) return tr( "Interrupts" );
                if( Column == Cpu ) return FormatCpu( Node->cpu );
                if( Column == Description ) return Node->description;
                return {};
            }
            switch( Column )
            {
                case Name:         return Node->name;
                case Pid:          return QString::number( Node->key.pid );
                case Cpu:          return FormatCpu( Node->cpu );
                case PrivateBytes: return Node->key.pid == 0 ? QString{} : FormatBytes( Node->sample.privateBytes );
                case WorkingSet:   return Node->key.pid == 0 ? QString{} : FormatBytes( Node->sample.workingSet );
                case User:         return Node->user;
                case Description:  return Node->key.pid == 4 ? tr( "NT Kernel & System" ) : Node->description;
                case Company:      return Node->company;
                case Threads:      return QString::number( Node->sample.threadCount );
                case Handles:      return QString::number( Node->sample.handleCount );
                case Session:      return QString::number( Node->sample.sessionId );
                case Priority:     return Node->priorityClass ? PriorityText( Node->priorityClass ) : QString::number( Node->sample.basePriority );
                case Integrity:    return IntegrityText( Node->integrity );
                case CommandLine:  return Node->commandLine;
                case ImagePath:    return Node->image;
                case Io:           return FormatRate( Node->io );
                default:           return {};
            }
        case SortRole:
            switch( Column )
            {
                case Pid:          return qlonglong( Node->key.pid );
                case Cpu:          return Node->cpu;
                case PrivateBytes: return qulonglong( Node->sample.privateBytes );
                case WorkingSet:   return qulonglong( Node->sample.workingSet );
                case Threads:      return qulonglong( Node->sample.threadCount );
                case Handles:      return qulonglong( Node->sample.handleCount );
                case Session:      return qulonglong( Node->sample.sessionId );
                case Io:           return Node->io;
                case Name:         return Node->pseudo ? QString() : Node->name.toLower();
                default:           return data( Index, Qt::DisplayRole ).toString().toLower();
            }
        case Qt::TextAlignmentRole:
            switch( Column )
            {
                case Pid: case Cpu: case PrivateBytes: case WorkingSet: case Threads: case Handles: case Session: case Io:
                    return int( Qt::AlignRight | Qt::AlignVCenter );
                default: return int( Qt::AlignLeft | Qt::AlignVCenter );
            }
        case Qt::DecorationRole:
            if( Column != Name ) return {};
            if( !Node->icon.isNull() ) return Node->icon;
            return GenericIcon( Node->pseudo || Node->key.pid <= 4 || Node->protectedProcess );
        case Qt::BackgroundRole:
        {
            const auto& Color = colors_[int( CategoryOf( *Node ) )];
            return Color.isValid() ? QVariant( Color ) : QVariant();
        }
        case Qt::ForegroundRole:
            if( Node->sample.suspended && !Node->exiting ) return dim_;
            return {};
        case Qt::ToolTipRole:
        {
            if( Column != Name || Node->pseudo ) return {};
            QStringList Lines;
            if( !Node->image.isEmpty() ) Lines << Node->image;
            if( !Node->commandLine.isEmpty() ) Lines << tr( "명령줄: %1" ).arg( Node->commandLine.left( 1024 ) );
            const auto Hosted = services_.value( Node->key.pid );
            if( !Hosted.isEmpty() ) Lines << tr( "서비스: %1" ).arg( Hosted.join( ", " ) );
            if( Node->detailsLoaded && !Node->opened ) Lines << tr( "표준 권한으로 상세 정보를 읽을 수 없습니다." );
            return Lines.join( '\n' );
        }
        case PidRole:        return qulonglong( Node->key.pid );
        case CreateTimeRole: return QString::number( Node->key.createTime );
        case CategoryRole:   return int( CategoryOf( *Node ) );
        case ImagePathRole:  return Node->image;
        case SearchRole:     return Node->pseudo ? QString{} : searchText( *Node );
        case DimRole:        return false;
        default:             return {};
    }
}

/////////////////////////////////////////////////////////////////////////////
/// ProcessFilterProxy

ProcessFilterProxy::ProcessFilterProxy( QObject* Parent ) : QSortFilterProxyModel( Parent )
{
    setRecursiveFilteringEnabled( true );
    // 매 표본의 값 변경으로 다시 거르지 않는다. 글자·범위·상세 정보가 바뀔 때만 Refilter한다.
    setDynamicSortFilter( false );
}

void ProcessFilterProxy::SetText( const QString& Text )
{
    if( text_ == Text.trimmed() )
        return;
    text_ = Text.trimmed();
    invalidateFilter();
}

void ProcessFilterProxy::SetScopes( int Scopes )
{
    if( scopes_ == Scopes )
        return;
    scopes_ = Scopes ? Scopes : ScopeName;
    invalidateFilter();
}

void ProcessFilterProxy::SetKeepAncestors( bool Keep )
{
    if( keepAncestors_ == Keep )
        return;
    keepAncestors_ = Keep;
    setRecursiveFilteringEnabled( Keep );
    invalidateFilter();
}

bool ProcessFilterProxy::Matches( const QModelIndex& SourceIndex ) const
{
    if( text_.isEmpty() )
        return true;
    const auto* Model = qobject_cast<const ProcessTreeModel*>( sourceModel() );
    const auto* Node = Model ? Model->Node( SourceIndex.siblingAtColumn( 0 ) ) : nullptr;
    if( !Node || Node->pseudo )
        return false;
    if( ( scopes_ & ScopeName ) && Node->name.contains( text_, Qt::CaseInsensitive ) ) return true;
    if( ( scopes_ & ScopeDescription ) && Node->description.contains( text_, Qt::CaseInsensitive ) ) return true;
    if( ( scopes_ & ScopeCommandLine ) && Node->commandLine.contains( text_, Qt::CaseInsensitive ) ) return true;
    if( ( scopes_ & ScopeUser ) && Node->user.contains( text_, Qt::CaseInsensitive ) ) return true;
    if( ( scopes_ & ScopePid ) && QString::number( Node->key.pid ) == text_ ) return true;
    return false;
}

int ProcessFilterProxy::MatchCount() const
{
    if( text_.isEmpty() || !sourceModel() )
        return 0;
    int Count = 0;
    std::function<void( const QModelIndex& )> Walk = [&]( const QModelIndex& Parent ) {
        for( int Row = 0; Row < sourceModel()->rowCount( Parent ); ++Row )
        {
            const auto Child = sourceModel()->index( Row, 0, Parent );
            if( Matches( Child ) ) ++Count;
            Walk( Child );
        }
    };
    Walk( {} );
    return Count;
}

void ProcessFilterProxy::SetOwnOnly( bool OwnOnly )
{
    if( ownOnly_ == OwnOnly )
        return;
    ownOnly_ = OwnOnly;
    invalidateFilter();
}

bool ProcessFilterProxy::owned( const QModelIndex& SourceIndex ) const
{
    if( !ownOnly_ )
        return true;
    const auto* Model = qobject_cast<const ProcessTreeModel*>( sourceModel() );
    const auto* Node = Model ? Model->Node( SourceIndex ) : nullptr;
    if( !Node )
        return true;
    if( Node->pseudo )
        return false;
    return !Node->detailsLoaded || ( !Node->userSid.isEmpty() && Node->userSid == Model->CurrentSid() );
}

bool ProcessFilterProxy::filterAcceptsRow( int Row, const QModelIndex& Parent ) const
{
    const auto Index = sourceModel()->index( Row, 0, Parent );
    return owned( Index ) && Matches( Index );
}

QVariant ProcessFilterProxy::data( const QModelIndex& Index, int Role ) const
{
    if( ( !text_.isEmpty() || ownOnly_ ) && keepAncestors_ && ( Role == Qt::ForegroundRole || Role == Qt::BackgroundRole )
        && !( owned( mapToSource( Index ) ) && Matches( mapToSource( Index ) ) ) )
        return Role == Qt::ForegroundRole ? QVariant( DimColor() ) : QVariant();
    return QSortFilterProxyModel::data( Index, Role );
}

} // namespace hc::sys
