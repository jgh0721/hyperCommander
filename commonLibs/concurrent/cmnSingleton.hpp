#ifndef HDR_CMN_CONCURRENT_SINGLETON
#define HDR_CMN_CONCURRENT_SINGLETON

#include "cmnMutex.hpp"
#include <atomic>

#if defined(_MSC_VER)
#   pragma execution_character_set( "utf-8" )
#endif

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

#endif // HDR_CMN_CONCURRENT_SINGLETON