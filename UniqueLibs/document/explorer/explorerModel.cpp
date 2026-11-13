#include "stdafx.h"
#include "explorerModel.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "remoteDrive.hpp"
#include "../workspace/workspacePaths.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDir>
#include <QFileInfo>

namespace hc::document::explorer
{
    namespace
    {
        bool LooksLikeWildcard( const QString& Text )
        {
            return Text.contains( QLatin1Char( '*' ) ) || Text.contains( QLatin1Char( '?' ) );
        }

        // 비교 키: 소문자, '/' 구분, 끝 '/' 없음(드라이브 루트는 "d:/").
        QString KeyOf( const QString& Path )
        {
            return workspace::PathKey( Path );
        }
    }

    // ============================================================================================ NameMatcher

    bool NameMatcher::SetText( const QString& Text )
    {
        const QString Trimmed = Text.trimmed();
        if( Trimmed == text_ )
            return false;
        text_ = Trimmed;
        // 와일드카드는 **전체 일치**다. `*.rst` 가 "a.rst.bak" 까지 잡으면 부분 일치이지 와일드카드가 아니다.
        // MRE FileTreeFilterProxy.cpp:74-88.
        wildcard_ = LooksLikeWildcard( Trimmed )
            ? QRegularExpression( QRegularExpression::wildcardToRegularExpression( Trimmed ), QRegularExpression::CaseInsensitiveOption )
            : QRegularExpression{};
        return true;
    }

    bool NameMatcher::Matches( const QString& Name ) const
    {
        if( text_.isEmpty() )
            return true;
        if( Name.isEmpty() )
            return false;
        return wildcard_.pattern().isEmpty() ? Name.contains( text_, Qt::CaseInsensitive ) : wildcard_.match( Name ).hasMatch();
    }

    // ============================================================================================ FileSystemModel

    FileSystemModel::FileSystemModel( QObject* Parent )
        : QFileSystemModel( Parent )
    {
        setReadOnly( true );
        // 맞지 않는 파일은 흐리게 두지 않고 뺀다(MRE MainWindow.cpp:4589-4597).
        setNameFilterDisables( false );
        setFilter( QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs );
        setOption( QFileSystemModel::DontWatchForChanges, false );
    }

    void FileSystemModel::sort( int Column, Qt::SortOrder Order )
    {
        // 컴퓨터 루트(드라이브 목록)는 정렬하지 않는다 — 프록시가 정렬한다. QFileSystemModel 의 정렬기는 드라이브마다
        // 속성을 동기로 묻기 때문에 끊긴 원격 매핑 하나가 GUI 를 멈춘다(MRE MainWindow.cpp:4602-4618).
        if( rootPath().isEmpty() )
            return;
        QFileSystemModel::sort( Column, Order );
    }

    void FileSystemModel::SetShowHidden( bool Show )
    {
        auto Filters = filter();
        Filters.setFlag( QDir::Hidden, Show );
        Filters.setFlag( QDir::System, Show );
        if( Filters != filter() )
            setFilter( Filters );
    }

    bool FileSystemModel::ShowHidden() const
    {
        return filter().testFlag( QDir::Hidden );
    }

    void FileSystemModel::SetExtensionFilter( const QStringList& NameFilters )
    {
        if( NameFilters == nameFilters() )
            return;
        setNameFilters( NameFilters );
    }

    QStringList FileSystemModel::TextFamilyNameFilters()
    {
        return { QStringLiteral( "*.txt" ), QStringLiteral( "*.md" ), QStringLiteral( "*.rst" ), QStringLiteral( "*.py" ), QStringLiteral( "*.json" ),
                 QStringLiteral( "*.toml" ), QStringLiteral( "*.yaml" ), QStringLiteral( "*.yml" ), QStringLiteral( "*.ini" ), QStringLiteral( "*.cfg" ),
                 QStringLiteral( "*.css" ), QStringLiteral( "*.js" ), QStringLiteral( "*.html" ), QStringLiteral( "*.xml" ), QStringLiteral( "*.csv" ),
                 QStringLiteral( "*.log" ) };
    }

