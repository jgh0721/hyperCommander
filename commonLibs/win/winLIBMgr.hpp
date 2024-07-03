#ifndef __HDR_CMN_LIB_MGR__
#define __HDR_CMN_LIB_MGR__

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리
#include "cmnBase.hpp"
#include "cmnSystemDetection.hpp"
#include "cmnCompilerDetection.hpp"

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <cassert>

#if defined(_MSC_VER)
#   pragma execution_character_set( "utf-8" )
#endif

#if !defined(CMN_OS_WIN)
#error This Feature Required Windows OS(CMN_OS_WIN)
#endif

#if defined(CMN_OS_WIN)

#   if !defined(NOMINMAX)
#       define NOMINMAX
#   endif

#   if !defined(WIN32_LEAN_AND_MEAN)
#       define WIN32_LEAN_AND_MEAN
#   endif

#   include <windows.h>

/*!
 * http://www.codeproject.com/Articles/6272/LateLoad-DLL-Wrapper
 */

namespace nsCmn
{
    namespace nsDetailLib
    {
        enum eImportedProcState
        {
            IMPORT_PROC_STATE_UNKNOWN = 0,
            IMPORT_PROC_STATE_MISSING = 1,
            IMPORT_PROC_STATE_AVAILABLE = 2,
            IMPORT_PROC_STATE_MAX
        };

        // Why no v-table?
        // 1) not needed
        // 2) reduces code size
        // 3) LATELOAD_BEGIN_CLASS calls ZeroMemory to blank out all the derived function
        //    pointers & ImportedProcState memeber variables - in the process the vtable
        //    will get blanked out.
        // 4) This class is not intended to be instantiated on its own.
        // 5) Makes it so that using this class on its own will cause an Access Violation

        class __declspec(novtable) CLibBase
        {
        public:
            ~CLibBase()
            {
                if( _hLib != nullptr )
                {
                    FreeLibrary( _hLib );
                    _hLib = nullptr;
                }

                if( _sLib != nullptr )
                {
                    delete[] _sLib;
                    _sLib = nullptr;
                }
            }

            bool                                    IsInit() const { return _hLib != nullptr; }

            DWORD                                   GetLoadError() const { return _dwError; }
            const wchar_t* GetModuleName() const { return static_cast< const wchar_t* >( _sLib ); }

        protected:
            bool                                    init( const wchar_t* sModulePath )
            {
                assert( sModulePath != nullptr && _sLib == nullptr );

                if( _sLib == nullptr )
                {
                    if( sModulePath && *sModulePath )
                    {
                        size_t fileFullPathLength = wcslen( sModulePath );
                        _sLib = new wchar_t[ fileFullPathLength + 1 ];
                        memset( _sLib, '\0', sizeof( wchar_t ) * ( fileFullPathLength + 1 ) );
                        wcsncpy_s( _sLib, fileFullPathLength + 1, sModulePath, _TRUNCATE );
                    }
                }

                _dwError = 0;
                _hLib = ::LoadLibraryW( _sLib );
                _dwError = ::GetLastError();

                return IsInit();
            }
            FARPROC                                 getProcAddress( LPCSTR pszFuncName, eImportedProcState& ips )
            {
                FARPROC pfn = nullptr;
                ips = IMPORT_PROC_STATE_UNKNOWN;

                if( _hLib )
                {
                    pfn = ::GetProcAddress( _hLib, pszFuncName );
                    if( pfn )
                        ips = IMPORT_PROC_STATE_AVAILABLE;
                    else
                        ips = IMPORT_PROC_STATE_MISSING;
                }

                return pfn;
            }

            HMODULE                                 _hLib = nullptr;
            DWORD                                   _dwError = 0;

            wchar_t*                                _sLib = nullptr;
        };

