#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCollator>
#include <QFileIconProvider>
#include <QFileSystemModel>
#include <QIdentityProxyModel>
#include <QItemSelection>
#include <QList>
#include <QPersistentModelIndex>
#include <QRegularExpression>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>

#include <functional>

namespace hc::document::explorer
{
    /// 탐색기 필터 문구 하나. `*` 나 `?` 가 있으면 대소문자를 가리지 않는 **전체 일치** 와일드카드,
    /// 없으면 대소문자를 가리지 않는 부분 일치다(`*.rst` 는 사람들이 파일 트리에 실제로 치는 형태다).
    /// MRE src/uis/FileTreeFilterProxy.cpp:74-88, 164-180. 필터 프록시와 바깥 묶음이 같은 규칙을 쓰도록 떼어 냈다.
    class NameMatcher
    {
    public:
        /// 앞뒤 공백을 걷어 낸 문구가 달라졌으면 true.
        bool                            SetText( const QString& Text );
        [[nodiscard]] const QString&    Text() const { return text_; }
        [[nodiscard]] bool              IsEmpty() const { return text_.isEmpty(); }
        [[nodiscard]] bool              IsWildcard() const { return !wildcard_.pattern().isEmpty(); }
        /// 빈 문구는 모두 맞는다. 빈 이름은 (빈 문구가 아니면) 맞지 않는다.
        [[nodiscard]] bool              Matches( const QString& Name ) const;

    private:
        QString                         text_;
        QRegularExpression              wildcard_;      // 와일드카드일 때만 채워진다
    };

    /// 탐색기의 원본 모델. MRE MainWindow.cpp:4602-4618 의 ExplorerFileSystemModel.
    ///
    /// 컴퓨터 루트(rootPath "")의 드라이브 목록은 프록시가 정렬한다. QFileSystemModel 자체의 정렬기는 드라이브마다
    /// 파일 속성을 동기로 조회하므로, 끊긴 원격 매핑 하나만 있어도 GUI 스레드가 Windows 재연결을 기다린다.
    /// 읽기 전용이다 — 이름 바꾸기 · 삭제는 모델이 아니라 컨트롤러가 저장소 계층으로 한다(설계 5.3).
    class FileSystemModel final : public QFileSystemModel
    {
    public:
        explicit FileSystemModel( QObject* Parent = nullptr );

        void                            sort( int Column, Qt::SortOrder Order = Qt::AscendingOrder ) override;

        /// OS 숨김 속성 항목을 보일지(QDir::Hidden). 제외 폴더 이름은 FilterProxy::SetShowHidden 이 따로 맡는다.
        void                            SetShowHidden( bool Show );
        [[nodiscard]] bool              ShowHidden() const;

        /// 확장자 허용 목록("*.rst" …). 비우면 모든 파일. 폴더는 늘 보인다. 맞지 않는 파일은 흐려지지 않고
        /// 트리에서 빠진다(setNameFilterDisables(false), MRE MainWindow.cpp:4589-4597).
        void                            SetExtensionFilter( const QStringList& NameFilters );

        /// 설계 5.4 의 텍스트 계열 확장자(txt md rst py json toml yaml yml ini cfg css js html xml csv log).
        /// 문서 처리기 확장자는 호출자가 더한다.
        [[nodiscard]] static QStringList TextFamilyNameFilters();
    };

    /// 탐색기 트리의 필터 · 정렬 프록시. MRE src/uis/FileTreeFilterProxy.{hpp,cpp} 를 옮기고 제외 폴더 이름,
    /// 맨 위 드라이브 제한, 고정 경로(문서 모드)를 더했다.
    ///
    /// * QFileSystemModel 위에서는 그 모델의 isDir() 로 디렉터리를 판정하고, 다른 모델은 hasChildren() 으로
    ///   되돌아가므로 규칙 전체를 파일 시스템 없이도 검증할 수 있다.
    /// * QFileSystemModel 은 게으르다. 아직 읽지 않은 디렉터리는 자식이 모델에 없어 필터가 볼 수 없다.
    ///   FilterWalker 가 필터가 켜진 동안 범위 안 폴더를 읽게 한다.
    /// * Windows 의 끊긴 원격 드라이브 루트는 필터가 비어 있어도 내보내지 않는다.
    class FilterProxy final : public QSortFilterProxyModel
    {
        Q_OBJECT

