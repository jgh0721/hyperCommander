#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QList>
#include <QObject>
#include <QPersistentModelIndex>
#include <QPointer>

#include <functional>

class QFileSystemModel;
class QTimer;
class QTreeView;

namespace hc::document::explorer
{
    /// 필터가 아직 읽지 않은 폴더를 볼 수 있게 범위 폴더 아래를 조금씩 읽고 펼친다.
    /// MRE MainWindow.cpp:4562-4584, 4755-4763, 5025-5112 를 떼어 냈다.
    ///
    /// * 예산 4000(폴더 하나에 두 번 쓰므로 실제로 닿는 폴더는 2000쯤). 떨어지면 멈춘다 — 결과가 모자랄 수는
    ///   있어도 앱이 멎지는 않는다.
    /// * 시간으로 끊는다(12 ms). 폴더 하나 펼치는 값이 디스크 · 개수에 따라 열 배씩 다르기 때문이다.
    /// * 안 읽은 폴더는 **읽으라고만** 한다. 먼저 펼치면 통과 못 한 폴더가 펼쳐졌다 사라지는 것이 보인다.
    ///   다 읽히면 directoryLoaded 가 그 폴더만 다시 큐에 넣는다.
    class FilterWalker final : public QObject
    {
        Q_OBJECT
    public:
        using IndexMap = std::function< QModelIndex( const QModelIndex& ) >;
        static constexpr int kBudget = 4000;
        static constexpr int kSliceMs = 12;

        /// ToView: 원본(Model) 인덱스 → 트리 뷰 인덱스, ToSource: 그 반대. 필터에 걸러진 행은 무효를 돌려준다.
        FilterWalker( QTreeView* Tree, QFileSystemModel* Model, IndexMap ToView, IndexMap ToSource, QObject* Parent = nullptr );

        /// Scope(원본 인덱스) 아래를 훑기 시작한다. 무효면 아무것도 하지 않는다(드라이브 전체를 훑지 않는다).
        void Start( const QModelIndex& Scope );
        void Stop();
        [[nodiscard]] bool IsRunning() const;
        /// 이번 훑기에서 펼쳐 본 폴더 수.
        [[nodiscard]] int Checked() const { return checked_; }

    signals:
        void Progress( int Checked );
        void Finished( int Checked );

    private:
        void queue( const QModelIndex& Source );
        void step();

        QPointer< QTreeView > tree_;
        QPointer< QFileSystemModel > model_;
        IndexMap toView_, toSource_;
        QTimer* timer_ = nullptr;
        QList< QPersistentModelIndex > queue_;
        QPersistentModelIndex scope_;
        int budget_ = 0, checked_ = 0;
        bool active_ = false;
    };
}
