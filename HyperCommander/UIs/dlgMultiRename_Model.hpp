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
    void                                SetSourceFiles( const QVector< QString >& VecFiles );

    void                                SetRenamePattern( const QString& Name, const QString& Ext, const QString& Search, const QString& Replace );
    void                                ProcessPattern();

    int                                 rowCount( const QModelIndex& Parent = QModelIndex() ) const override;
    int                                 columnCount( const QModelIndex& Parent = QModelIndex() ) const override;
    QVariant                            data( const QModelIndex& Index, int Role = Qt::DisplayRole ) const override;

private:

    struct Item
    {
        QString                         Src;
        QString                         Src_Name;
        QString                         Src_Ext;
        qint64                          Src_Size = -1;
        QDateTime                       Src_Created;
        QDateTime                       Src_Modified;

        QString                         Dst_Name;
        QString                         Dst_Ext;
    };

    // Pattern
    // Name -> Ext -> Counter -> Search & Replace
    QString                             Pattern_Name;
    QString                             Pattern_Ext;
    QString                             Pattern_Search;
    QString                             Pattern_Replace;

    struct Counter
    {
        int                             Group;
        int                             Start = 0;
        int                             Step = 0;
        QChar                           Char;
    };

    // Rows
    QVector< Item >                     vecItems;
};

