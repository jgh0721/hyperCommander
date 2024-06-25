#ifndef HDR_CMN_CONCURRENT_MUTEX
#define HDR_CMN_CONCURRENT_MUTEX

#include "cmnBase.hpp"
#include "cmnOAL_Defs.hpp"

#include <mutex>
#include <atomic>

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
        namespace nsDetail
        {

        } // nsDetail

        class CCmnMutexLocker;
        class CCmnMutex
        {
            CMN_DISALLOW_COPY_AND_ASSIGN( CCmnMutex );
        public:
            CMN_DECL_CONSTEXPR enum RecursionMode { NonRecursive, Recursive };
#if defined(USE_OS_NATIVE_IMPLEMENTATION) 
#if defined(CMN_OS_WIN)
            explicit CCmnMutex( RecursionMode mode = Recursive );
#endif
#else
            explicit CCmnMutex( RecursionMode mode = NonRecursive );
#endif

            ~CCmnMutex();

            CMN_DECL_CONSTEXPR bool    IsRecursive() const { return _mode == Recursive; }
            bool    IsLocked() const;

            void    Lock() CMN_DECL_NOEXCEPT;
            bool    TryLock() CMN_DECL_NOEXCEPT;
            void    Unlock() CMN_DECL_NOTHROW;

        private:
            friend class CCmnMutexLocker;
#if defined(USE_OS_NATIVE_IMPLEMENTATION) 
    #if defined(CMN_OS_WIN)
            CRITICAL_SECTION    _mutexData;
    #else
            static_assert( false );
    #endif
#else
    #if defined(USE_QT_SUPPORT)
            QMutex              _mutexData;
    #else
            std::mutex          _mutexData;
    #endif
#endif
            RecursionMode       _mode       = NonRecursive;
            nsOAL::oalTID       _tid        = 0;
            std::atomic_uint32_t _refCount  = 0;
        };

        class CCmnMutexLocker
        {
            CMN_DISALLOW_COPY_AND_ASSIGN( CCmnMutexLocker );
        public:
            inline explicit CCmnMutexLocker( CCmnMutex* m ) CMN_DECL_NOEXCEPT
                : _mutexData( m )
            {
                _mutexData->Lock();
            }

            inline ~CCmnMutexLocker() { unlock(); }

            inline void unlock() CMN_DECL_NOTHROW
            {
                _mutexData->Unlock();
            }

            inline void relock() CMN_DECL_NOEXCEPT
            {
                if( _mutexData->IsLocked() == false )
                    _mutexData->Lock();
            }

        private:
            CCmnMutex*                      _mutexData;
        };

    } // nsConcurrent

} // nsCmn

#endif // HDR_CMN_CONCURRENT_MUTEX