#include "stdafx.h"
#include "serviceModel.hpp"

/// 고유 라이브러리
#include "UniqueLibs/systemSessionService.hpp"

/// 외부 라이브러리
#include <fmstyle/ThemeManager.h>

/// 프레임워크
#include <QApplication>
#include <QStyle>

/// 플랫폼
#include <winsvc.h>

namespace hc::sys {

using hc::system::ServiceState;
using hc::system::StartType;

ServiceListModel::ServiceListModel( QObject* Parent ) : QAbstractTableModel( Parent ), settings_( Settings::Load() )
{
}

int ServiceListModel::rowCount( const QModelIndex& Parent ) const { return Parent.isValid() ? 0 : int( rows_.size() ); }
int ServiceListModel::columnCount( const QModelIndex& Parent ) const { return Parent.isValid() ? 0 : ColumnCount; }

int ServiceListModel::IndexOfName( const QString& Name ) const { return index_.value( Name.toLower(), -1 ); }

bool ServiceListModel::IsProblem( const ServiceRow& Row ) const
{
    if( Row.status.state != ServiceState::Stopped || !Row.configLoaded || Row.trigger )
        return false;
    if( Row.start != StartType::Auto && !( Row.start == StartType::AutoDelayed && !settings_.problemExcludesDelayed ) )
        return false;
    return Row.status.win32ExitCode != 0;
}

int ServiceListModel::Count( Filter Value ) const
{
    int Result = 0;
    for( const auto& Row : rows_ )
    {
        switch( Value )
        {
            case Filter::All:      ++Result; break;
            case Filter::Running:  Result += Row.status.state == ServiceState::Running; break;
            case Filter::Stopped:  Result += Row.status.state == ServiceState::Stopped; break;
            case Filter::Problem:  Result += IsProblem( Row ); break;
            case Filter::Disabled: Result += Row.start == StartType::Disabled; break;
        }
    }
    return Result;
}

QHash<quint32, QStringList> ServiceListModel::ServicesByPid() const
{
    QHash<quint32, QStringList> Result;
    for( const auto& Row : rows_ )
        if( Row.status.pid )
            Result[Row.status.pid].append( QString::fromStdWString( Row.status.name ) );
    return Result;
}

void ServiceListModel::Apply( std::vector<hc::system::ServiceStatusInfo> List )
{
    // 처음 채울 때는 한 번에 초기화한다. 행을 하나씩 넣으면 표가 행마다 다시 배치·정렬한다.
    if( rows_.isEmpty() )
    {
        beginResetModel();
        QStringList Added;
        for( auto& Info : List )
        {
            ServiceRow Item;
            Item.status = std::move( Info );
            index_.insert( QString::fromStdWString( Item.status.name ).toLower(), int( rows_.size() ) );
            Added.append( QString::fromStdWString( Item.status.name ) );
            rows_.append( std::move( Item ) );
        }
        endResetModel();
        if( !Added.isEmpty() )
            ReloadConfig( Added );
        emit sigCountsChanged();
        return;
    }
    emit sigBatchStarted();
    QHash<QString, int> Incoming;
    for( int Index = 0; Index < int( List.size() ); ++Index )
        Incoming.insert( QString::fromStdWString( List[Index].name ).toLower(), Index );
    // 사라진 서비스: 뒤에서부터 지운다.
    for( int Row = int( rows_.size() ) - 1; Row >= 0; --Row )
    {
        if( Incoming.contains( QString::fromStdWString( rows_[Row].status.name ).toLower() ) )
            continue;
        beginRemoveRows( {}, Row, Row );
        rows_.removeAt( Row );
        endRemoveRows();
    }
    index_.clear();
    for( int Row = 0; Row < rows_.size(); ++Row )
        index_.insert( QString::fromStdWString( rows_[Row].status.name ).toLower(), Row );
    QStringList Added;
    for( auto& Info : List )
    {
        const auto Key = QString::fromStdWString( Info.name ).toLower();
        const int Row = index_.value( Key, -1 );
        if( Row >= 0 )
        {
            auto& Current = rows_[Row].status;
            const bool Changed = Current.state != Info.state || Current.pid != Info.pid || Current.win32ExitCode != Info.win32ExitCode
                                 || Current.controlsAccepted != Info.controlsAccepted || Current.displayName != Info.displayName;
            Current = std::move( Info );
            if( Changed )
                emit dataChanged( index( Row, 0 ), index( Row, ColumnCount - 1 ) );
            continue;
        }
        const int Insert = int( rows_.size() );
        beginInsertRows( {}, Insert, Insert );
        ServiceRow Item;
        Item.status = std::move( Info );
        rows_.append( std::move( Item ) );
        index_.insert( Key, Insert );
        endInsertRows();
        Added.append( QString::fromStdWString( rows_.last().status.name ) );
    }
    emit sigBatchFinished();
    if( !Added.isEmpty() )
        ReloadConfig( Added );
    emit sigCountsChanged();
}

void ServiceListModel::ReloadMissingConfig()
{
    QStringList Names;
    for( const auto& Row : rows_ )
        if( !Row.configLoaded )
            Names.append( QString::fromStdWString( Row.status.name ) );
    if( !Names.isEmpty() )
        ReloadConfig( Names );
}

void ServiceListModel::ReloadConfig( const QStringList& Names )
{
    if( Names.isEmpty() )
        for( const auto& Row : rows_ )
            pendingConfig_.append( QString::fromStdWString( Row.status.name ) );
    else
        pendingConfig_.append( Names );
    startConfig();
}

void ServiceListModel::startConfig()
{
    if( configRunning_ || pendingConfig_.isEmpty() )
        return;
    configRunning_ = true;
    const auto Batch = pendingConfig_;
    pendingConfig_.clear();
    RunAsync( this,
              [Batch, Helper = AdminSessionConnected()] {
                  QVector<QPair<QString, hc::system::ServiceConfig>> Result;
                  for( const auto& Name : Batch )
                  {
                      hc::system::ServiceConfig Config;
                      uint32_t Error = 0;
                      if( hc::system::QueryServiceConfiguration( Name.toStdWString(), Config, &Error ) )
                          Result.append( { Name, std::move( Config ) } );
                      else if( Helper && Error == ERROR_ACCESS_DENIED )
                      {
                          // 표준 권한으로 읽지 못한 구성은 연결된 관리자 도우미에 묻는다.
                          const auto Reply = AdminSession().RequestIfConnected( "service.query", { { "name", Name } } );
                          if( Reply["outcome"].toString() != "Completed" )
                              continue;
                          const auto Start = Reply["startType"].toString();
                          using hc::system::StartType;
                          Config.start = Start == "auto" ? StartType::Auto : Start == "autoDelayed" ? StartType::AutoDelayed : Start == "manual" ? StartType::Manual
                                       : Start == "disabled" ? StartType::Disabled : Start == "boot" ? StartType::Boot : Start == "system" ? StartType::System : StartType::Unknown;
                          Config.triggerStart = Reply["trigger"].toBool();
                          Config.account = Reply["account"].toString().toStdWString();
                          Config.description = Reply["description"].toString().toStdWString();
                          Config.resolvedImage = Reply["image"].toString().toStdWString();
                          Config.binaryPath = Reply["binaryPath"].toString().toStdWString();
                          Result.append( { Name, std::move( Config ) } );
                      }
                  }
                  return Result;
              },
              [this]( const QVector<QPair<QString, hc::system::ServiceConfig>>& Configs ) {
                  applyConfig( Configs );
                  configRunning_ = false;
                  startConfig();
              } );
}

void ServiceListModel::applyConfig( const QVector<QPair<QString, hc::system::ServiceConfig>>& Configs )
{
    emit sigBatchStarted();
    for( const auto& [Name, Config] : Configs )
    {
        const int Row = IndexOfName( Name );
        if( Row < 0 )
            continue;
        auto& Item = rows_[Row];
        Item.configLoaded = true;
        Item.start = Config.start;
        Item.trigger = Config.triggerStart;
        Item.account = QString::fromStdWString( Config.account );
        Item.description = QString::fromStdWString( Config.description );
        Item.image = QString::fromStdWString( Config.resolvedImage );
        Item.binaryPath = QString::fromStdWString( Config.binaryPath );
        emit dataChanged( index( Row, 0 ), index( Row, ColumnCount - 1 ) );
    }
    emit sigBatchFinished();
    emit sigCountsChanged();
    emit sigConfigLoaded();
}

QVariant ServiceListModel::headerData( int Section, Qt::Orientation Orientation, int Role ) const
{
    if( Orientation != Qt::Horizontal || Role != Qt::DisplayRole )
        return {};
    switch( Section )
    {
        case Name:        return tr( "이름" );
        case DisplayName: return tr( "표시 이름" );
        case State:       return tr( "상태" );
        case StartType:   return tr( "시작 유형" );
        case Account:     return tr( "로그온" );
        case Pid:         return tr( "PID" );
        case Description: return tr( "설명" );
        case Type:        return tr( "유형" );
        case ImagePath:   return tr( "실행 파일" );
        default:          return {};
    }
}

QVariant ServiceListModel::data( const QModelIndex& Index, int Role ) const
{
    if( !Index.isValid() || Index.row() >= rows_.size() )
        return {};
    const auto& Row = rows_[Index.row()];
    const int Column = Index.column();
    switch( Role )
    {
        case Qt::DisplayRole:
        case Qt::EditRole:
            switch( Column )
            {
                case Name:        return QString::fromStdWString( Row.status.name );
                case DisplayName: return QString::fromStdWString( Row.status.displayName );
                case State:
                {
                    auto Text = ServiceStateText( Row.status.state );
                    if( IsProblem( Row ) ) Text += tr( " · 오류 %1" ).arg( Row.status.win32ExitCode );
                    return Text;
                }
                case StartType:   return Row.configLoaded ? StartTypeText( Row.start, Row.trigger ) : QString{};
                case Account:     return Row.configLoaded && !Row.status.driver ? AccountText( Row.account ) : QString{};
                case Pid:         return Row.status.pid ? QString::number( Row.status.pid ) : QString{};
                case Description: return Row.description;
                case Type:        return ServiceTypeText( Row.status.type );
                case ImagePath:   return Row.image;
                default:          return {};
            }
        case SortRole:
            if( Column == Pid ) return qulonglong( Row.status.pid );
            if( Column == State ) return int( Row.status.state );
            return data( Index, Qt::DisplayRole ).toString().toLower();
        case Qt::TextAlignmentRole:
            return Column == Pid ? int( Qt::AlignRight | Qt::AlignVCenter ) : int( Qt::AlignLeft | Qt::AlignVCenter );
        case Qt::ForegroundRole:
        {
            if( Column != State ) return {};
            using fm::style::Token;
            const auto& Colors = fm::style::ThemeManager::instance().colors();
            if( IsProblem( Row ) ) return Colors[Token::Danger];
            switch( Row.status.state )
            {
                case ServiceState::Running: return Colors[Token::Ok];
                case ServiceState::Paused:  return Colors[Token::Paused];
                case ServiceState::Stopped: return Colors[Token::Fg3];
                default:                    return Colors[Token::AccentFg];
            }
        }
        case Qt::DecorationRole:
            if( Column != Name ) return {};
            return QApplication::style()->standardIcon( Row.status.driver ? QStyle::SP_DriveHDIcon : QStyle::SP_ComputerIcon );
        case Qt::ToolTipRole:
            if( Column == State && IsProblem( Row ) ) return tr( "자동 시작 서비스가 오류로 중지되었습니다: %1" ).arg( Win32ErrorText( Row.status.win32ExitCode ) );
            if( Column == Description ) return Row.description;
            return {};
        case NameRole:      return QString::fromStdWString( Row.status.name );
        case StateRole:     return int( Row.status.state );
        case ProblemRole:   return IsProblem( Row );
        case StartTypeRole: return int( Row.start );
        case SearchRole:
            return QString::fromStdWString( Row.status.name ) + '\n' + QString::fromStdWString( Row.status.displayName ) + '\n' + Row.description + '\n' + Row.image;
        default: return {};
    }
}

ServiceFilterProxy::ServiceFilterProxy( QObject* Parent ) : QSortFilterProxyModel( Parent )
{
    setDynamicSortFilter( true );
}

void ServiceFilterProxy::SetText( const QString& Text )
{
    if( text_ == Text.trimmed() ) return;
    text_ = Text.trimmed();
    invalidateFilter();
}

void ServiceFilterProxy::SetFilter( ServiceListModel::Filter Value )
{
    if( filter_ == Value ) return;
    filter_ = Value;
    invalidateFilter();
}

bool ServiceFilterProxy::filterAcceptsRow( int Row, const QModelIndex& Parent ) const
{
    const auto* Model = qobject_cast<const ServiceListModel*>( sourceModel() );
    const auto* Item = Model ? Model->Row( Row ) : nullptr;
    if( !Item ) return false;
    switch( filter_ )
    {
        case ServiceListModel::Filter::Running:  if( Item->status.state != ServiceState::Running ) return false; break;
        case ServiceListModel::Filter::Stopped:  if( Item->status.state != ServiceState::Stopped ) return false; break;
        case ServiceListModel::Filter::Problem:  if( !Model->IsProblem( *Item ) ) return false; break;
        case ServiceListModel::Filter::Disabled: if( Item->start != StartType::Disabled ) return false; break;
        default: break;
    }
    if( text_.isEmpty() ) return true;
    return sourceModel()->index( Row, 0, Parent ).data( ServiceListModel::SearchRole ).toString().contains( text_, Qt::CaseInsensitive );
}

} // namespace hc::sys