    // ============================================================================================ FilterProxy

    FilterProxy::FilterProxy( QObject* Parent )
        : QSortFilterProxyModel( Parent )
    {
        // 자손이 하나라도 통과하면 조상도 남긴다. 이것이 없으면 "conf" 를 쳤을 때 conf.py 를 담은 폴더까지 사라진다.
        setRecursiveFilteringEnabled( true );
        setDynamicSortFilter( true );
        collator_.setCaseSensitivity( Qt::CaseInsensitive );
        collator_.setNumericMode( true );
        SetExcludedNames( DefaultExcludedNames() );
    }

    QStringList FilterProxy::DefaultExcludedNames()
    {
        return { QStringLiteral( ".git" ), QStringLiteral( ".multiroot" ), QStringLiteral( "__pycache__" ), QStringLiteral( ".venv" ), QStringLiteral( "node_modules" ) };
    }

    void FilterProxy::changeFilter( const std::function< void() >& Change )
    {
        beginFilterChange();
        Change();
        endFilterChange( QSortFilterProxyModel::Direction::Rows );
    }

    void FilterProxy::SetFilterText( const QString& Text )
    {
        NameMatcher Next = matcher_;
        if( !Next.SetText( Text ) )
            return;
        changeFilter( [ this, &Next ] { matcher_ = Next; } );
        emit FilterTextChanged( matcher_.Text() );
    }

    bool FilterProxy::IsMatch( const QModelIndex& SourceIndex ) const
    {
        if( !IsFiltering() || !SourceIndex.isValid() )
            return false;
        return matcher_.Matches( nameOf( SourceIndex ) );
    }

    void FilterProxy::SetRootSourceIndex( const QModelIndex& Index )
    {
        changeFilter( [ this, &Index ] { rootSourceIndex_ = Index; } );
    }

    void FilterProxy::SetScope( const QString& Folder )
    {
        const QString Normalized = Folder.isEmpty() ? QString{} : workspace::NormalizePath( Folder );
        if( Normalized == scope_ )
            return;
        changeFilter( [ this, &Normalized ] {
            scope_ = Normalized;
            scopeKey_ = scope_.isEmpty() ? QString{} : KeyOf( scope_ );
            scopeChainKeys_.clear();
            if( !scope_.isEmpty() )
                for( const auto& Each : workspace::AncestorFolders( scope_ ) )
                    scopeChainKeys_.insert( KeyOf( Each ) );
        } );
    }

    bool FilterProxy::IsInScope( const QString& Path ) const
    {
        if( scope_.isEmpty() )
            return true;
        const QString Key = KeyOf( Path );
        if( Key == scopeKey_ )
            return true;
        const QString Prefix = scopeKey_.endsWith( QLatin1Char( '/' ) ) ? scopeKey_ : scopeKey_ + QLatin1Char( '/' );
        return Key.startsWith( Prefix );
    }

    void FilterProxy::SetExcludedNames( const QStringList& Names )
    {
        QStringList Clean; QSet< QString > Keys;
        for( const auto& Name : Names ) {
            const QString Trimmed = Name.trimmed();
            if( Trimmed.isEmpty() || Keys.contains( Trimmed.toLower() ) )
                continue;
            Clean.append( Trimmed ); Keys.insert( Trimmed.toLower() );
        }
        if( Keys == excludedKeys_ )
            return;
        changeFilter( [ this, &Clean, &Keys ] { excludedNames_ = Clean; excludedKeys_ = Keys; } );
    }

    bool FilterProxy::IsExcludedName( const QString& Name ) const
    {
        return excludedKeys_.contains( Name.toLower() );
    }

    void FilterProxy::SetShowHidden( bool Show )
    {
        if( Show == showHidden_ )
            return;
        changeFilter( [ this, Show ] { showHidden_ = Show; } );
    }

