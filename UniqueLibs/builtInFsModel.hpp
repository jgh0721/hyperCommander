﻿#pragma once

#include "columnMgr.hpp"

struct Node
{
    QString                             Name;
    QString                             Ext;

    DWORD                               Attiributes = 0;
    uint64_t                            Size = 0;

    QDateTime                           Created;
    QDateTime                           Modified;

    ///////////////////////////////////////////////////////////////////////////

    QPixmap                             Icon;
    QVector< QVariant >                 VecContent;
};

class FSModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    static const int USR_ROLE_ATTRIBUTE     = Qt::UserRole + 1;
    static const int USR_ROLE_HIDDENOPACITY = Qt::UserRole + 2;

    Q_INVOKABLE void                    SetRoot( const QString& Root );
    Q_INVOKABLE void                    SetCurrentPath( const QString& Path );
    Q_INVOKABLE void                    Refresh();
    QString                             GetCurrentPath() const;
    void                                SetColumnView( const ColumnView& CV );
    QString                             GetFileFullPath( const QModelIndex& Index ) const;
    Node                                GetFileInfo( const QModelIndex& Index ) const;
    bool                                IsRoot() const;

    void                                ChangeDirectory( const QString& Child );

    int                                 GetFileCount() const;
    int                                 GetDirectoryCount() const;
    int64_t                             GetTotalSize() const;

    QModelIndex                         index( int row, int column, const QModelIndex& parent = QModelIndex() ) const override;
    QModelIndex                         parent( const QModelIndex& child ) const override;
    int                                 rowCount( const QModelIndex& parent = QModelIndex() ) const override;
    int                                 columnCount( const QModelIndex& parent = QModelIndex() ) const override;
    QVariant                            data( const QModelIndex& index, int role = Qt::DisplayRole ) const override;
    QVariant                            headerData( int section, Qt::Orientation orientation, int role = Qt::DisplayRole ) const override;
    Qt::ItemFlags                       flags( const QModelIndex& index ) const override;

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

    QVector< Node >                     VecNode;
    QVector< int >                      VecSelectedRows;
};

class FSProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:

    void                                SetHiddenSystem( bool IsShow );
    bool                                GetHiddenSystem() const;

    bool                                filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

private:

    bool                                IsShowHiddenSystem_ = false;
};