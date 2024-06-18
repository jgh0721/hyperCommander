#pragma once

#include "columnMgr.hpp"
#include "cmnTypeDefs.hpp"

// https://github.com/KubaO/stackoverflown/blob/master/questions/icon-proxy-39144638/main.cpp

/*!
    FSModel -> Initialize 
        DriveIocn
        DirectoryIcon
        FileIcon

    FSModel -> Refresh
        현재 경로가 Fs 인지 Packer 인지 확인
            Fs, 
                FindFirstFileEx 를 이용하여 디렉토리 검색
                스레드 : Everything 을 이용한 폴더 크기 계산
                스레드 : 아이콘 획득
                스레드 : 컨텐츠 계산
            Packer,
                스레드 : 컨텐츠 계산

    Root => X: 또는 \, UNC 경로는 루트를 \ 로 사용한다.
    Path => Root 와 항목 사이의 경로, 구분자로 \ 를 사용한다.
    Name => 항목의 이름
    FullPath => Root + Path + Name

    필요 데이터
        ColumnView
        ColorScheme
        FileSet
        PackerPluginMgr

    메소드
        SetRoot
        SetColorScheme
        GetItemInfo
        Refresh
        GetChildItems

        Rename
        MkDir
        ChDir
        Delete

    시그널
        sigChangedDirectory( const QString& Path )


    // Fs -> Packer 등으로 들어갈 때 복귀할 경로를 저장한다. 
    QStack< Parent > 예). C:\ => C:\Downloads\aaa.zip => C:\temp\_HC.zip( aaa.zip 에서 압축 푼 파일이 다시 압축 파일 )
 */

class FSModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    static constexpr int USR_ROLE_SORT          = Qt::UserRole;
    static constexpr int USR_ROLE_ATTRIBUTE     = Qt::UserRole + 1;
    static constexpr int USR_ROLE_HIDDENOPACITY = Qt::UserRole + 2;

    ////////////////////////////////////////////////////////////////////////////
    ///
    
    Q_INVOKABLE void                    SetRoot( const QString& Root );
    Q_INVOKABLE void                    SetCurrentPath( const QString& Path );
    Q_INVOKABLE void                    Refresh();
    QString                             GetCurrentPath() const;
    void                                SetColumnView( const ColumnView& CV );
    QString                             GetName( const QModelIndex& Index ) const;
    QString                             GetFileFullPath( const QModelIndex& Index ) const;
    QString                             GetFileFullPath( const QString& Name ) const;
    Node                                GetFileInfo( const QModelIndex& Index ) const;
    bool                                IsRoot() const;

    ////////////////////////////////////////////////////////////////////////////
    /// 파일 / 디렉토리 조작 

    void                                ChangeDirectory( const QString& Child );
    void                                ChangeDirectory( const QModelIndex& Child);

    int                                 GetFileCount() const;
    int                                 GetDirectoryCount() const;
    int64_t                             GetTotalSize() const;

    DWORD                               Rename( const QModelIndex& Index, const QString& NewName );
    DWORD                               MakeDirectory( const QString& NewName );
    QVector< Node >                     GetChildItems( const QString& Path );

    ////////////////////////////////////////////////////////////////////////////
    /// 모델

    QModelIndex                         index( int row, int column, const QModelIndex& parent = QModelIndex() ) const override;
    QModelIndex                         parent( const QModelIndex& child ) const override;
    int                                 rowCount( const QModelIndex& parent = QModelIndex() ) const override;
    int                                 columnCount( const QModelIndex& parent = QModelIndex() ) const override;
    QVariant                            data( const QModelIndex& index, int role = Qt::DisplayRole ) const override;
    bool                                setData(const QModelIndex& index, const QVariant& value, int role) override;
    QVariant                            headerData( int section, Qt::Orientation orientation, int role = Qt::DisplayRole ) const override;
    Qt::ItemFlags                       flags( const QModelIndex& index ) const override;
    void                                multiData(const QModelIndex& index, QModelRoleDataSpan roleDataSpan) const override;
signals:

    void                                sigChangedDirectory( const QString& CurrentPath );

private:

    // 경로 구분자로 끝나지 않는 형태( 예). C:, \\DEVTESTER-QST 등 )
    // Root 가 빈 문자열이라면 드라이브 목록을 표시한다. 
    QString                             Root;
    // 경로 구분자로 시작하는 경로, Root + CurrentPath 를 더하면 완전 경로가 나온다. 경로 구분자는 \\ 사용
    QString                             CurrentPath;
    ColumnView                          CurrentView;
    QPixmap                             Icon_Directory;

    int                                 FileCount = -1;
    int                                 DirectoryCount = -1;
    int64_t                             TotalSize = 0;

    // TODO: Mutex 등을 통해 추가로 보호해야 한다.

    QVector< Node >                     VecNode;
    QVector< TyPrFBWithBG >             VecRowColors;
};

class FSProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:

    Q_INVOKABLE void                    SetHiddenSystem( bool IsShow );
    Q_INVOKABLE bool                    GetHiddenSystem() const;

    bool                                filterAcceptsRow( int source_row, const QModelIndex& source_parent ) const override;

private:

    bool                                IsShowHiddenSystem_ = false;
};