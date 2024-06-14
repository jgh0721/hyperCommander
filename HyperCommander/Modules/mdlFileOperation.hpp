#pragma once

#include "cmnTypeDefs.hpp"

class CFileOperation : public QThread
{
    Q_OBJECT
public:

    enum TyEnOperation { FILE_OP_COPY, FILE_OP_MOVE, FILE_OP_DELETE };

    void                                SetMode( TyEnOperation Mode );
    void                                SetSource( const QVector< Node >& Src, const QVector< QString >& SrcFullPath );
    void                                SetDestination( const QString& Dst );
    void                                SetTotalSize( qint64 TotalSize );

signals:

    void                                NotifyChangedItem( const QString& Src, const QString& Dst );
    void                                NotifyChangedProgress( qint16 Item, qint64 Total );
    void                                NotifyChangedStatus( qint64 CurrentItemCount, qint64 CurrentTotalSize );

protected:
    void                                run() override;
    void                                workCopy();
    void                                workMove();
    void                                workDelete();

private:
    QString                             retrieveDstItem( const QString& Dst, const QString& Name );

    TyEnOperation                       mode_ = FILE_OP_COPY;

    QVector< Node >                     vecSrc_;
    QVector< QString >                  vecSrcFullPath_;
    qint64                              totalSize_ = 0;

    QString                             dst_;

};