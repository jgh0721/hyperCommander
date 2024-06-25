#include "stdafx.h"
#include "cmnMutex.hpp"

#if defined(USE_OS_NATIVE_IMPLEMENTATION) 
    #if defined(CMN_OS_WIN)
    #else
    #endif
#else
    #if defined(USE_QT_SUPPORT)
        #include <QtCore>
    #else
    #endif
#endif

#if defined(_MSC_VER)
#   pragma execution_character_set( "utf-8" )
#endif

namespace nsCmn
{
    namespace nsConcurrent
    {
        CCmnMutex::CCmnMutex( RecursionMode mode /*= NonRecursive */ )
            : _mode( mode )
#if defined(USE_OS_NATIVE_IMPLEMENTATION) 
    #if defined(CMN_OS_WIN)
    #else
    #endif
#else
    #if defined(USE_QT_SUPPORT)
            , _mutexData( (QMutex::RecursionMode) mode )
    #else
    #endif
#endif
        {
#if defined(USE_OS_NATIVE_IMPLEMENTATION) 
    #if defined(CMN_OS_WIN)
            InitializeCriticalSectionAndSpinCount( &_mutexData, 4000 );
    #else
    #endif
#else
    #if defined(USE_QT_SUPPORT)
    #else
            static_assert( mode == NonRecursive, "std::mutex not support recursive mode" );
    #endif
#endif
        }

        CCmnMutex::~CCmnMutex()
        {
#if defined(USE_OS_NATIVE_IMPLEMENTATION) 
    #if defined(CMN_OS_WIN)
            DeleteCriticalSection( &_mutexData );
    #else
    #endif
#else
    #if defined(USE_QT_SUPPORT)
    #else
    #endif
#endif
        }

        bool CCmnMutex::IsLocked() const
        {
#if defined(USE_OS_NATIVE_IMPLEMENTATION) 
    #if defined(CMN_OS_WIN)
            return _mutexData.LockCount >= 0;
#else
            assert( false );
    #endif
#else
            return _refCount > 0;
    #if defined(USE_QT_SUPPORT)
    #else
    #endif
#endif
        }

        void CCmnMutex::Lock() CMN_DECL_NOEXCEPT
        {
#if defined(USE_OS_NATIVE_IMPLEMENTATION) 
    #if defined(CMN_OS_WIN)
            EnterCriticalSection( &_mutexData );
#else
            assert( false );
    #endif
#else
    #if defined(USE_QT_SUPPORT)
            _mutexData.lock();
            _refCount++;
    #else
            _mutexData.lock();
            _refCount++;
#endif
#endif
        }

        bool CCmnMutex::TryLock() CMN_DECL_NOEXCEPT
        {
            do
            {
#if defined(USE_OS_NATIVE_IMPLEMENTATION) 
    #if defined(CMN_OS_WIN)
                return TryEnterCriticalSection( &_mutexData ) != FALSE;
    #else
                assert( false );
    #endif
#else
    #if defined(USE_QT_SUPPORT)
                if( _mutexData.try_lock() == true )
                    _refCount++;
                return true;
    #else
                if( _mutexData.try_lock() == true )
                    _refCount++;
                return true;
#endif
#endif

            } while (false);

            return false;
        }

        void CCmnMutex::Unlock() CMN_DECL_NOTHROW
        {
#if defined(USE_OS_NATIVE_IMPLEMENTATION) 
    #if defined(CMN_OS_WIN)
            LeaveCriticalSection( &_mutexData );
    #else
            assert( false );
    #endif
#else
    #if defined(USE_QT_SUPPORT)
            _mutexData.unlock();
            _refCount--;
    #else
            _mutexData.unlock();
            _refCount--;
#endif
#endif
        }

    } // nsConcurrent

} // nsCmn
