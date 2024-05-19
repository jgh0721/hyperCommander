#pragma once

#include <QtCore>

/*
|-------------------------------------------|
| 원본 파일이름 | 원본 확장자 | 크기 | 날짜 |
| 대상 파일이름 | 대상 확장자               |
|-------------------------------------------|
*/

class QMultiRenameModel : public QAbstractTableModel
{
    Q_OBJECT
public:

    int                                 rowCount( const QModelIndex& Parent = QModelIndex() ) const override;
    int                                 columnCount( const QModelIndex& Parent = QModelIndex() ) const override;
    QVariant                            data( const QModelIndex& Index, int Role = Qt::DisplayRole ) const override;

private:

};

