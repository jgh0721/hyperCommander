#pragma once

// 서비스 목록 모델과 상태 필터. 상태는 ServiceMonitor의 알림으로 갱신하고, 구성(시작 유형·로그온·설명)은
// 백그라운드에서 따로 읽는다.

#include <QAbstractTableModel>
#include <QHash>
#include <QIcon>
#include <QSortFilterProxyModel>
#include <QVector>

#include "commonLibs/system/serviceControl.hpp"
#include "systemSupport.hpp"

namespace hc::sys {

struct ServiceRow
{
    hc::system::ServiceStatusInfo status;
    bool                          configLoaded = false;
    hc::system::StartType         start        = hc::system::StartType::Unknown;
    bool                          trigger      = false;
    QString                       account, description, image, binaryPath;
};

class ServiceListModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { Name, DisplayName, State, StartType, Account, Pid, Description, Type, ImagePath, ColumnCount };
    enum Role
    {
        SortRole = Qt::UserRole + 201,
        NameRole,
        StateRole,
        ProblemRole,
        SearchRole,
        StartTypeRole,
    };
    enum class Filter { All, Running, Stopped, Problem, Disabled };

    explicit ServiceListModel( QObject* Parent = nullptr );
    int      rowCount( const QModelIndex& Parent = {} ) const override;
    int      columnCount( const QModelIndex& Parent = {} ) const override;
    QVariant data( const QModelIndex& Index, int Role ) const override;
    QVariant headerData( int Section, Qt::Orientation Orientation, int Role ) const override;

    void Apply( std::vector<hc::system::ServiceStatusInfo> List );
    void ReloadConfig( const QStringList& Names = {} );
    // 구성을 읽지 못한 서비스만 다시 읽는다(관리자 도우미가 연결된 뒤).
    void ReloadMissingConfig();
    void SetSettings( const Settings& Value ) { settings_ = Value; }
    const ServiceRow* Row( int Index ) const { return Index >= 0 && Index < rows_.size() ? &rows_[Index] : nullptr; }
    int  IndexOfName( const QString& Name ) const;
    bool IsProblem( const ServiceRow& Row ) const;
    int  Count( Filter Value ) const;
    QHash<quint32, QStringList> ServicesByPid() const;

signals:
    // 한 번의 적용을 묶는다. 보기는 이 사이에 갱신을 미룬다.
    void sigBatchStarted();
    void sigBatchFinished();
    void sigCountsChanged();
    void sigConfigLoaded();

private:
    void startConfig();
    void applyConfig( const QVector<QPair<QString, hc::system::ServiceConfig>>& Configs );
    QVector<ServiceRow> rows_;
    QHash<QString, int> index_;   // 소문자 이름 → 행
    Settings            settings_;
    bool                configRunning_ = false;
    QStringList         pendingConfig_;
};

class ServiceFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit ServiceFilterProxy( QObject* Parent = nullptr );
    void SetText( const QString& Text );
    QString Text() const { return text_; }
    void SetFilter( ServiceListModel::Filter Value );
    ServiceListModel::Filter FilterValue() const { return filter_; }
    int  MatchCount() const { return rowCount(); }

protected:
    bool filterAcceptsRow( int Row, const QModelIndex& Parent ) const override;

private:
    QString                  text_;
    ServiceListModel::Filter filter_ = ServiceListModel::Filter::All;
};

} // namespace hc::sys
