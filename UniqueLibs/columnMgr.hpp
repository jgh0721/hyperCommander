#pragma once

/*!
    컬럼 정의 규격

    [=모듈.분류.이름.형식]




    환경설정

    [ColumnView]


 */

struct Node;

struct ColumnParseResult
{
    QStringView                         Module;
    QStringView                         Cate;
    QStringView                         Name;
    QStringView                         Convert;
};

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
    //bool                                IsBuiltIn = false;

    QVector< Column >                   VecColumns;
};

class CColumnMgr
{
public:

    void                                Initialize();

    ColumnView                          GetColumnView( int Index );

    // true = 분석이 끝나지 않았음. 그대로 다시 호출해야 함. 
    static bool                         Parse( wchar_t*& Def, ColumnParseResult& Result, QString& Content );
    static void                         CreateColumnContent( const ColumnParseResult& Result, Node* Info, QString& Content );

private:

    static void                         builtInFsColumn( QStringView Name, QStringView Type, Node* Info, QString& Content );

    QVector< ColumnView >               VecColumnViews;
};