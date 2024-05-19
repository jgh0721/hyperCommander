#pragma once

#if !defined(USE_QT_SUPPORT)
#error This Feature Require USE_QT_SUPPORT!!
#endif

#include <QtCore>
#include <memory>

#define QT_GENERIC_ARGS QGenericArgument val0, QGenericArgument val1, QGenericArgument val2, QGenericArgument val3, QGenericArgument val4, QGenericArgument val5, QGenericArgument val6, QGenericArgument val7, QGenericArgument val8, QGenericArgument val9
#define QT_GENERIC_ARGS_10 QGenericArgument val0, QGenericArgument val1 = QGenericArgument(), QGenericArgument val2 = QGenericArgument(), QGenericArgument val3 = QGenericArgument(), QGenericArgument val4 = QGenericArgument(), QGenericArgument val5 = QGenericArgument(), QGenericArgument val6 = QGenericArgument(), QGenericArgument val7 = QGenericArgument(), QGenericArgument val8 = QGenericArgument(), QGenericArgument val9 = QGenericArgument()
#define QT_GENERIC_ARGS_10_DEFAULT QGenericArgument val0 = QGenericArgument(), QGenericArgument val1 = QGenericArgument(), QGenericArgument val2 = QGenericArgument(), QGenericArgument val3 = QGenericArgument(), QGenericArgument val4 = QGenericArgument(), QGenericArgument val5 = QGenericArgument(), QGenericArgument val6 = QGenericArgument(), QGenericArgument val7 = QGenericArgument(), QGenericArgument val8 = QGenericArgument(), QGenericArgument val9 = QGenericArgument()

/// 비동기적으로 QT 의 메소드를 호출하기 위한 정보들을 보관하는 클래스
/// 지정한 인자들은 모두 복사된다. 
class CInvokeInfo
{
public:
    CInvokeInfo();
    CInvokeInfo( const QString& sMethodName, const QByteArray& sMethodSignature, QObject* object = Q_NULLPTR );
    CInvokeInfo( const QString& sMethodName, QObject* object = Q_NULLPTR, QT_GENERIC_ARGS_10_DEFAULT );

    ~CInvokeInfo();

    FORCEINLINE QString                 methodName() const { return _sMethodName; }
    FORCEINLINE QByteArray              methodSignature() const { return _sMethodignature; }

    // 지정한 오브젝트에서 호출하려는 메소드를 찾아 메소드 색인번호를 반환합니다. 성공 : 0 이상, 실패 : 음수
    static int                          methodIndex( QObject* object, const char* methodSig );
    // 지정한 객체에서, 인자로 설정한 메소드 호출이 가능한지 확인합니다.
    static bool                         checkMethodArgs( QObject* object, const QString& sMethodName, const QByteArray& baMethodSig );

    // 반환값을 포함한 인자 수를 반환한다. 
    FORCEINLINE int                     argCount() const { return _argCount; }
    // 반환값을 포함한 인자 형식에 대한 배열( QMetaType ), 반환값 : 색인번호 0
    FORCEINLINE int*                    argTypes() const { return _argTypes; }
    // 반환값을 포함한 인자 값에 대한 포인터, 반환값 : 색인번호 0
    FORCEINLINE void**                  args() const { return _args; }

    // invoke 를 호출하는 스레드의 문맥에서 호출된다. ( qt_metacall 을 이용하여 직접 호출함 ) 
    // 기존에 지정한 대상 객체는 무시되고 지정한 객체에 대해 호출합니다. 
    int                                 invoke( QObject* object );
    // 생성할 때 지정한 객체를 호출합니다.
    int                                 invoke();

private:
    Q_DISABLE_COPY( CInvokeInfo );

    QObject* _object;
    QString                                         _sMethodName;
    QByteArray                                      _sMethodignature;

    int                                             _argCount;
    // 인자의 형식을 담은 포인터 배열
    int* _argTypes;
    void** _args;
    // 호출가능한지 확인, < 0 : 호출 불가, 0 : 검사 필요, > 0 : 호출 가능
    int                                             _isAbleToCall;
};

typedef QSharedPointer< CInvokeInfo >               TySpInvokeInfo;

class CInvokeInfoWithResult : public CInvokeInfo
{
public:
    CInvokeInfoWithResult();
    CInvokeInfoWithResult( const QString& sMethodName, QObject* object = Q_NULLPTR, 
                           QGenericReturnArgument ret = QGenericReturnArgument(), QT_GENERIC_ARGS_10_DEFAULT );

private:
    Q_DISABLE_COPY( CInvokeInfoWithResult )
};