    void FilterProxy::SetAllowedTopLevelRoots( const QStringList& Roots )
    {
        QStringList Keys;
        for( const auto& Root : Roots ) {
            QString Key = KeyOf( Root );
            // "d:/" · "//server/share" — 끝 '/' 는 드라이브 루트에만 남는다. 비교는 끝 '/' 를 떼고 한다.
            while( Key.size() > 1 && Key.endsWith( QLatin1Char( '/' ) ) )
                Key.chop( 1 );
            if( !Key.isEmpty() && !Keys.contains( Key ) )
                Keys.append( Key );
        }
        if( Keys == allowedKeys_ )
            return;
        changeFilter( [ this, &Roots, &Keys ] { allowedRoots_ = Roots; allowedKeys_ = Keys; } );
    }

    bool FilterProxy::IsDirectory( const QModelIndex& SourceIndex ) const
    {
        const QAbstractItemModel* Source = sourceModel();
        if( const auto* FileSystem = qobject_cast< const QFileSystemModel* >( Source ) )
            return FileSystem->isDir( SourceIndex );
        // 단위 시험의 가상 모델 등 QFileSystemModel 이 아닌 모델도 지원한다.
        return Source != nullptr && Source->hasChildren( SourceIndex );
    }

    QString FilterProxy::SourcePath( const QModelIndex& SourceIndex ) const
    {
        if( !SourceIndex.isValid() )
            return {};
        const auto* FileSystem = qobject_cast< const QFileSystemModel* >( sourceModel() );
        if( !FileSystem )
            return SourceIndex.data( Qt::DisplayRole ).toString();
        // 맨 위 드라이브 행의 filePath() 는 링크 속성까지 물어 끊긴 드라이브에서 기다린다. fileName() 은 캐시된 이름이다.
        if( !SourceIndex.parent().isValid() && FileSystem->rootPath().isEmpty() )
            return QDir::fromNativeSeparators( FileSystem->fileName( SourceIndex ) );
        return FileSystem->filePath( SourceIndex );
    }

    QString FilterProxy::nameOf( const QModelIndex& SourceIndex ) const
    {
        if( const auto* FileSystem = qobject_cast< const QFileSystemModel* >( sourceModel() ) )
            return FileSystem->fileName( SourceIndex );
        return SourceIndex.data( Qt::DisplayRole ).toString();
    }

    bool FilterProxy::isRootOrAncestor( const QModelIndex& SourceIndex ) const
    {
        if( !rootSourceIndex_.isValid() || !SourceIndex.isValid() )
            return false;
        for( QModelIndex Node = rootSourceIndex_; Node.isValid(); Node = Node.parent() )
            if( Node == SourceIndex )
                return true;
        return false;
    }

    bool FilterProxy::isScopeChain( const QModelIndex& SourceIndex ) const
    {
        if( scopeChainKeys_.isEmpty() )
            return false;
        QString Key = KeyOf( SourcePath( SourceIndex ) );
        if( scopeChainKeys_.contains( Key ) )
            return true;
        // 드라이브 루트는 "d:/" 와 "d:" 두 모양으로 들어올 수 있다.
        if( Key.endsWith( QLatin1Char( '/' ) ) ) Key.chop( 1 ); else Key.append( QLatin1Char( '/' ) );
        return scopeChainKeys_.contains( Key );
    }

    bool FilterProxy::isOutsideScope( const QModelIndex& SourceIndex ) const
    {
        if( scope_.isEmpty() )
            return false;
        return !IsInScope( SourcePath( SourceIndex ) ) && !isScopeChain( SourceIndex );
    }

    bool FilterProxy::isInsideExcluded( const QModelIndex& SourceIndex ) const
    {
        // 제외 폴더의 자손이 이름이 맞아 조상을 되살리지 않게 위로 올라가며 본다. 뿌리 · 범위 사슬에서 멈춘다 —
        // 워크스페이스 자체가 .venv 안에 있어도 그 안은 보여야 한다.
        for( QModelIndex Node = SourceIndex; Node.isValid(); Node = Node.parent() ) {
            if( Node == rootSourceIndex_ || isScopeChain( Node ) )
                return false;
            if( excludedKeys_.contains( nameOf( Node ).toLower() ) )
                return true;
        }
        return false;
    }

