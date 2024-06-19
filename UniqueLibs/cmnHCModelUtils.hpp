#pragma once

#include <QtCore>

#include "cmnTypeDefs.hpp"

class FSModel;

class QTraverseSrcModel : public QThread
{
    Q_OBJECT

public:
    QVector< QModelIndex >              src;
    QString                             base;
    quint32                             baseLength = 0;
    QString                             filter;
    QRegularExpression                  regexp;
    bool                                isRecursive = true;

    QVector< Node >                     vecItems_;
    QVector< QString >                  vecItemMiddle_;     // Src 의 전체 경로에서 base 부분과 이름을 제외한 나머지 경로들이 들어간다.
                                                            // 항상 \\ 로 끝난다. 

    qint64                              totalSize_ = 0;
    qint64                              fileCount_ = 0;
    qint64                              dirCount_ = 0;

    bool                                IsFinished() const;
protected:
    void                                run() override;

signals:
    void                                notifyStats( const QString& Item, bool IsDirectory, qint64 totalSize, qint64 fileCount, qint64 dirCount );
    void                                notifyResultReady();

private:
    void                                traverseSubDirectory( const QString& Path, const Node& Item );
    void                                processFile( const QString& Parent, const Node& Item );

    bool                                isFinished_ = false;
    FSModel*                            model_ = nullptr;
};