    public:
        explicit FilterProxy( QObject* Parent = nullptr );

        /// 기본 제외 폴더 이름: .git .multiroot __pycache__ .venv node_modules(설계 5.4).
        [[nodiscard]] static QStringList DefaultExcludedNames();

        /// 필터 문구. 비면 전부 통과한다. NameMatcher 규칙을 따른다.
        void                            SetFilterText( const QString& Text );
        [[nodiscard]] QString           FilterText() const { return matcher_.Text(); }
        [[nodiscard]] bool              IsFiltering() const { return !matcher_.IsEmpty(); }
        [[nodiscard]] const NameMatcher& Matcher() const { return matcher_; }
        /// 원본 인덱스의 **이름 자체**가 지금 문구에 맞는가(조상으로 남았을 뿐인 행은 false, 필터가 비어 있으면 false).
        /// 트리 대리자의 일치 강조와 필터 걷기의 일치 수 세기에 쓴다. 디스크를 읽지 않는다.
        [[nodiscard]] bool              IsMatch( const QModelIndex& SourceIndex ) const;

        /// 조상 검사를 멈출 자리(트리 뷰의 루트에 해당하는 **원본** 인덱스). 문서 모드면 무효 인덱스.
        /// 뿌리와 그 조상은 무슨 일이 있어도 남는다.
        void                            SetRootSourceIndex( const QModelIndex& Index );
        [[nodiscard]] QModelIndex       RootSourceIndex() const { return rootSourceIndex_; }

        /// 필터 범위 폴더. 워크스페이스 모드는 워크스페이스 루트, 문서 모드는 문서 폴더. 빈 문자열이면 제한하지 않는다.
        /// * 범위 폴더와 그 조상은 필터 · 제외 이름과 관계없이 늘 남는다(설계 5.7 '조상 폴더는 늘 남긴다').
        /// * 필터 문구가 있는 동안 범위 밖 행은 범위의 조상이 아니면 거른다. 드라이브 전체를 펼친 문서 모드에서
        ///   다른 곳의 읽지 않은 폴더가 남아 펼쳐지고 읽히는 일을 막는다.
        /// * 이름이 맞은 조상의 "아래는 통째로 보인다" 규칙은 이 사슬에서 멈춘다(뿌리 이름이 우연히 맞는 경우와 같은 이유).
        /// FilterWalker 도 이 범위 아래만 걷는다.
        void                            SetScope( const QString& Folder );
        [[nodiscard]] QString           Scope() const { return scope_; }
        /// 경로가 범위 폴더이거나 그 아래인가(범위가 비었으면 늘 true). 대소문자 무시, 디스크를 읽지 않는다.
        [[nodiscard]] bool              IsInScope( const QString& Path ) const;

        /// 제외 폴더 이름(대소문자 무시, 어느 깊이에서든). 같은 이름의 파일도 숨긴다(서브모듈의 `.git` 파일).
        /// 제외된 폴더의 자손은 필터 문구에 맞아도 보이지 않는다.
        void                            SetExcludedNames( const QStringList& Names );
        [[nodiscard]] QStringList       ExcludedNames() const { return excludedNames_; }
        [[nodiscard]] bool              IsExcludedName( const QString& Name ) const;

        /// '숨김 항목 표시'. 켜면 제외 폴더 이름을 적용하지 않는다(OS 숨김 속성은 FileSystemModel::SetShowHidden).
        void                            SetShowHidden( bool Show );
        [[nodiscard]] bool              ShowHidden() const { return showHidden_; }

        /// 보이지 않는 "내 PC" 루트(rootPath "") 바로 아래 드라이브 행 가운데 남길 것("D:/", "//server/share/").
        /// 비우면 모든 드라이브(끊긴 원격 드라이브 제외)를 남긴다. 문서 모드는 문서 폴더의 드라이브만 둔다(설계 5.3).
        void                            SetAllowedTopLevelRoots( const QStringList& Roots );
        [[nodiscard]] QStringList       AllowedTopLevelRoots() const { return allowedRoots_; }

