#pragma once

#include <QtCore>

#include "mdlFileSystem.hpp"
#include "cmnTypeDefs.hpp"
#include "column_Defs.hpp"

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

class CFsModelT : public QAbstractItemModel
{
    Q_OBJECT;
public:
    explicit CFsModelT( QObject* Parent = nullptr );

    static constexpr int USR_ROLE_SORT          = Qt::UserRole;
    static constexpr int USR_ROLE_ATTRIBUTE     = Qt::UserRole + 1;
    static constexpr int USR_ROLE_HIDDENOPACITY = Qt::UserRole + 2;

    void                            Refresh();

    void                            SetRoot( nsHC::TySpFileSystem Fs );
    void                            AppendRoot( nsHC::TySpFileSystem Fs );
    nsHC::TySpFileSystem            GetRoot() const;
    qsizetype                       GetRootCount() const { return vecFs_.count(); }

    // 드라이브 문자등을 제외한 \ 로 시작하는 경로 부분
    virtual void                    SetCurrentPath( const QString& Path );
    virtual QString                 GetCurrentPath() const;
    virtual QString                 GetCurrentPathWithRoot() const { return ""; }
    virtual QString                 GetName( const QModelIndex& Index ) const { return ""; }

    void                            ChangeDirectory( const QString& Child );
    void                            ChangeDirectory( const QModelIndex& Child );

    virtual void                    SetColumnView( const ColumnView& CV );
    virtual void                    SetColorScheme( const TyColorScheme& ColorScheme );

    //////////////////////////////////////////////////////////////////////////
    ///

    bool                            IsRoot() const;
    int                             GetFileCount() const;
    int                             GetDirectoryCount() const;
    uint64_t                        GetTotalSize() const;

    nsHC::TySpFileSource            GetFileInfo( const QModelIndex& Index ) const;
    QString                         GetFileFullPath( const QModelIndex& Index ) const;

signals:

    void                            sigRefreshCompleted();
    void                            sigChangedDirectory( const QString& CurrentPath );

public:
    //////////////////////////////////////////////////////////////////////////
    ///

    QModelIndex                     index( int row, int column, const QModelIndex& parent = QModelIndex() ) const override;
    QModelIndex                     parent( const QModelIndex& child ) const override;
    int                             rowCount( const QModelIndex& parent = QModelIndex() ) const override;
    int                             columnCount( const QModelIndex& parent = QModelIndex() ) const override;
    QVariant                        data( const QModelIndex& index, int role = Qt::DisplayRole ) const override;
    QVariant                        headerData( int section, Qt::Orientation orientation, int role = Qt::DisplayRole ) const override;
    Qt::ItemFlags                   flags( const QModelIndex& index ) const override;

protected:
    virtual void                    doRefresh() = 0;

protected:

    QString                         path_;

    ColumnView                      columnView_;
    TyColorScheme                   colorScheme_;
    QVector< QPair< TyFileSet, TyPrFGWithBG > > vecFileSetColors_;

    mutable nsCmn::nsConcurrent::CCmnMutex  lock;
    QVector< nsHC::TySpFileSystem > vecFs_;         // 어떠한 FS 에서 PACKER 를 호출하면 PACKER FS 가 추가된다.
    QVector< nsHC::TySpFileSource > vecNode_;
    QVector< TyPrFGWithBG >         vecRowColors_;

    int                             fileCount_ = 0;
    int                             directoryCount_ = 0;
    uint64_t                        totalSize_ = 0;
};