        /*!
            DYNLOAD_BEGIN_CLASS( ClassName, L"ModuleName" )
                DYNLOAD_FUNC_0( NULL, HCURSOR, WINAPI, GetCursor )
                DYNLOAD_FUNC_1( NULL, HANDLE, WINAPI, NonExistantFunction, BOOL )
            DYNLOAD_END_CLASS()
        */
#       define DYNLOAD_BEGIN_CLASS( ClassName, ModuleName ) \
        class ClassName : public nsCmn::nsDetailLib::CLibBase \
        { \
        public: \
            ClassName() \
            { \
                init( ModuleName ); \
            } 

#define DECLARE_FUNC_PTR( FuncName ) \
        public: \
            TYPE_##FuncName m_pf##FuncName = nullptr; \
        private: \
            nsCmn::nsDetailLib::eImportedProcState m_ips##FuncName = nsCmn::nsDetailLib::IMPORT_PROC_STATE_UNKNOWN; \

#define DECLARE_FUNC_IS_FUNC( FuncName ) \
        public: \
            bool Is_##FuncName() \
            { \
                if( nsCmn::nsDetailLib::IMPORT_PROC_STATE_UNKNOWN == m_ips##FuncName ) \
                    m_pf##FuncName = (TYPE_##FuncName)getProcAddress( #FuncName, m_ips##FuncName); \
                return( nsCmn::nsDetailLib::IMPORT_PROC_STATE_AVAILABLE == m_ips##FuncName); \
            }

        // Function Declaration, Zero Parameters, returns a value
        //
        // ErrorResult, Default return value if the function could not be loaded & it is called anyways
        // ReturnType,  type of value that the function returns
        // CallingConv, Calling convention of the function
        // FuncName,    Name of the function
        //
        // A function prototype that looked like...
        //   typedef BOOL (CALLBACK* SOMETHING)();
        //  or
        //   BOOL CALLBACK Something();
        //
        // Would be changed to...
        //   DYNLOAD_FUNC_WITH_00(0,BOOL,CALLBACK,Something)
        //
        // If "Something" could not be loaded, and it was called - it would return 0
        //
        //	-------------------------------------------
        //
        //	LATELOAD_FUNC_0(NULL,HCURSOR,WINAPI,GetCursor)
        //	 NULL - if the ProcAddress was not loaded, this value is returned in the call to that func
        //	 HCURSOR - the return type for the function
        //	 WINAPI - function calling convention
        //	 GetCursor - the exported function
        //
        //	DYNLOAD_FUNC_WITH_0(up to _12)
        //	 Identical to LATELOAD_FUNC_0, except it allows 1-9 parameters to the function
        //
        //	DYNLOAD_FUNC_VOID_0(up to _12)
        //	 Same as before, but for use with NULL return types
        //

#define DYNLOAD_FUNC_WITH_00( ErrorResult, ReturnType, CallingConv, FuncName ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )(); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName() \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName(); \
        }

#define DYNLOAD_FUNC_WITH_01( ErrorResult, ReturnType, CallingConv, FuncName, ParamType1, ParamName1 ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )( ParamType1 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName( ParamType1 ParamName1 ) \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName( ParamName1 ); \
        }

#define DYNLOAD_FUNC_WITH_02( ErrorResult, ReturnType, CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2 ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName( ParamType1 ParamName1, ParamType2 ParamName2 ) \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName( ParamName1, ParamName2 ); \
        }

#define DYNLOAD_FUNC_WITH_03( ErrorResult, ReturnType, CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3 ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3 ) \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName( ParamName1, ParamName2, ParamName3 ); \
        }

#define DYNLOAD_FUNC_WITH_04( ErrorResult, ReturnType, CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4 ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4 ) \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4 ); \
        }

#define DYNLOAD_FUNC_WITH_05( ErrorResult, ReturnType, CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5 ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5 ) \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5 ); \
        }

#define DYNLOAD_FUNC_WITH_06( ErrorResult, ReturnType, CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6 ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6 ) \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6 ); \
        }

