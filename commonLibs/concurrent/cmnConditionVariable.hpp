#ifndef HDR_CMN_CONCURRENT_CONDITION_VARIABLE
#define HDR_CMN_CONCURRENT_CONDITION_VARIABLE

#include "cmnMutex.hpp"
#include "cmnSingleton.hpp"

#include <map>
#include <memory>

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

        //////////////////////////////////////////////////////////////////////////
        /// Condition Variable

        class CConditionVariableEx
        {
        public:
            CConditionVariableEx();

            void                                        notifyOne();
            void                                        notifyAll();

            void                                        wait();
            // 조건이 만족하는지 일정시간(untilMs) 동안 기다린다. 조건이 만족하면 true, 만족하지 않으면 false 를 반환한다. 
            bool                                        waitUntil( unsigned int untilMs );

            void                                        reset();
        private:
            bool                                        isSatisfyCondition();

            bool                                        _isSatisfy;
            std::mutex                                  _mutex;
            std::condition_variable                     _cv;
        };

        typedef std::shared_ptr<CConditionVariableEx> TySpConditionVariableEx;

        class CConditionVariableMgr
        {
        public:
            CConditionVariableMgr();
            ~CConditionVariableMgr();

            TySpConditionVariableEx                     GetConditionVariableEx( const std::string& name );
            void                                        DeleteConditionVariableEx( const std::string& name );
            void                                        DeleteConditionVariableEx( TySpConditionVariableEx spConditionVariableEx );

        private:
            typedef std::map< std::string, TySpConditionVariableEx, nsCmn::lt_nocaseA > TyMapNameToConditionVariableEx;

            CCmnMutex                                   _mutex;
            TyMapNameToConditionVariableEx              _mapNameToConditionVariableEx;
        };

        typedef nsCmn::nsConcurrent::TSingleton< CConditionVariableMgr >  TyStConditionVariableMgr;

    } // nsConcurrent
} // nsCmn

#endif // HDR_CMN_CONCURRENT_CONDITION_VARIABLE