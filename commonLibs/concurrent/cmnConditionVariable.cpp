#include "stdafx.h"
#include "cmnConditionVariable.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

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

        //////////////////////////////////////////////////////////////////////////
        /// Condition Variable

        CConditionVariableEx::CConditionVariableEx()
            : _isSatisfy( false )
        {

        }

        void CConditionVariableEx::notifyOne()
        {
            {
                std::unique_lock< std::mutex > lck( _mutex );
                _isSatisfy = true;
            }

            _cv.notify_one();
        }

        void CConditionVariableEx::notifyAll()
        {
            {
                std::unique_lock< std::mutex > lck( _mutex );
                _isSatisfy = true;
            }

            _cv.notify_all();
        }

        void CConditionVariableEx::wait()
        {
            {
                std::unique_lock< std::mutex > lck( _mutex );
                _cv.wait( lck, std::bind( &CConditionVariableEx::isSatisfyCondition, this ) );
            }

            reset();
        }

        bool CConditionVariableEx::waitUntil( unsigned int untilMs )
        {
            assert( untilMs > 0 );

            bool isSatisfy = false;
            std::chrono::system_clock::time_point wakeUpTime = std::chrono::system_clock::now() + std::chrono::milliseconds( untilMs );

            {
                std::unique_lock< std::mutex > lck( _mutex );
                isSatisfy = _cv.wait_until( lck, wakeUpTime, std::bind( &CConditionVariableEx::isSatisfyCondition, this ) );
            }

            if( isSatisfy == true )
                reset();

            return isSatisfy;
        }

        //////////////////////////////////////////////////////////////////////////

        void CConditionVariableEx::reset()
        {
            std::unique_lock< std::mutex > lck( _mutex );
            _isSatisfy = false;
        }

        bool CConditionVariableEx::isSatisfyCondition()
        {
            // 해당 함수는 wait 내부에서 호출되므로 별도로 락을 잡을 필요 없음. ( wait 에서 락을 획득한다. )
            return _isSatisfy;
        }

        //////////////////////////////////////////////////////////////////////////
        //////////////////////////////////////////////////////////////////////////

        CConditionVariableMgr::CConditionVariableMgr()
        {

        }

        CConditionVariableMgr::~CConditionVariableMgr()
        {
            CCmnMutexLocker lck( &_mutex );
            _mapNameToConditionVariableEx.clear();
        }

        nsCmn::nsConcurrent::TySpConditionVariableEx CConditionVariableMgr::GetConditionVariableEx( const std::string& name )
        {
            CCmnMutexLocker lck( &_mutex );

            if( _mapNameToConditionVariableEx.count( name ) > 0 )
                return _mapNameToConditionVariableEx[ name ];

            nsCmn::nsConcurrent::TySpConditionVariableEx spConditionVariableEx;

            spConditionVariableEx = std::make_shared< CConditionVariableEx >();
            _mapNameToConditionVariableEx.insert( std::make_pair( name, spConditionVariableEx ) );

            return spConditionVariableEx;
        }

        void CConditionVariableMgr::DeleteConditionVariableEx( const std::string& name )
        {
            CCmnMutexLocker lck( &_mutex );

            if( _mapNameToConditionVariableEx.count( name ) > 0 )
                _mapNameToConditionVariableEx.erase( name );
        }

        void CConditionVariableMgr::DeleteConditionVariableEx( TySpConditionVariableEx spConditionVariableEx )
        {
            CCmnMutexLocker lck( &_mutex );

            for( TyMapNameToConditionVariableEx::iterator it = _mapNameToConditionVariableEx.begin(); it != _mapNameToConditionVariableEx.end(); ++it )
            {
                if( spConditionVariableEx != nullptr &&
                    it->second != nullptr &&
                    spConditionVariableEx.get() == it->second.get() )
                {
                    _mapNameToConditionVariableEx.erase( it );
                    break;
                }
            }
        }

    } // nsConcurrent
} // nsCmn