#include "stdafx.h"
#include "cmnQtUtils.hpp"

//////////////////////////////////////////////////////////////////////////

CInvokeInfo::CInvokeInfo()
    : _object( Q_NULLPTR )
    , _argCount( 0 )
    , _argTypes( Q_NULLPTR )
    , _args( Q_NULLPTR )
    , _isAbleToCall( 0 )
{
}

CInvokeInfo::CInvokeInfo( const QString& sMethodName, const QByteArray& sMethodSignature, QObject* object /*= Q_NULLPTR */ )
    : _object( object )
    , _sMethodName( sMethodName )
    , _sMethodignature( sMethodSignature )
    , _argCount( 0 )
    , _argTypes( Q_NULLPTR )
    , _args( Q_NULLPTR )
    , _isAbleToCall( 0 )
{
    Q_ASSERT( sMethodName.isEmpty() == false );
    Q_ASSERT( sMethodSignature.isEmpty() == false );
}

CInvokeInfo::CInvokeInfo( const QString& sMethodName, QObject* object /* = Q_NULLPTR */, QGenericArgument val0 /* = QGenericArgument() */, QGenericArgument val1 /* = QGenericArgument() */, QGenericArgument val2 /* = QGenericArgument() */, QGenericArgument val3 /* = QGenericArgument() */, QGenericArgument val4 /* = QGenericArgument() */, QGenericArgument val5 /* = QGenericArgument() */, QGenericArgument val6 /* = QGenericArgument() */, QGenericArgument val7 /* = QGenericArgument() */, QGenericArgument val8 /* = QGenericArgument() */, QGenericArgument val9 /* = QGenericArgument() */ )
    : _object( object )
    , _sMethodName( sMethodName )
    , _argCount( 0 )
    , _argTypes( Q_NULLPTR )
    , _args( Q_NULLPTR )
    , _isAbleToCall( 0 )
{
    Q_ASSERT( sMethodName.isEmpty() == false );

    // based on QMetaMethod::invoke
    QVarLengthArray< char, 512 > sig;
    QGenericReturnArgument ret = QGenericReturnArgument();

    // 입력하지 않은 인자에 대해서는 널 포인터를 반환한다.
    const char* typeNames[] = {
        ret.name(),
        val0.name(),
        val1.name(),
        val2.name(),
        val3.name(),
        val4.name(),
        val5.name(),
        val6.name(),
        val7.name(),
        val8.name(),
        val9.name(),
    };

    void *param[] = {
        ret.data(),
        val0.data(),
        val1.data(),
        val2.data(),
        val3.data(),
        val4.data(),
        val5.data(),
        val6.data(),
        val7.data(),
        val8.data(),
        val9.data()
    };

    // 호출 메소드 서명 생성 및 호출 인자 수량 파악
    sig.append( _sMethodName.toLatin1(), _sMethodName.toLatin1().size() );
    sig.append( '(' );

    _argCount = 1; // 반환값을 포함하여 최소 1개

    while( typeNames[ _argCount ] != Q_NULLPTR )
    {
        size_t len = qstrlen( typeNames[ _argCount ] );
        if( len <= 0 )
            break;

        sig.append( typeNames[ _argCount ], len );
        sig.append( ',' );

        ++_argCount;
    }

    if( _argCount == 1 )
        sig.append( ')' );
    else
        sig[ sig.size() - 1 ] = ')';    // 인자가 있다면 앞의 while 문에 의해 마지막이 "," 로 끝났을 것이므로 "," 를 지우고 ")" 를 넣는다
    sig.append( '\0' );

    _sMethodignature = sig.constData();
    Q_ASSERT( _argCount >= 1 );

    if( object != Q_NULLPTR )
    {
        _isAbleToCall = checkMethodArgs( object, sMethodName, methodSignature() ) ? 1 : -1;
        if( _isAbleToCall < 0 )
        {
#if defined(USE_LOGGING_IN_CMNLIBS)
            LM_ERROR_TO( logger, L"지정한 객체에 대해 메소드 호출 불가|{}|{}", object->objectName(), methodSignature().constData() );
#endif // #if defined(USE_LOGGING_IN_CMNLIBS)
            Q_ASSERT( false );
        }
    }

    _args = (void**)malloc( _argCount * sizeof( void* ) );
    Q_CHECK_PTR( _args );
    memset( _args, '\0', _argCount * sizeof( void* ) );

    _argTypes = (int*)malloc( _argCount * sizeof( int ) );
    Q_CHECK_PTR( _argTypes );
    memset( _argTypes, '\0', _argCount * sizeof( int ) );
    _argTypes[ 0 ] = 0; // 반환값 형식
    _args[ 0 ] = 0;

    int nargs = 0;

    for( int i = 1; i < _argCount; ++i )
    {
        _argTypes[ i ] = QMetaType::type( typeNames[ i ] );
        if( _argTypes[ i ] != QMetaType::UnknownType )
        {
            _args[ i ] = QMetaType::create( _argTypes[ i ], param[ i ] );
            ++nargs;
        }
        else
        {
            for( int x = 0; x < _argCount; ++x )
            {
                if( _argTypes[ x ] && _args[ x ] )
                    QMetaType::destroy( _argTypes[ x ], _args[ x ] );
            }

            free( _args );
            free( _argTypes );
            _argTypes = Q_NULLPTR;
            _args = Q_NULLPTR;

#if defined(USE_LOGGING_IN_CMNLIBS)
            LM_ERROR_TO( logger, L"등록되지 않은 형식으로 호출정보를 구성하려 시도함" );
#endif // #if defined(USE_LOGGING_IN_CMNLIBS)
            Q_ASSERT( false );
            break;
        }
    }
}