#define DYNLOAD_FUNC_WITH_07( ErrorResult, ReturnType, CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7 ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7 ) \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7 ); \
        }

#define DYNLOAD_FUNC_WITH_08( ErrorResult, ReturnType, CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7, ParamType8, ParamName8 ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7, ParamType8 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7, ParamType8 ParamName8 ) \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7, ParamName8 ); \
        }

#define DYNLOAD_FUNC_WITH_09( ErrorResult, ReturnType, CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7, ParamType8, ParamName8, ParamType9, ParamName9 ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7, ParamType8, ParamType9 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7, ParamType8 ParamName8, ParamType9 ParamName9 ) \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7, ParamName8, ParamName9 ); \
        }

#define DYNLOAD_FUNC_WITH_10( ErrorResult, ReturnType, CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7, ParamType8, ParamName8, ParamType9, ParamName9, ParamType10, ParamName10 ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7, ParamType8, ParamType9, ParamType10 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7, ParamType8 ParamName8, ParamType9 ParamName9, ParamType10 ParamName10 ) \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7, ParamName8, ParamName9, ParamName10 ); \
        }

#define DYNLOAD_FUNC_WITH_11( ErrorResult, ReturnType, CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7, ParamType8, ParamName8, ParamType9, ParamName9, ParamType10, ParamName10, ParamType11, ParamName11 ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7, ParamType8, ParamType9, ParamType10, ParamType11 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7, ParamType8 ParamName8, ParamType9 ParamName9, ParamType10 ParamName10, ParamType11 ParamName11 ) \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7, ParamName8, ParamName9, ParamName10, ParamName11 ); \
        }

#define DYNLOAD_FUNC_WITH_12( ErrorResult, ReturnType, CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7, ParamType8, ParamName8, ParamType9, ParamName9, ParamType10, ParamName10, ParamType11, ParamName11, ParamType12, ParamName12 ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7, ParamType8, ParamType9, ParamType10, ParamType11, ParamType12 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7, ParamType8 ParamName8, ParamType9 ParamName9, ParamType10 ParamName10, ParamType11 ParamName11, ParamType12 ParamName12 ) \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7, ParamName8, ParamName9, ParamName10, ParamName11, ParamName12 ); \
        }

#define DYNLOAD_FUNC_WITH_13( ErrorResult, ReturnType, CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7, ParamType8, ParamName8, ParamType9, ParamName9, ParamType10, ParamName10, ParamType11, ParamName11, ParamType12, ParamName12, ParamType13, ParamName13 ) \
        private: \
            typedef ReturnType( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7, ParamType8, ParamType9, ParamType10, ParamType11, ParamType12, ParamType13 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        ReturnType FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7, ParamType8 ParamName8, ParamType9 ParamName9, ParamType10 ParamName10, ParamType11 ParamName11, ParamType12 ParamName12, ParamType13 ParamName13 ) \
        { \
            if( !Is_##FuncName() ) \
                return ErrorResult; \
            return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7, ParamName8, ParamName9, ParamName10, ParamName11, ParamName12, ParamName13 ); \
        }

#define DYNLOAD_FUNC_VOID_00( CallingConv, FuncName ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )(); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName() \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName(); \
        }

#define DYNLOAD_FUNC_VOID_01( CallingConv, FuncName, ParamType1, ParamName1 ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )( ParamType1 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName( ParamType1 ParamName1 ) \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName( ParamName1 ); \
        }

#define DYNLOAD_FUNC_VOID_02( CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2 ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName( ParamType1 ParamName1, ParamType2 ParamName2 ) \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName( ParamName1, ParamName2 ); \
        }

#define DYNLOAD_FUNC_VOID_03( CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3 ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3 ) \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName( ParamName1, ParamName2, ParamName3 ); \
        }

#define DYNLOAD_FUNC_VOID_04( CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4 ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4 ) \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4 ); \
        }

#define DYNLOAD_FUNC_VOID_05( CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5 ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5 ) \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5 ); \
        }