    bool FilterProxy::isAllowedTopLevel( const QModelIndex& SourceIndex ) const
    {
        if( allowedKeys_.isEmpty() || SourceIndex.parent().isValid() )
            return true;
        const auto* FileSystem = qobject_cast< const QFileSystemModel* >( sourceModel() );
        if( FileSystem && !FileSystem->rootPath().isEmpty() )
            return true;
        QString Key = KeyOf( SourcePath( SourceIndex ) );
        while( Key.size() > 1 && Key.endsWith( QLatin1Char( '/' ) ) )
            Key.chop( 1 );
        return allowedKeys_.contains( Key );
    }

    bool FilterProxy::isDisconnectedRemoteDrive( const QModelIndex& SourceIndex ) const
    {
        const auto* FileSystem = qobject_cast< const QFileSystemModel* >( sourceModel() );
        if( FileSystem == nullptr || !SourceIndex.isValid() || SourceIndex.parent().isValid() )
            return false;
        // 파일 모델 맨 위의 "Z:/" 모양 드라이브 루트만 본다(MRE FileTreeFilterProxy.cpp:136-162).
        const QString Path = QDir::fromNativeSeparators( FileSystem->fileName( SourceIndex ) );
        if( ( Path.size() != 2 && Path.size() != 3 ) || Path.at( 1 ) != QLatin1Char( ':' )
            || ( Path.size() == 3 && Path.at( 2 ) != QLatin1Char( '/' ) ) )
            return false;
        return IsDisconnectedRemoteDrivePath( Path );
    }

    bool FilterProxy::filterAcceptsRow( int SourceRow, const QModelIndex& SourceParent ) const
    {
        const QAbstractItemModel* Source = sourceModel();
        if( Source == nullptr )
            return true;
        const QModelIndex Index = Source->index( SourceRow, 0, SourceParent );
        if( !Index.isValid() )
            return false;
        // 끊긴 원격 드라이브는 빈 필터에서도 내보내지 않는다 — 뷰가 그 자식 · 속성을 묻지 않게.
        if( isDisconnectedRemoteDrive( Index ) )
            return false;
        if( !isAllowedTopLevel( Index ) )
            return false;
        // 뷰의 뿌리와 그 조상, 범위 사슬은 무슨 일이 있어도 남긴다. 뿌리가 사라지면 QTreeView 가 무효 rootIndex 를
        // 모델 맨 위로 읽어 드라이브 전체를 보이고 훑기 시작한다(MRE FileTreeFilterProxy.cpp:182-190).
        if( isRootOrAncestor( Index ) || isScopeChain( Index ) )
            return true;
        if( excludesAnything() && isInsideExcluded( Index ) )
            return false;
        if( !IsFiltering() )
            return true;
        // 필터가 켜져 있으면 범위 밖은 거른다(설계 5.7 — 문서 모드에서 드라이브 전체를 훑지 않는다).
        if( isOutsideScope( Index ) )
            return false;
        if( matcher_.Matches( nameOf( Index ) ) )
            return true;
        // 아직 읽지 않은 **디렉터리**는 남긴다. 지우면 그 안을 들여다볼 길이 영영 막힌다. hasChildren 을 빼면
        // 파일에도 canFetchMore 가 true 라 모든 파일이 통과한다(MRE FileTreeFilterProxy.cpp:195-208).
        if( Source->hasChildren( Index ) && Source->canFetchMore( Index ) )
            return true;
        // 이름이 맞은 디렉터리 **아래**는 통째로 보인다. 뿌리 · 범위 사슬에서 멈춘다.
        for( QModelIndex Ancestor = SourceParent; Ancestor.isValid() && Ancestor != rootSourceIndex_ && !isScopeChain( Ancestor ); Ancestor = Ancestor.parent() )
            if( matcher_.Matches( nameOf( Ancestor ) ) )
                return true;
        return false;
    }