CInvokeInfo::~CInvokeInfo()
{
    if( _argTypes != Q_NULLPTR )
    {
        for( int i = 0; i < _argCount; ++i )
        {
            if( _argTypes[ i ] && _args[ i ] )
                QMetaType::destroy( _argTypes[ i ], _args[ i ] );
        }

        free( _argTypes );
        free( _args );
        _argTypes = Q_NULLPTR;
        _args = Q_NULLPTR;
    }
}

int CInvokeInfo::methodIndex( QObject* object, const char* methodSig )
{
    Q_ASSERT( object != Q_NULLPTR );
    Q_ASSERT( methodSig != Q_NULLPTR && qstrlen( methodSig ) > 0 );
    int methodIndex = -1;

    const QMetaObject* meta = object->metaObject();
    methodIndex = meta->indexOfMethod( methodSig );

    if( methodIndex < 0 )
    {
        QByteArray norm = QMetaObject::normalizedSignature( methodSig );
        methodIndex = meta->indexOfMethod( norm.constData() );
    }

    return methodIndex;
}

bool CInvokeInfo::checkMethodArgs( QObject* object, const QString& sMethodName, const QByteArray& baMethodSig )
{
    bool isAbleToCall = false;

    do
    {
        Q_CHECK_PTR( object );
        if( object == Q_NULLPTR )
            break;

        const QMetaObject* meta = object->metaObject();
        Q_CHECK_PTR( meta );

        //////////////////////////////////////////////////////////////////////////
        /// 호출할 객체명의 메소드 이름 추출,
        /// 대상 객체에서 같은 이름을 가진 모든 메소드를 대상으로 호출 가능 여부 확인

        QByteArray dstMethodSig = QMetaObject::normalizedSignature( baMethodSig );

        for( int idx = 0; idx < meta->methodCount(); ++idx )
        {
            const QMetaMethod& metaMethod = meta->method( idx );

            if( metaMethod.methodType() != QMetaMethod::Slot &&
                metaMethod.methodType() != QMetaMethod::Method )
                continue;

            if( sMethodName.compare( metaMethod.name() ) != 0 )
                continue;

            QByteArray srcMethodSig = QMetaObject::normalizedSignature( metaMethod.methodSignature() );
            isAbleToCall = QMetaObject::checkConnectArgs( srcMethodSig, dstMethodSig );

            if( isAbleToCall == true )
                break;
        }

    } while( false );

    return isAbleToCall;
}

int CInvokeInfo::invoke( QObject* object )
{
    int isSuccessToInvokeID = 0;
    Q_ASSERT( object != Q_NULLPTR );
    Q_ASSERT( _argCount >= 1 );
    Q_ASSERT( _argTypes != Q_NULLPTR );
    Q_ASSERT( _args != Q_NULLPTR );

    do
    {
        if( _isAbleToCall == 0 &&
            checkMethodArgs( object, methodName(), methodSignature() ) == false )
        {
            _isAbleToCall = -1;
            Q_ASSERT( false );
            break;
        }

        int methodIndex = CInvokeInfo::methodIndex( object, methodSignature() );
        if( methodIndex <= 0 )
        {
            Q_ASSERT( false );
            break;
        }

        isSuccessToInvokeID = object->qt_metacall( QMetaObject::InvokeMetaMethod, methodIndex, args() );

    } while( false );

    return isSuccessToInvokeID;
}

int CInvokeInfo::invoke()
{
    return invoke( _object );
}

//////////////////////////////////////////////////////////////////////////

CInvokeInfoWithResult::CInvokeInfoWithResult()
    : CInvokeInfo()
{

}