        /// 원본 인덱스가 디렉터리인가(QFileSystemModel 이면 isDir, 아니면 hasChildren).
        [[nodiscard]] bool              IsDirectory( const QModelIndex& SourceIndex ) const;
        /// 원본 인덱스의 경로('/' 구분). 맨 위 드라이브 행은 캐시된 이름만 써서 끊긴 드라이브를 건드리지 않는다.
        [[nodiscard]] QString           SourcePath( const QModelIndex& SourceIndex ) const;

    signals:
        void                            FilterTextChanged( const QString& Text );

    protected:
        [[nodiscard]] bool              filterAcceptsRow( int SourceRow, const QModelIndex& SourceParent ) const override;
        [[nodiscard]] bool              lessThan( const QModelIndex& Left, const QModelIndex& Right ) const override;

    private:
        [[nodiscard]] QString           nameOf( const QModelIndex& SourceIndex ) const;
        [[nodiscard]] bool              isRootOrAncestor( const QModelIndex& SourceIndex ) const;
        [[nodiscard]] bool              isScopeChain( const QModelIndex& SourceIndex ) const;
        [[nodiscard]] bool              isOutsideScope( const QModelIndex& SourceIndex ) const;
        [[nodiscard]] bool              isInsideExcluded( const QModelIndex& SourceIndex ) const;
        [[nodiscard]] bool              isAllowedTopLevel( const QModelIndex& SourceIndex ) const;
        [[nodiscard]] bool              isDisconnectedRemoteDrive( const QModelIndex& SourceIndex ) const;
        [[nodiscard]] bool              excludesAnything() const { return !showHidden_ && !excludedKeys_.isEmpty(); }
        void                            changeFilter( const std::function< void() >& Change );

        NameMatcher                     matcher_;
        QPersistentModelIndex           rootSourceIndex_;
        QString                         scope_;
        QString                         scopeKey_;          // 소문자, '/' 구분, 끝 '/' 없음(드라이브 루트는 "d:/")
        QSet< QString >                 scopeChainKeys_;    // 범위 폴더와 그 조상
        QStringList                     excludedNames_;
        QSet< QString >                 excludedKeys_;      // 소문자
        bool                            showHidden_ = false;
        QStringList                     allowedRoots_;
        QStringList                     allowedKeys_;       // 소문자, 끝 '/' 없음
        /// "file2" 가 "file10" 앞에 오게 한다. 만드는 비용이 있어 하나만 둔다.
        QCollator                       collator_;
    };

    /// 표시 루트의 자식 맨 끝에 합성 묶음 행 하나를 붙이는 프록시. MRE src/uis/ExternalFilesProxy.{hpp,cpp}.
    ///
    /// MRE 와 다른 점(설계 5.3):
    /// * 묶음의 파일은 호출자가 SetFiles 로 **통째로** 준다(지금 열린 문서). MRE 는 더하기만 했다.
    ///   루트 안 파일 · 빈 경로 · 중복(대소문자 무시)은 프록시가 뺀다. 그래서 열린 문서 전부를 넘겨도 된다.
    /// * 필터 문구를 묶음에도 건다. 원본이 FilterProxy 면 그 문구를 저절로 따른다. 맞는 파일이 없으면 묶음이 사라진다.
    /// * 표시 루트가 무효 인덱스("내 PC", 문서 모드)여도 맨 위 단 끝에 묶음을 붙인다.
    ///
    /// 묶음 안 순서는 정렬하지 않는다(MRE 그대로). 남은 파일은 자리를 지키고 새 파일은 끝에 붙는다.
    class OutsideProxy final : public QIdentityProxyModel
    {
        Q_OBJECT

    public:
        explicit OutsideProxy( QObject* Parent = nullptr );

        void                            setSourceModel( QAbstractItemModel* Source ) override;

        /// RootPath: 워크스페이스 루트(문서 모드면 빈 문자열). SourceRoot: **원본 모델**(이 프록시의 source)에서
        /// 표시 루트의 인덱스(문서 모드면 무효). 모델을 다시 세우고 받아 둔 파일을 새 루트로 다시 가른다.
        void                            SetRoot( const QString& RootPath, const QModelIndex& SourceRoot );
        [[nodiscard]] QString           RootPath() const { return rootPath_; }
        [[nodiscard]] QModelIndex       SourceRoot() const { return root_; }