namespace nsCmn
{
    namespace nsCmnQt
    {
        class QObjectPtr
        {
        public:
            QObjectPtr( const QObjectPtr& objPtr )
                : _obj( objPtr._obj )
            {

            }

            QObjectPtr( QObject* obj )
                : _obj( obj )
            {
                Q_ASSERT( obj != nullptr );
            }

            template< typename T >
            QObjectPtr( QSharedPointer<T> spObj )
                : _obj( Q_NULLPTR )
            {
                Q_ASSERT( spObj != nullptr );
                _obj = spObj.data();
            }

            QObject* data() const
            {
                return _obj.data();
            }

        private:
            QPointer<QObject>           _obj;
        };

        bool                            MarshalTo( QGenericArgument arg, QByteArray& ba );
        // 성공하면 arg 에서 name 포인터를 반드시 free 해야한다. 
        bool                            DemarshalFrom( const QString& sArgName, const QByteArray& ba, QGenericArgument& arg );

        // QMetaObject::invokeMethod 를 호출한다. 호출이 실패하면 로그 출력과 함께 Q_ASSERT 가 발동된다. 
        bool                            InvokeMethod( QObjectPtr obj, const char* member, QGenericArgument val0 = QGenericArgument( Q_NULLPTR ), QGenericArgument val1 = QGenericArgument(), QGenericArgument val2 = QGenericArgument(), QGenericArgument val3 = QGenericArgument(), QGenericArgument val4 = QGenericArgument(), QGenericArgument val5 = QGenericArgument(), QGenericArgument val6 = QGenericArgument(), QGenericArgument val7 = QGenericArgument(), QGenericArgument val8 = QGenericArgument(), QGenericArgument val9 = QGenericArgument() );
        // QMetaObject::invokeMethod 를 호출한다. 호출이 실패하면 로그 출력과 함께 Q_ASSERT 가 발동된다. 
        bool                            InvokeMethod( QObjectPtr obj, const char* member, Qt::ConnectionType type, QGenericArgument val0 = QGenericArgument( Q_NULLPTR ), QGenericArgument val1 = QGenericArgument(), QGenericArgument val2 = QGenericArgument(), QGenericArgument val3 = QGenericArgument(), QGenericArgument val4 = QGenericArgument(), QGenericArgument val5 = QGenericArgument(), QGenericArgument val6 = QGenericArgument(), QGenericArgument val7 = QGenericArgument(), QGenericArgument val8 = QGenericArgument(), QGenericArgument val9 = QGenericArgument() );
        // QMetaObject::invokeMethod 를 호출한다. 호출이 실패하면 로그 출력과 함께 Q_ASSERT 가 발동된다. 
        bool                            InvokeMethod( QObjectPtr obj, const char* member, QGenericReturnArgument ret, QGenericArgument val0 = QGenericArgument( Q_NULLPTR ), QGenericArgument val1 = QGenericArgument(), QGenericArgument val2 = QGenericArgument(), QGenericArgument val3 = QGenericArgument(), QGenericArgument val4 = QGenericArgument(), QGenericArgument val5 = QGenericArgument(), QGenericArgument val6 = QGenericArgument(), QGenericArgument val7 = QGenericArgument(), QGenericArgument val8 = QGenericArgument(), QGenericArgument val9 = QGenericArgument() );
        // QMetaObject::invokeMethod 를 호출한다. 호출이 실패하면 로그 출력과 함께 Q_ASSERT 가 발동된다. 
        bool                            InvokeMethod( QObjectPtr obj, const char* member, Qt::ConnectionType type, QGenericReturnArgument ret, QGenericArgument val0 = QGenericArgument( Q_NULLPTR ), QGenericArgument val1 = QGenericArgument(), QGenericArgument val2 = QGenericArgument(), QGenericArgument val3 = QGenericArgument(), QGenericArgument val4 = QGenericArgument(), QGenericArgument val5 = QGenericArgument(), QGenericArgument val6 = QGenericArgument(), QGenericArgument val7 = QGenericArgument(), QGenericArgument val8 = QGenericArgument(), QGenericArgument val9 = QGenericArgument() );

    } // nsCmnQt
} // nsCmn