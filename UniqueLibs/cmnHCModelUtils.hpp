#pragma once

#include <QtCore>

#include "cmnTypeDefs.hpp"
#include "Modules/mdlFileEngine.hpp"

class CFSModel;

class QTraverseModel : public QObject
{
    Q_OBJECT

public:
    nsHC::TySpFETaskItemSource      Items;

public slots:
    void                            Run( const QVector< QModelIndex >& Src );

signals:
    void                            notifyResultReady();

private:
    void                            traverseSubDirectory( const QString& Path, const nsHC::TySpFileSource& Item );
    void                            processFile( const QString& Parent, const nsHC::TySpFileSource& Item );

};

//class QTraverseSrcModel : public QThread
//{
//    Q_OBJECT
//
//public:
//    QVector< QModelIndex >              src;
//    QString                             base;
//    quint32                             baseLength = 0;
//    QString                             filter;
//    QRegularExpression                  regexp;
//    bool                                isRecursive = true;
//
//    QVector< nsHC::TySpFileSource >     vecItems_;
//    QVector< QString >                  vecItemMiddle_;     // Src 의 전체 경로에서 base 부분과 이름을 제외한 나머지 경로들이 들어간다.
//                                                            // 항상 \\ 로 끝난다. 
//
//    qint64                              totalSize_ = 0;
//    qint64                              fileCount_ = 0;
//    qint64                              dirCount_ = 0;
//
//    bool                                IsFinished() const;
//protected:
//    void                                run() override;
//
//signals:
//    void                                notifyStats( const QString& Item, bool IsDirectory, qint64 totalSize, qint64 fileCount, qint64 dirCount );
//    void                                notifyResultReady();
//
//private:
//
//    bool                                isFinished_ = false;
//    CFSModel*                           model_ = nullptr;
//};
//
