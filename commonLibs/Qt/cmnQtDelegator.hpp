#ifndef __HDR_CMN_QT_DELEGATOR__
#define __HDR_CMN_QT_DELEGATOR__

// https://medium.com/@nihil84/thread-delegation-in-qt-5e36da1f88f7#.itqwqr4ud

#include <QObject>
#include <QFuture>
#include <QFutureInterface>

class QIDelegator : public QObject
{
    Q_OBJECT;
public:
    QIDelegator()
    {
        connect( this, SIGNAL( invoke() ), SLOT( onInvokation() ) );
    }

signals:
    void                                            invoke();

private slots:
    virtual void                                    onInvokation() = 0;
};


template< typename R, typename F >
class QDelegator : public QIDelegator
{
public:
    typedef R ResultType;
    QDelegator( F fun ) : m_fun(fun) {}
    QFuture<ResultType>                             delegate( QThread* thread );

private:
    QFutureInterface<ResultType>                    m_res;
    F                                               m_fun;
    void                                            onInvokation();
};

template< typename F >
class QDelegator<void, F> : public QIDelegator
{
public:
    typedef void ResultType;
    QDelegator( F fun ) : m_fun(fun) {}
    QFuture<ResultType>                             delegate( QThread* thread );

private:
    QFutureInterface<ResultType>                    m_res;
    F                                               m_fun;
    void                                            onInvokation();
};

template< typename R, typename F >
QFuture<R> QDelegator<R, F>::delegate( QThread* thread )
{
    moveToThread( thread );
    QFuture<R> future = m_res.future();
    emit invoke();
    return future;
}

template< typename F >
QFuture<void> QDelegator<void, F>::delegate( QThread *thread )
{
    moveToThread( thread );
    QFuture<void> future = m_res.future();
    emit invoke();
    return future;
}

template< typename R, typename F >
void QDelegator<R, F>::onInvokation()
{
    m_res.reportStarted();
    R result = m_fun();
    m_res.reportFinished( &result );
    this->deleteLater();
}

template <typename F>
void QDelegator<void, F>::onInvokation()
{
    m_res.reportStarted();
    m_fun();
    m_res.reportFinished();
    this->deleteLater();
}

template< typename R >
struct QResult
{
    template< typename F >
    static QFuture<R> delegate( QThread* thread, F fun )
    {
        return (new QDelegator<R, F>(fun))->delegate(thread);
    }
};

#endif // __HDR_CMN_QT_DELEGATOR__
