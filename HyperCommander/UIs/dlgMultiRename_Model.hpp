#pragma once

#include <QtCore>

/*
|-------------------------------------------|
| 원본 파일이름 | 원본 확장자 | 크기 | 날짜 |
| 대상 파일이름 | 대상 확장자 | 위치        |
|-------------------------------------------|
*/

class QMultiRenameModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    /*!
     * @brief 지정한 패턴대로 파일이름을 변경합니다. 
     * @return GetLastError()
     */
    quint32                             DoWork();

    void                                SetSourceFiles( const QVector< QString >& VecFiles );

    void                                SetRenamePattern( const QString& Name, const QString& Ext );
    // ApplyToFileExt = 0, Both
    // ApplyToFileExt = 1, FileName
    // ApplyToFileExt = 2, FileExt
    void                                SetSearchReplace( const QString& Search, const QString& Replace, bool IsCaseSensitive, bool IsOnlyOnce, int ApplyToFileExt );
    void                                ProcessPattern();

    int                                 rowCount( const QModelIndex& Parent = QModelIndex() ) const override;
    int                                 columnCount( const QModelIndex& Parent = QModelIndex() ) const override;
    QVariant                            data( const QModelIndex& Index, int Role = Qt::DisplayRole ) const override;
    QVariant                            headerData( int section, Qt::Orientation orientation, int role ) const override;

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

        // 최종 변경된 이름, Dst_Name + Dst_Ext 를 이용하여 이름을 변경하려 하였지만 이름이 겹쳐서 추가 변경이 되었을 수 있다. 
        QString                         Result;
    };

    struct Counter
    {
        int                             Group;
        int                             Start = 0;
        int                             Step = 0;
        QChar                           Char;
    };

    void                                processPattern( const QString& Pattern, const Item& Info, QString& Dst );
    void                                processFindReplace( const QString& Search, const QString& Replace, Item& Info );

    void                                processNameExt( const wchar_t*& Fmt, const QString& Src, QString& Dst );

    // Pattern
    // Name -> Ext -> Counter -> Search & Replace
    QString                             Pattern_Name;
    QString                             Pattern_Ext;
    QVector< Counter >                  Pattern_VecCounter;
    QString                             Pattern_Search;
    QString                             Pattern_Replace;
    bool                                Pattern_SR_IsUseCaseSensitive = false;
    bool                                Pattern_SR_IsUseOnlyOnce = false;
    int                                 Pattern_SR_ApplyToFileExt = 0;

    // Rows
    QVector< Item >                     vecItems;
};