    bool FilterProxy::lessThan( const QModelIndex& Left, const QModelIndex& Right ) const
    {
        if( isDisconnectedRemoteDrive( Left ) || isDisconnectedRemoteDrive( Right ) )
            return collator_.compare( Left.data( Qt::DisplayRole ).toString(), Right.data( Qt::DisplayRole ).toString() ) < 0;
        // 폴더가 언제나 파일 앞이다. 정렬 방향을 뒤집어도 그렇다(MRE FileTreeFilterProxy.cpp:223-242).
        const bool LeftIsDir = IsDirectory( Left );
        if( LeftIsDir != IsDirectory( Right ) )
            return sortOrder() == Qt::AscendingOrder ? LeftIsDir : !LeftIsDir;
        return collator_.compare( Left.data( Qt::DisplayRole ).toString(), Right.data( Qt::DisplayRole ).toString() ) < 0;
    }

    // ============================================================================================ OutsideProxy

    OutsideProxy::OutsideProxy( QObject* Parent )
        : QIdentityProxyModel( Parent ), groupLabel_( tr( "워크스페이스 밖" ) ), groupToolTip_( tr( "워크스페이스 밖에서 연 문서" ) )
    {
        // 합성 행은 원본 인덱스가 없으므로 Identity 기본 배치 처리가 그것을 무효로 만든다(MRE ExternalFilesProxy.cpp:28-33).
        setHandleSourceLayoutChanges( false );
    }

    void OutsideProxy::setSourceModel( QAbstractItemModel* Source )
    {
        for( const auto& Connection : connections_ )
            disconnect( Connection );
        connections_.clear();
        if( Source ) {
            // 표시 루트(또는 조상)가 지워지기 전에 합성 묶음을 먼저 걷는다. 아니면 그 영구 인덱스가 무효해진 루트보다
            // 오래 남는다(MRE ExternalFilesProxy.cpp:42-56).
            connections_ << connect( Source, &QAbstractItemModel::rowsAboutToBeRemoved, this, [ this ]( const QModelIndex& Parent, int First, int Last ) {
                for( QModelIndex Ancestor = root_; Ancestor.isValid(); Ancestor = Ancestor.parent() ) {
                    if( Ancestor.parent() != Parent || Ancestor.row() < First || Ancestor.row() > Last )
                        continue;
                    applyVisible( {} );
                    break;
                }
            } );
            // 표시 루트의 행 수가 바뀌면 합성 행의 자리(맨 끝)도 바뀐다. 끝에 붙은 묶음은 원본 삽입 · 제거가 그 앞에서
            // 일어나므로 Identity 의 행 이동 처리로 충분하다.
        }
        QIdentityProxyModel::setSourceModel( Source );
        if( !Source )
            return;
        if( auto* Filter = qobject_cast< FilterProxy* >( Source ) ) {
            matcher_.SetText( Filter->FilterText() );
            connections_ << connect( Filter, &FilterProxy::FilterTextChanged, this, &OutsideProxy::SetFilterText );
        }
        connections_ << connect( Source, &QAbstractItemModel::layoutAboutToBeChanged, this, [ this ] {
            emit layoutAboutToBeChanged();
            layoutIndexes_ = persistentIndexList();
            layoutSources_.clear();
            for( const auto& Index : layoutIndexes_ )
                layoutSources_.append( mapToSource( Index ) );
        } );
        connections_ << connect( Source, &QAbstractItemModel::layoutChanged, this, [ this ] {
            QModelIndexList Updated;
            const QModelIndex Group = GroupIndex();
            for( int At = 0; At < layoutIndexes_.size(); ++At ) {
                const auto& Old = layoutIndexes_.at( At );
                if( IsGroup( Old ) )
                    Updated << ( Group.isValid() ? Group.siblingAtColumn( Old.column() ) : QModelIndex{} );
                else if( IsOutsideFile( Old ) )
                    Updated << ( Group.isValid() ? index( Old.row(), Old.column(), Group ) : QModelIndex{} );
                else
                    Updated << mapFromSource( layoutSources_.at( At ) );
            }
            changePersistentIndexList( layoutIndexes_, Updated );
            layoutIndexes_.clear();
            layoutSources_.clear();
            emit layoutChanged();
        } );
        // 필터가 바뀌어 원본이 다시 세워지면(모델 리셋) 묶음 행은 다음 SetFiles · SetRoot 까지 그대로 다시 계산된다.
        connections_ << connect( Source, &QAbstractItemModel::modelReset, this, [ this ] { visible_ = filtered( files_ ); } );
    }