        /// 묶음 파일을 통째로 바꾼다. 묶음에 든 파일 수(필터 전)를 돌려준다.
        int                             SetFiles( const QStringList& Paths );
        /// SetFiles 로 받은 것 가운데 루트 밖이라 묶음 대상이 된 파일(정규화된 절대 경로).
        [[nodiscard]] QStringList       Files() const { return files_; }
        /// 필터를 거쳐 지금 묶음 행으로 보이는 파일.
        [[nodiscard]] QStringList       VisibleFiles() const { return visible_; }

        void                            SetFilterText( const QString& Text );
        [[nodiscard]] QString           FilterText() const { return matcher_.Text(); }

        /// 묶음 이름과 말풍선. 기본은 '워크스페이스 밖'.
        void                            SetGroupLabel( const QString& Label );
        void                            SetGroupToolTip( const QString& ToolTip );
        [[nodiscard]] QString           GroupLabel() const { return groupLabel_; }

        /// 묶음 행. 보이는 파일이 없거나 루트를 잃었으면 무효.
        [[nodiscard]] QModelIndex       GroupIndex() const;
        [[nodiscard]] QModelIndex       IndexForFile( const QString& Path ) const;
        [[nodiscard]] bool              IsGroup( const QModelIndex& Index ) const;
        [[nodiscard]] bool              IsOutsideFile( const QModelIndex& Index ) const;
        /// 바깥 파일이면 그 경로, 묶음이면 빈 문자열, 그 밖은 원본의 QFileSystemModel::FilePathRole.
        [[nodiscard]] QString           FilePath( const QModelIndex& Index ) const;

        QModelIndex                     mapToSource( const QModelIndex& Index ) const override;
        QItemSelection                  mapSelectionToSource( const QItemSelection& Selection ) const override;
        QModelIndex                     buddy( const QModelIndex& Index ) const override;
        QMap< int, QVariant >           itemData( const QModelIndex& Index ) const override;
        QModelIndex                     index( int Row, int Column, const QModelIndex& Parent = {} ) const override;
        QModelIndex                     parent( const QModelIndex& Index ) const override;
        QModelIndex                     sibling( int Row, int Column, const QModelIndex& Index ) const override;
        int                             rowCount( const QModelIndex& Parent = {} ) const override;
        int                             columnCount( const QModelIndex& Parent = {} ) const override;
        bool                            hasChildren( const QModelIndex& Parent = {} ) const override;
        bool                            canFetchMore( const QModelIndex& Parent ) const override;
        void                            fetchMore( const QModelIndex& Parent ) override;
        QVariant                        data( const QModelIndex& Index, int Role = Qt::DisplayRole ) const override;
        bool                            setData( const QModelIndex& Index, const QVariant& Value, int Role = Qt::EditRole ) override;
        Qt::ItemFlags                   flags( const QModelIndex& Index ) const override;

    private:
        [[nodiscard]] bool              groupAllowed() const;
        [[nodiscard]] bool              atRoot( const QModelIndex& Parent ) const;
        [[nodiscard]] int               groupRow() const;
        void                            applyVisible( const QStringList& Wanted );
        [[nodiscard]] QStringList       classify() const;
        [[nodiscard]] QStringList       filtered( const QStringList& Files ) const;

        QString                         rootPath_;
        QPersistentModelIndex           root_;
        QStringList                     candidates_;    // SetFiles 로 받은 것(정규화, 중복 제거)
        QStringList                     files_;         // 그 가운데 루트 밖
        QStringList                     visible_;       // 필터를 거친 것 = 묶음 행
        NameMatcher                     matcher_;
        QString                         groupLabel_;
        QString                         groupToolTip_;
        QFileIconProvider               icons_;
        char                            groupTag_ = 0;
        char                            fileTag_ = 0;
        QList< QMetaObject::Connection > connections_;
        QModelIndexList                 layoutIndexes_;
        QList< QPersistentModelIndex >  layoutSources_;
    };
}
