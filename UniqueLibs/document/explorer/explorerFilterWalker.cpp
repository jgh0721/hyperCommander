#include "stdafx.h"
#include "explorerFilterWalker.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QElapsedTimer>
#include <QFileSystemModel>
#include <QTimer>
#include <QTreeView>

namespace hc::document::explorer
{
    FilterWalker::FilterWalker( QTreeView* Tree, QFileSystemModel* Model, IndexMap ToView, IndexMap ToSource, QObject* Parent )
        : QObject( Parent ), tree_( Tree ), model_( Model ), toView_( std::move( ToView ) ), toSource_( std::move( ToSource ) )
    {
        // 간격 0 은 "밀린 이벤트를 다 처리한 뒤" — 훑는 동안에도 그림이 그려지고 키가 들어온다.
        timer_ = new QTimer( this );
        timer_->setSingleShot( true );
        timer_->setInterval( 0 );
        connect( timer_, &QTimer::timeout, this, &FilterWalker::step );
        // 뒤늦게 읽힌 폴더는 **그 폴더만** 다시 본다. 트리 전체를 처음부터 다시 훑으면 큰 폴더에서 읽힐 때마다
        // 그 일을 되풀이하다 멎는다(MRE MainWindow.cpp:4755-4763).
        if( model_ )
            connect( model_, &QFileSystemModel::directoryLoaded, this, [ this ]( const QString& Path ) {
                if( !active_ || !model_ )
                    return;
                const QModelIndex Loaded = model_->index( Path );
                // 범위 밖에서 읽힌 폴더(사용자가 다른 곳을 펼친 경우)는 걷지 않는다.
                for( QModelIndex Node = Loaded; Node.isValid(); Node = Node.parent() )
                    if( Node == QModelIndex( scope_ ) ) { queue( Loaded ); return; }
            } );
    }

    void FilterWalker::Start( const QModelIndex& Scope )
    {
        Stop();
        if( !Scope.isValid() || !tree_ || !model_ )
            return;
        active_ = true;
        scope_ = Scope;
        budget_ = kBudget;
        checked_ = 0;
        queue( Scope );
    }

    void FilterWalker::Stop()
    {
        const bool Was = active_;
        active_ = false;
        timer_->stop();
        queue_.clear();
        budget_ = 0;
        if( Was )
            emit Finished( checked_ );
    }

    bool FilterWalker::IsRunning() const
    {
        return active_ && !queue_.isEmpty();
    }

    void FilterWalker::queue( const QModelIndex& Source )
    {
        if( !Source.isValid() || budget_ <= 0 )
            return;
        --budget_;
        queue_.append( QPersistentModelIndex( Source ) );
        timer_->start();
    }

    void FilterWalker::step()
    {
        if( !active_ || !tree_ || !model_ ) { Stop(); return; }
        const QModelIndex TreeRoot = tree_->rootIndex();
        QElapsedTimer Slice; Slice.start();
        while( !queue_.isEmpty() && Slice.elapsed() < kSliceMs ) {
            const QModelIndex Source = queue_.takeFirst();
            if( !Source.isValid() )
                continue;
            if( model_->canFetchMore( Source ) ) {
                // 읽으라고만 한다. 다 읽히면 directoryLoaded 가 다시 넣는다.
                model_->fetchMore( Source );
                continue;
            }
            const QModelIndex Parent = toView_( Source );
            if( !Parent.isValid() )
                continue;   // 그새 필터에 걸러졌다
            ++checked_;
            if( Parent != TreeRoot )
                tree_->expand( Parent );
            auto* View = tree_->model();
            const int Rows = View->rowCount( Parent );
            for( int Row = 0; Row < Rows; ++Row ) {
                const QModelIndex Child = View->index( Row, 0, Parent );
                const QModelIndex ChildSource = toSource_( Child );
                // 폴더만 내려간다. 합성 행(바깥 묶음)은 원본이 없어 여기서 빠진다.
                if( !ChildSource.isValid() || !model_->isDir( ChildSource ) )
                    continue;
                queue( ChildSource );
            }
        }
        emit Progress( checked_ );
        if( !queue_.isEmpty() )
            timer_->start();
    }
}
