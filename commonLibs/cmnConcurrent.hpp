#pragma once

#include <memory>
#include <atomic>

namespace nsCmn
{
    namespace nsOAL
    {
        namespace nsDetail
        {
        } // nsDetail

        typedef enum
        {
            OS_TYPE_UNKNOWN = 0

            // 리눅스
            , OS_TYPE_LINUX

            // MAC
            , OS_TYPE_MAC

            // WINDOWS
            , OS_TYPE_WINDOWS

        } TyEnOSType;

        //////////////////////////////////////////////////////////////////////////
        /// Files and Directories

        namespace nsDetail
        {
#if defined(CMN_OS_WIN)
            const wchar_t nativeSep = L'\\';
#else
            const wchar_t nativeSep = L'/';
#endif
            const wchar_t win32Sep = L'\\';
            const wchar_t commonSep = L'/';

        } // nsDetail

        // 현재 디렉토리를 구한다. SHELL 지원을 활성화하지 않으면 _getcwd 함수를 사용하여 현재 디렉토리를 반환한다
        // 항상 \\ 붙인다. 

        template< typename CHAR >
        const CHAR* GetNativePathSep();

        template<>
        inline const char* GetNativePathSep()
        {
#if defined(CMN_OS_WIN)
            return "\\";
#else
            return "/";
#endif
        }

        template<>
        inline const wchar_t* GetNativePathSep()
        {
#if defined(CMN_OS_WIN)
            return L"\\";
#else
            return L"/";
#endif
        }


        //////////////////////////////////////////////////////////////////////////
        /// Process, Thread

#if defined(CMN_OS_WIN)
        using oalPID = DWORD;
        using oalTID = DWORD;
#else
#if defined(CMN_OS_LINUX)
        using oalPID = pid_t;
        using oalTID = pid_t;
#elif defined(CMN_OS_MAC)
        using oalPID = uint64_t;
        using oalTID = uint64_t;
#endif
#endif
    }
}

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
            explicit CCmnMutex( RecursionMode mode = NonRecursive );
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
            RecursionMode       _mode = NonRecursive;
            nsOAL::oalTID       _tid = 0;
            std::atomic_uint32_t _refCount = 0;
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
            CCmnMutex* _mutexData;
        };

    } // nsConcurrent

} // nsCmn

namespace nsCmn
{
    namespace nsConcurrent
    {
        //////////////////////////////////////////////////////////////////////////
        /// 싱글턴 클래스

        /*!
            싱글턴 구현 유틸리티

            사용방법 :
            클래스 ABC 를 싱글턴으로 사용하고 싶을 때
            TSingleton<ABC>::GetInstance()
        */

        template< typename T >
        class TSingleton
        {
        public:
            static T* GetInstance()
            {
                T* tmp = _pSelf.load( std::memory_order_consume );
                if( tmp == nullptr )
                {
                    CCmnMutexLocker lck( &_initSingletonMutex );
                    tmp = _pSelf.load( std::memory_order_consume );
                    if( tmp == nullptr )
                    {
                        tmp = new T;
                        assert( tmp != nullptr );
                        _pSelf.store( tmp, std::memory_order_release );
                    }
                }
                return tmp;
            }

            static void ReleaseInstance()
            {
                T* tmp = _pSelf.load( std::memory_order_consume );
                if( tmp != nullptr )
                {
                    CCmnMutexLocker lck( &_initSingletonMutex );
                    tmp = _pSelf.load( std::memory_order_consume );
                    if( tmp != nullptr )
                    {
                        delete tmp;
                        _pSelf.store( nullptr, std::memory_order_release );
                    }
                }
            }

        protected:
            TSingleton() {};
            ~TSingleton() {};

        private:
            static std::atomic< T* >                _pSelf;
            static CCmnMutex                        _initSingletonMutex;
        };

        template< typename T >
        std::atomic<T*> TSingleton<T>::_pSelf;
        template< typename T >
        CCmnMutex TSingleton<T>::_initSingletonMutex;

    } // nsConcurrent

} // nsCmn