    void OutsideProxy::SetRoot( const QString& RootPath, const QModelIndex& SourceRoot )
    {
        beginResetModel();
        rootPath_ = RootPath.isEmpty() ? QString{} : workspace::NormalizeRoot( RootPath );
        root_ = SourceRoot;
        files_ = classify();
        visible_ = groupAllowed() ? filtered( files_ ) : QStringList{};
        endResetModel();
    }

    int OutsideProxy::SetFiles( const QStringList& Paths )
    {
        QStringList Next; QSet< QString > Keys;
        for( const auto& Path : Paths ) {
            if( Path.isEmpty() )
                continue;
            const QString Normalized = workspace::NormalizePath( Path );
            const QString Key = workspace::PathKey( Normalized );
            if( Keys.contains( Key ) )
                continue;
            Keys.insert( Key ); Next.append( Normalized );
        }
        candidates_ = Next;
        // 남은 파일은 자리를 지키고 새 파일은 끝에 붙는다(MRE 처럼 정렬하지 않는다).
        QStringList Wanted;
        const QStringList Classified = classify();
        for( const auto& Existing : files_ )
            for( const auto& Each : Classified )
                if( workspace::SamePath( Existing, Each ) ) { Wanted.append( Each ); break; }
        for( const auto& Each : Classified ) {
            bool Known = false;
            for( const auto& Existing : Wanted ) if( workspace::SamePath( Existing, Each ) ) { Known = true; break; }
            if( !Known ) Wanted.append( Each );
        }
        files_ = Wanted;
        applyVisible( groupAllowed() ? filtered( files_ ) : QStringList{} );
        return int( files_.size() );
    }

    void OutsideProxy::SetFilterText( const QString& Text )
    {
        if( !matcher_.SetText( Text ) )
            return;
        applyVisible( groupAllowed() ? filtered( files_ ) : QStringList{} );
    }

    void OutsideProxy::SetGroupLabel( const QString& Label )
    {
        groupLabel_ = Label;
        if( const auto Group = GroupIndex(); Group.isValid() )
            emit dataChanged( Group, Group, { Qt::DisplayRole } );
    }

    void OutsideProxy::SetGroupToolTip( const QString& ToolTip )
    {
        groupToolTip_ = ToolTip;
    }

    QStringList OutsideProxy::classify() const
    {
        if( rootPath_.isEmpty() )
            return candidates_;
        QStringList Result;
        // 루트 안 파일은 트리에 이미 있다. 디스크를 읽지 않고 문자열로 가른다(열린 문서는 경로가 정규화되어 있다).
        for( const auto& Each : candidates_ )
            if( !workspace::IsInside( Each, rootPath_, false ) )
                Result.append( Each );
        return Result;
    }

    QStringList OutsideProxy::filtered( const QStringList& Files ) const
    {
        if( matcher_.IsEmpty() )
            return Files;
        QStringList Result;
        for( const auto& Each : Files )
            if( matcher_.Matches( QFileInfo( Each ).fileName() ) )
                Result.append( Each );
        return Result;
    }

    bool OutsideProxy::groupAllowed() const
    {
        return sourceModel() != nullptr && ( rootPath_.isEmpty() || root_.isValid() );
    }

    int OutsideProxy::groupRow() const
    {
        return sourceModel() ? sourceModel()->rowCount( root_ ) : 0;
    }

