#pragma once

struct Column
{
    int                                 Index = 0;
    int                                 Row = 0;
    QString                             Name;
    Qt::Alignment                       Align;
    QString                             Content;
};

struct ColumnView
{
    QString                             Name;

    QVector< Column >                   VecColumns;
};

class CColumnMgr
{
public:

    void                                Initialize();

    ColumnView                          GetColumnView( int Index );


private:

    QVector< ColumnView >               VecColumnViews;
};