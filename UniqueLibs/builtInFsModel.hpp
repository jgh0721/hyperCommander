#pragma once

#include "columnMgr.hpp"
#include "cmnTypeDefs.hpp"

#include "Modules/mdlFileModel.hpp"

// https://github.com/KubaO/stackoverflown/blob/master/questions/icon-proxy-39144638/main.cpp

class CFSIconGatherer : public QRunnable
{
    CFsModelT*                          model_ = nullptr;
    nsCmn::nsConcurrent::CCmnMutex*     lock_ = nullptr;
    QString                             currentPathWithRoot_;
    QVector< nsHC::TySpFileSource >&    vecNode_;

public:
    CFSIconGatherer( CFsModelT* Model, QVector< nsHC::TySpFileSource >& Nodes, 
                     const QString& CurrentPathWithRoot,
                     nsCmn::nsConcurrent::CCmnMutex* lock = nullptr )
        : model_( Model ), vecNode_( Nodes ), currentPathWithRoot_( CurrentPathWithRoot ), lock_( lock ) {}

    void                            run() override;

    void                            requestInterruption() { interrupt_.testAndSetOrdered( 0, 1 ); }
    bool                            isInterruptionRequested() const { return static_cast< int >( interrupt_ ); }
    void                            clearInterruption() { interrupt_.testAndSetOrdered( 1, 0 ); }

private:
    QAtomicInt                      interrupt_;
};

class CFSDirSizeGatherer : public QRunnable
{
    CFsModelT* model_ = nullptr;
    nsCmn::nsConcurrent::CCmnMutex*  lock_ = nullptr;
    QString                          currentPathWithRoot_;
    QVector< nsHC::TySpFileSource >& vecNode_;
    int                              sizeCol_ = -1;

public:
    CFSDirSizeGatherer( CFsModelT* Model, QVector< nsHC::TySpFileSource >& Nodes,
                     const QString& CurrentPathWithRoot, int SizeCol,
                     nsCmn::nsConcurrent::CCmnMutex* lock = nullptr )
        : model_( Model ), vecNode_( Nodes ), currentPathWithRoot_( CurrentPathWithRoot ), sizeCol_( SizeCol ), lock_( lock ) {}

    void                            run() override;
    void                            setCurrentPath( const QString& CurrentPath ) { currentPathWithRoot_ = CurrentPath; }

    void                            requestInterruption() { interrupt_.testAndSetOrdered( 0, 1 ); }
    bool                            isInterruptionRequested() const { return static_cast< int >( interrupt_ ); }
    void                            clearInterruption() { interrupt_.testAndSetOrdered( 1, 0 ); }

private:
    QAtomicInt                      interrupt_;
};

class CFSModel : public CFsModelT
{
    Q_OBJECT
public:

    ////////////////////////////////////////////////////////////////////////////
    ///

    QString                         GetCurrentPathWithRoot() const override { return retrieveRootWithPath(); }

    ////////////////////////////////////////////////////////////////////////////
    /// 파일 / 디렉토리 조작 
    
    QVector< nsHC::TySpFileSource > GetChildItems( const QString& RootWithPath, bool IncludeDotDot = false );
    
    // QString                         GetFileFullPath( const QString& Name ) const;


//    QString                             GetCurrentPath() const;
//    QString                             GetName( const QModelIndex& Index ) const;
//        
//
//    DWORD                               Rename( const QModelIndex& Index, const QString& NewName );
//    DWORD                               MakeDirectory( const QString& NewName );
//    QVector< Node >                     GetChildItems( const QString& Path );
//
    ////////////////////////////////////////////////////////////////////////////
    /// 모델
    
    void                            multiData( const QModelIndex& index, QModelRoleDataSpan roleDataSpan ) const override;

protected:
    void                            doRefresh() override;

private:
    friend class CFSIconGatherer;
    friend class CFSDirSizeGatherer;

    QString                         retrieveRootWithPath() const;
    void                            createBuiltFsValues( QVector< nsHC::TySpFileSource >& Items );
    void                            createBuiltFsValues( const nsHC::TySpFileSource& Item, int Column );

    CFSIconGatherer*                FsIconGatherer_ = nullptr;
    CFSDirSizeGatherer*             DirSizeGatherer_ = nullptr;

//    // 경로 구분자로 끝나지 않는 형태( 예). C:, \\DEVTESTER-QST 등 )
//    // Root 가 빈 문자열이라면 드라이브 목록을 표시한다. 
//    QString                             Root;
//    // 경로 구분자로 시작하는 경로, Root + CurrentPath 를 더하면 완전 경로가 나온다. 경로 구분자는 \\ 사용
//    QString                             CurrentPath;
//    ColumnView                          CurrentView;
//    QPixmap                             Icon_Directory;
//    // TODO: Mutex 등을 통해 추가로 보호해야 한다.
};

class CFSProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:

    Q_INVOKABLE void                    SetHiddenSystem( bool IsShow );
    Q_INVOKABLE bool                    GetHiddenSystem() const;

    bool                                filterAcceptsRow( int source_row, const QModelIndex& source_parent ) const override;

private:

    bool                                IsShowHiddenSystem_ = false;
};