    void OutsideProxy::applyVisible( const QStringList& Wanted )
    {
        if( Wanted == visible_ )
            return;
        const QModelIndex RootView = mapFromSource( root_ );
        if( visible_.isEmpty() ) {
            if( Wanted.isEmpty() || !groupAllowed() ) { visible_ = Wanted; return; }
            const int Row = groupRow();
            beginInsertRows( RootView, Row, Row );
            visible_ = Wanted;
            endInsertRows();
            return;
        }
        if( Wanted.isEmpty() ) {
            const int Row = groupRow();
            beginRemoveRows( RootView, Row, Row );
            visible_.clear();
            endRemoveRows();
            return;
        }
        // 빠진 것을 뒤에서부터 걷고, 새 것은 끝에 붙인다.
        const QModelIndex Group = GroupIndex();
        for( int Row = int( visible_.size() ) - 1; Row >= 0; --Row ) {
            bool Keep = false;
            for( const auto& Each : Wanted ) if( workspace::SamePath( Each, visible_.at( Row ) ) ) { Keep = true; break; }
            if( Keep ) continue;
            beginRemoveRows( Group, Row, Row );
            visible_.removeAt( Row );
            endRemoveRows();
        }
        for( const auto& Each : Wanted ) {
            bool Known = false;
            for( const auto& Existing : visible_ ) if( workspace::SamePath( Existing, Each ) ) { Known = true; break; }
            if( Known ) continue;
            const int Row = int( visible_.size() );
            beginInsertRows( Group, Row, Row );
            visible_.append( Each );
            endInsertRows();
        }
    }

    bool OutsideProxy::IsGroup( const QModelIndex& Index ) const
    {
        return Index.isValid() && Index.model() == this && Index.internalPointer() == &groupTag_;
    }

    bool OutsideProxy::IsOutsideFile( const QModelIndex& Index ) const
    {
        return Index.isValid() && Index.model() == this && Index.internalPointer() == &fileTag_;
    }

    bool OutsideProxy::atRoot( const QModelIndex& Parent ) const
    {
        return groupAllowed() && !IsGroup( Parent ) && !IsOutsideFile( Parent ) && mapToSource( Parent ) == QModelIndex( root_ );
    }

    QModelIndex OutsideProxy::GroupIndex() const
    {
        if( visible_.isEmpty() || !groupAllowed() )
            return {};
        return createIndex( groupRow(), 0, const_cast< char* >( &groupTag_ ) );
    }

    QModelIndex OutsideProxy::IndexForFile( const QString& Path ) const
    {
        const QModelIndex Group = GroupIndex();
        if( Path.isEmpty() || !Group.isValid() )
            return {};
        for( int Row = 0; Row < visible_.size(); ++Row )
            if( workspace::SamePath( visible_.at( Row ), Path ) )
                return index( Row, 0, Group );
        return {};
    }

    QString OutsideProxy::FilePath( const QModelIndex& Index ) const
    {
        if( IsOutsideFile( Index ) )
            return visible_.value( Index.row() );
        if( IsGroup( Index ) || !Index.isValid() )
            return {};
        return Index.data( QFileSystemModel::FilePathRole ).toString();
    }

    QModelIndex OutsideProxy::mapToSource( const QModelIndex& Index ) const
    {
        if( IsGroup( Index ) || IsOutsideFile( Index ) )
            return {};
        return QIdentityProxyModel::mapToSource( Index );
    }

    QItemSelection OutsideProxy::mapSelectionToSource( const QItemSelection& Selection ) const
    {
        // 선택에 파일 시스템 행과 합성 묶음이 섞일 수 있다.
        return QAbstractProxyModel::mapSelectionToSource( Selection );
    }

    QModelIndex OutsideProxy::buddy( const QModelIndex& Index ) const
    {
        return IsGroup( Index ) || IsOutsideFile( Index ) ? Index : QIdentityProxyModel::buddy( Index );
    }

    QMap< int, QVariant > OutsideProxy::itemData( const QModelIndex& Index ) const
    {
        return IsGroup( Index ) || IsOutsideFile( Index ) ? QAbstractItemModel::itemData( Index ) : QIdentityProxyModel::itemData( Index );
    }