CInvokeInfoWithResult::CInvokeInfoWithResult( const QString& sMethodName, QObject* object, QGenericReturnArgument ret, QGenericArgument val0, QGenericArgument val1, QGenericArgument val2, QGenericArgument val3, QGenericArgument val4, QGenericArgument val5, QGenericArgument val6, QGenericArgument val7, QGenericArgument val8, QGenericArgument val9 )
    : CInvokeInfo( sMethodName, object, val0, val1, val2, val3, val4, val5, val6, val7, val8, val9 )
{

}

namespace nsCmn
{
    namespace nsCmnQt
    {

        //////////////////////////////////////////////////////////////////////////
        /// QObject, MetaObject, Invoke...

        bool MarshalTo( QGenericArgument arg, QByteArray& ba )
        {
            bool isSuccess = false;

            do
            {
                const char* argName = arg.name();
                Q_ASSERT( argName != Q_NULLPTR );
                if( argName == Q_NULLPTR )
                    break;

                QDataStream in( &ba, QIODevice::WriteOnly );

                int nArgTypeId = QMetaType::type( argName );
                if( nArgTypeId == QMetaType::UnknownType )
                {
                    //Q_ASSERT_X( ( nArgTypeId == 0 ) || ( nArgTypeId == QMetaType::UnknownType ), __FUNCTION__,
                    //            QString("Qt Meta-Object System 에 등록되지 않은 인자입니다|{}", argName ) );
                    break;
                }

                isSuccess = QMetaType::save( in, nArgTypeId, arg.data() );

            } while( false );

            return isSuccess;
        }

        bool DemarshalFrom( const QString& sArgName, const QByteArray& ba, QGenericArgument& arg )
        {
            bool isSuccess = false;
            int nArgType = 0;
            void* data = Q_NULLPTR;

            do
            {
                if( sArgName.isEmpty() == true || ba.isEmpty() == true )
                {
                    arg = QGenericArgument();
                    return true;
                }

                nArgType = QMetaType::type( sArgName.toStdString().c_str() );
                if( nArgType == QMetaType::UnknownType )
                {
                    Q_ASSERT_X( nArgType == QMetaType::UnknownType, __FUNCTION__, "Unknown Meta-Type" );
                    break;
                }

                data = QMetaType::create( nArgType );
                Q_ASSERT( data != Q_NULLPTR );
                if( data == Q_NULLPTR )
                    break;

                QDataStream in( ba );
                isSuccess = QMetaType::load( in, nArgType, data );

            } while( false );

            if( isSuccess == true )
            {
                // NOTE: 반드시 이름 부분의 포인터를 해제해야한다. qstrdup 는 delete[] 를 통해 해제해야한다.  
                arg = QGenericArgument( qstrdup( sArgName.toStdString().c_str() ), data );
            }
            else
            {
                if( data != Q_NULLPTR )
                    QMetaType::destroy( nArgType, data );
            }

            return isSuccess;
        }

        bool InvokeMethod( QObjectPtr obj, const char* member, QT_METAMETHOD_ARGS )
        {
            return InvokeMethod( obj, member, Qt::AutoConnection, val0, val1, val2, val3, val4, val5, val6, val7, val8, val9 );
        }

        bool InvokeMethod( QObjectPtr obj, const char* member, Qt::ConnectionType type, QT_METAMETHOD_ARGS )
        {
            Q_ASSERT( obj.data() != Q_NULLPTR );
            Q_ASSERT( member != Q_NULLPTR && qstrlen( member ) > 0 );
            bool call = QMetaObject::invokeMethod( obj.data(), member, type, val0, val1, val2, val3, val4, val5, val6, val7, val8, val9 );
            Q_ASSERT( call == true );
            return call;
        }

        bool InvokeMethod( QObjectPtr obj, const char* member, QMetaMethodReturnArgument ret, QT_METAMETHOD_ARGS )
        {
            return InvokeMethod( obj, member, Qt::AutoConnection, ret, val0, val1, val2, val3, val4, val5, val6, val7, val8, val9 );
        }

        bool InvokeMethod( QObjectPtr obj, const char* member, Qt::ConnectionType type, QMetaMethodReturnArgument ret, QT_METAMETHOD_ARGS )
        {
            Q_ASSERT( obj.data() != Q_NULLPTR );
            Q_ASSERT( member != Q_NULLPTR && qstrlen( member ) > 0 );
            bool call = QMetaObject::invokeMethod( obj.data(), member, type, ret, val0, val1, val2, val3, val4, val5, val6, val7, val8, val9 );
            Q_ASSERT( call == true );
            return call;
        }

    } // nsCmnQt
} // nsCmn