#define DYNLOAD_FUNC_VOID_06( CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6 ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6 ) \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6 ); \
        }

#define DYNLOAD_FUNC_VOID_07( CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7 ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7 ) \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7 ); \
        }

#define DYNLOAD_FUNC_VOID_08( CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7, ParamType8, ParamName8 ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7, ParamType8 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7, ParamType8 ParamName8 ) \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7, ParamName8 ); \
        }

#define DYNLOAD_FUNC_VOID_09( CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7, ParamType8, ParamName8, ParamType9, ParamName9 ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7, ParamType8, ParamType9 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7, ParamType8 ParamName8, ParamType9 ParamName9 ) \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7, ParamName8, ParamName9 ); \
        }

#define DYNLOAD_FUNC_VOID_10( CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7, ParamType8, ParamName8, ParamType9, ParamName9, ParamType10, ParamName10 ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7, ParamType8, ParamType9, ParamType10 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7, ParamType8 ParamName8, ParamType9 ParamName9, ParamType10 ParamName10 ) \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7, ParamName8, ParamName9, ParamName10 ); \
        }

#define DYNLOAD_FUNC_VOID_11( CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7, ParamType8, ParamName8, ParamType9, ParamName9, ParamType10, ParamName10, ParamType11, ParamName11 ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7, ParamType8, ParamType9, ParamType10, ParamType11 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7, ParamType8 ParamName8, ParamType9 ParamName9, ParamType10 ParamName10, ParamType11 ParamName11 ) \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7, ParamName8, ParamName9, ParamName10, ParamName11 ); \
        }

#define DYNLOAD_FUNC_VOID_12( CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7, ParamType8, ParamName8, ParamType9, ParamName9, ParamType10, ParamName10, ParamType11, ParamName11, ParamType12, ParamName12 ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7, ParamType8, ParamType9, ParamType10, ParamType11, ParamType12 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7, ParamType8 ParamName8, ParamType9 ParamName9, ParamType10 ParamName10, ParamType11 ParamName11, ParamType12 ParamName12 ) \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7, ParamName8, ParamName9, ParamName10, ParamName11, ParamName12 ); \
        }

#define DYNLOAD_FUNC_VOID_13( CallingConv, FuncName, ParamType1, ParamName1, ParamType2, ParamName2, ParamType3, ParamName3, ParamType4, ParamName4, ParamType5, ParamName5, ParamType6, ParamName6, ParamType7, ParamName7, ParamType8, ParamName8, ParamType9, ParamName9, ParamType10, ParamName10, ParamType11, ParamName11, ParamType12, ParamName12, ParamType13, ParamName13 ) \
        private: \
            typedef void( CallingConv * TYPE_##FuncName )( ParamType1, ParamType2, ParamType3, ParamType4, ParamType5, ParamType6, ParamType7, ParamType8, ParamType9, ParamType10, ParamType11, ParamType12, ParamType13 ); \
        DECLARE_FUNC_PTR( FuncName ) \
        DECLARE_FUNC_IS_FUNC( FuncName ) \
        void FuncName( ParamType1 ParamName1, ParamType2 ParamName2, ParamType3 ParamName3, ParamType4 ParamName4, ParamType5 ParamName5, ParamType6 ParamName6, ParamType7 ParamName7, ParamType8 ParamName8, ParamType9 ParamName9, ParamType10 ParamName10, ParamType11 ParamName11, ParamType12 ParamName12, ParamType13 ParamName13 ) \
        { \
            if( Is_##FuncName() ) \
                return m_pf##FuncName( ParamName1, ParamName2, ParamName3, ParamName4, ParamName5, ParamName6, ParamName7, ParamName8, ParamName9, ParamName10, ParamName11, ParamName12, ParamName13 ); \
        }

#       define DYNLOAD_END_CLASS() };
    }

} // nsCmn

#endif // CMN_OS_WIN
#endif // __HDR_CMN_LIB_MGR__