    QModelIndex OutsideProxy::index( int Row, int Column, const QModelIndex& Parent ) const
    {
        if( Row < 0 || Column < 0 || Column >= columnCount( Parent ) || Row >= rowCount( Parent ) )
            return {};
        if( IsGroup( Parent ) )
            return createIndex( Row, Column, const_cast< char* >( &fileTag_ ) );
        if( atRoot( Parent ) && !visible_.isEmpty() && Row == groupRow() )
            return createIndex( Row, Column, const_cast< char* >( &groupTag_ ) );
        return QIdentityProxyModel::index( Row, Column, Parent );
    }

    QModelIndex OutsideProxy::parent( const QModelIndex& Index ) const
    {
        if( IsGroup( Index ) )
            return mapFromSource( root_ );
        if( IsOutsideFile( Index ) )
            return GroupIndex();
        return QIdentityProxyModel::parent( Index );
    }

    QModelIndex OutsideProxy::sibling( int Row, int Column, const QModelIndex& Index ) const
    {
        return index( Row, Column, parent( Index ) );
    }

    int OutsideProxy::rowCount( const QModelIndex& Parent ) const
    {
        if( Parent.column() > 0 || IsOutsideFile( Parent ) )
            return 0;
        if( IsGroup( Parent ) )
            return int( visible_.size() );
        return QIdentityProxyModel::rowCount( Parent ) + ( atRoot( Parent ) && !visible_.isEmpty() ? 1 : 0 );
    }

    int OutsideProxy::columnCount( const QModelIndex& Parent ) const
    {
        if( IsGroup( Parent ) || IsOutsideFile( Parent ) )
            return sourceModel() ? sourceModel()->columnCount( root_ ) : 0;
        return QIdentityProxyModel::columnCount( Parent );
    }

    bool OutsideProxy::hasChildren( const QModelIndex& Parent ) const
    {
        if( IsOutsideFile( Parent ) || Parent.column() > 0 )
            return false;
        return IsGroup( Parent ) || ( atRoot( Parent ) && !visible_.isEmpty() ) || QIdentityProxyModel::hasChildren( Parent );
    }

    bool OutsideProxy::canFetchMore( const QModelIndex& Parent ) const
    {
        return !IsGroup( Parent ) && !IsOutsideFile( Parent ) && QIdentityProxyModel::canFetchMore( Parent );
    }

    void OutsideProxy::fetchMore( const QModelIndex& Parent )
    {
        if( !IsGroup( Parent ) && !IsOutsideFile( Parent ) )
            QIdentityProxyModel::fetchMore( Parent );
    }

    QVariant OutsideProxy::data( const QModelIndex& Index, int Role ) const
    {
        if( !IsGroup( Index ) && !IsOutsideFile( Index ) )
            return QIdentityProxyModel::data( Index, Role );
        if( Index.column() != 0 )
            return {};
        const bool Group = IsGroup( Index );
        switch( Role ) {
        case Qt::DisplayRole:
            return Group ? groupLabel_ : QFileInfo( FilePath( Index ) ).fileName();
        case Qt::ToolTipRole:
            return Group ? groupToolTip_ : QDir::toNativeSeparators( FilePath( Index ) );
        case Qt::DecorationRole:
            return Group ? icons_.icon( QFileIconProvider::Folder ) : icons_.icon( QFileInfo( FilePath( Index ) ) );
        case QFileSystemModel::FilePathRole:
            return Group ? QVariant{} : QVariant( FilePath( Index ) );
        case QFileSystemModel::FileNameRole:
            return Group ? QVariant( groupLabel_ ) : QVariant( QFileInfo( FilePath( Index ) ).fileName() );
        default:
            return {};
        }
    }

    bool OutsideProxy::setData( const QModelIndex& Index, const QVariant& Value, int Role )
    {
        return !IsGroup( Index ) && !IsOutsideFile( Index ) && QIdentityProxyModel::setData( Index, Value, Role );
    }

    Qt::ItemFlags OutsideProxy::flags( const QModelIndex& Index ) const
    {
        if( IsGroup( Index ) || IsOutsideFile( Index ) )
            return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        return QIdentityProxyModel::flags( Index );
    }
}
