#pragma once

#ifdef _MSC_VER
# define CMN_MSC_VER _MSC_VER
#else
# define CMN_MSC_VER 0
#endif

#define _MSVC2005_VER 1400
#define _MSVC2008_VER 1500
#define _MSVC2010_VER 1600
#define _MSVC2012_VER 1700
#define _MSVC2013_VER 1800
#define _MSVC2015_VER 1900
#define _MSVC2017_VER 1910
#define _MSVC2019_VER 1920
#define _MSVC2022_VER 1930

#ifdef _SECURE_SCL
# define CMN_SECURE_SCL _SECURE_SCL
#else
# define CMN_SECURE_SCL 0
#endif

#ifdef __has_feature
# define CMN_HAS_FEATURE(x) __has_feature(x)
#else
# define CMN_HAS_FEATURE(x) 0
#endif

#ifdef __has_builtin
# define CMN_HAS_BUILTIN(x) __has_builtin(x)
#else
# define CMN_HAS_BUILTIN(x) 0
#endif

#ifdef __has_cpp_attribute
# define CMN_HAS_CPP_ATTRIBUTE(x) __has_cpp_attribute(x)
#else
# define CMN_HAS_CPP_ATTRIBUTE(x) 0
#endif

// Check if exceptions are disabled.
#if defined(__GNUC__) && !defined(__EXCEPTIONS)
# define CMN_EXCEPTIONS 0
#endif
#if CMN_MSC_VER && !_HAS_EXCEPTIONS
# define CMN_EXCEPTIONS 0
#endif
#ifndef CMN_EXCEPTIONS
# define CMN_EXCEPTIONS 1
#endif

// based on Qt Compiler Detection

#if defined(_MSC_VER) 
#   if defined(__cplusplus)
#       if CMN_MSC_VER >= _MSVC2008_VER
#           define CMN_COMPILER_SHARED_PTR
#       endif // CMN_MSC_VER >= _MSVC2008_VER
#       if CMN_MSC_VER >= _MSVC2010_VER
#           define CMN_COMPILER_AUTO_TYPE
#           define CMN_COMPILER_NULLPTR
#           define CMN_COMPILER_RVALUE_REFS
#       endif // CMN_MSC_VER >= _MSVC2010_VER
#       if CMN_MSC_VER >= _MSVC2012_VER
#           define CMN_COMPILER_CLASS_ENUM
#           define CMN_COMPILER_ATOMICS
#       endif // CMN_MSC_VER >= _MSVC2012_VER
#       if CMN_MSC_VER >= _MSVC2013_VER
#           define CMN_COMPILER_VARIADIC_TEMPLATES
#       endif // CMN_MSC_VER >= _MSVC2013_VER
#       if _MSC_FULL_VER >= 180030324 // VC 12 SP 2 RC
#           define CMN_COMPILER_INITIALIZER_LISTS
#       endif // VC 12 SP 2 RC
#       if CMN_MSC_VER >= _MSVC2015_VER
#           define CMN_COMPILER_DEFAULT_MEMBERS
#           define CMN_COMPILER_DELETE_MEMBERS
#           define CMN_COMPILER_NOEXCEPT
#           define CMN_COMPILER_RANGE_FOR
#           define CMN_COMPILER_REF_QUALIFIERS
#           define CMN_COMPILER_THREAD_LOCAL
#           define CMN_COMPILER_UDL
#           define CMN_COMPILER_UNICODE_STRINGS
#           define CMN_COMPILER_UNRESTRICTED_UNIONS
#           define CMN_COMPILER_RESTRICTED_KEYWORD
#       endif // CMN_MSC_VER >= _MSVC2015_VER
#   endif // defined(__cplusplus)
#   define CMN_GCC_VERSION 0
#endif // _MSC_VER

#ifdef CMN_COMPILER_DEFAULT_MEMBERS
#   define CMN_DECL_EQ_DEFAULT = default
#else
#   define CMN_DECL_EQ_DEFAULT
#endif

#ifdef CMN_COMPILER_DELETE_MEMBERS
#   define CMN_DECL_EQ_DELETE = delete
#else
#   define CMN_DECL_EQ_DELETE
#endif

#if defined CMN_COMPILER_CONSTEXPR
#   if defined(__cpp_constexpr) && __cpp_constexpr-0 >= 201304
#       define CMN_DECL_CONSTEXPR constexpr
#       define CMN_DECL_RELAXED_CONSTEXPR constexpr
#       define CMN_CONSTEXPR constexpr
#       define CMN_RELAXED_CONSTEXPR constexpr
#   else
#       define CMN_DECL_CONSTEXPR constexpr
#       define CMN_DECL_RELAXED_CONSTEXPR
#       define CMN_CONSTEXPR constexpr
#       define CMN_RELAXED_CONSTEXPR const
#   endif
#else
#   define CMN_DECL_CONSTEXPR
#   define CMN_DECL_RELAXED_CONSTEXPR
#   define CMN_CONSTEXPR const
#   define CMN_RELAXED_CONSTEXPR const
#endif

#ifdef CMN_COMPILER_EXPLICIT_OVERRIDES
#   define CMN_DECL_OVERRIDE override
#   define CMN_DECL_FINAL final
#else
#   ifndef CMN_DECL_OVERRIDE
#       define CMN_DECL_OVERRIDE
#   endif
#   ifndef CMN_DECL_FINAL
#       define CMN_DECL_FINAL
#   endif
#endif

#ifdef CMN_COMPILER_NOEXCEPT
#   define CMN_DECL_NOEXCEPT noexcept
#   define CMN_DECL_NOEXCEPT_EXPR(x) noexcept(x)
#   ifdef CMN_DECL_NOTHROW
#       undef CMN_DECL_NOTHROW /* override with C++11 noexcept if available */
#   endif
#else
#   define CMN_DECL_NOEXCEPT
#   define CMN_DECL_NOEXCEPT_EXPR(x)
#endif

#ifndef CMN_DECL_NOTHROW
#   define CMN_DECL_NOTHROW CMN_DECL_NOEXCEPT
#endif

#ifdef _MSC_VER
#  define CMN_NEVER_INLINE __declspec(noinline)
#  define CMN_ALWAYS_INLINE __forceinline
#elif defined(__GNU__)
#  define CMN_NEVER_INLINE __attribute__((noinline))
#  define CMN_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#  define CMN_NEVER_INLINE
#  define CMN_ALWAYS_INLINE inline
#endif

// 
// #ifdef _SECURE_SCL
// # define CMN_SECURE_SCL _SECURE_SCL
// #else
// # define CMN_SECURE_SCL 0
// #endif

#if defined(CMN_COMPILER_RESTRICTED_KEYWORD)
#define CMN_DECL_RESTRICTED __restrict
#else
#define CMN_DECL_RESTRICTED
#endif

#ifdef __GNUC__
    #define CMN_GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)
    #define CMN_GCC_EXTENSION __extension__
    #if CMN_GCC_VERSION >= 406
        #pragma GCC diagnostic push
        // Disable the warning about "long long" which is sometimes reported even
        // when using __extension__.
        #pragma GCC diagnostic ignored "-Wlong-long"
        // Disable the warning about declaration shadowing because it affects too
        // many valid cases.
        #pragma GCC diagnostic ignored "-Wshadow"
        // Disable the warning about implicit conversions that may change the sign of
        // an integer; silencing it otherwise would require many explicit casts.
        #pragma GCC diagnostic ignored "-Wsign-conversion"
    #endif
    #if __cplusplus >= 201103L || defined __GXX_EXPERIMENTAL_CXX0X__
        #define CMN_HAS_GXX_CXX11 1
    #endif

//     #define W(x)          W_(x)
//     #define W_(x)         L ## x
// 
//     #define LOCATION_(t)  t(__FILE__)
//     #define FUNCTION_(t)  t(__FUNCTION__)
// 
//     #ifndef __FILEW__ 
//         #define __FILEW__ LOCATION_(W)
//     #endif
//     #ifndef __FUNCTIONW__ 
//         #define __FUNCTIONW__ L""
//     #endif

// Variadic templates are available in GCC since version 4.4
// (http://gcc.gnu.org/projects/cxx0x.html) and in Visual C++
// since version 2013.
#if (CMN_HAS_FEATURE(cxx_variadic_templates) || (CMN_GCC_VERSION >= 404 && CMN_HAS_GXX_CXX11))
#   define CMN_COMPILER_VARIADIC_TEMPLATES
#endif

#else
// # define CMN_GCC_EXTENSION
#endif

// #if defined(__GNUC_LIBSTD__) && defined (__GNUC_LIBSTD_MINOR__)
// # define CMN_GNUC_LIBSTD_VERSION (__GNUC_LIBSTD__ * 100 + __GNUC_LIBSTD_MINOR__)
// #endif
// 
// #ifndef CMN_USE_NULLPTR
// #   define CMN_USE_NULLPTR ((CMN_GCC_VERSION >= 405 && CMN_HAS_GXX_CXX11) || (CMN_MSC_VER >= _MSVC2010_VER))
// #endif

// #ifndef CMN_USE_RVALUE_REFERENCES
// // Don't use rvalue references when compiling with clang and an old libstdc++
// // as the latter doesn't provide std::move.
// # if defined(CMN_GNUC_LIBSTD_VERSION) && CMN_GNUC_LIBSTD_VERSION <= 402
// #  define CMN_USE_RVALUE_REFERENCES 0
// # else
// #  define CMN_USE_RVALUE_REFERENCES \
//     (CMN_HAS_FEATURE(cxx_rvalue_references) || \
//         (CMN_GCC_VERSION >= 403 && CMN_HAS_GXX_CXX11) || CMN_MSC_VER >= _MSVC2010_VER)
// # endif
// #endif
// 

#ifndef CMN_THROW
# if CMN_EXCEPTIONS
#  define CMN_THROW(x) throw x
# else
#  define CMN_THROW(x) assert(false)
# endif
#endif

// // Define FMT_USE_NOEXCEPT to make fmt use noexcept (C++11 feature).
// #ifndef CMN_USE_NOEXCEPT
// # define CMN_USE_NOEXCEPT 0
// #endif
// 

#if CMN_EXCEPTIONS
#   if defined(CMN_COMPILER_NOEXCEPT) || CMN_HAS_FEATURE(cxx_noexcept)
#       define CMN_NOEXCEPT noexcept
#   else
#       define CMN_NOEXCEPT throw()
#   endif
#else
#   define CMN_NOEXCEPT
#endif

#if CMN_EXCEPTIONS
# define CMN_TRY try
# define CMN_CATCH(x) catch (x)
#else
# define CMN_TRY if (true)
# define CMN_CATCH(x) if (false)
#endif

// // 
// // A macro to disallow the copy constructor and operator= functions
// // This should be used in the private: declarations for a class
// #ifndef CMN_USE_DELETED_FUNCTIONS
// # define CMN_USE_DELETED_FUNCTIONS 0
// #endif
// 

#if defined(CMN_COMPILER_DELETE_MEMBERS) || CMN_HAS_FEATURE(cxx_deleted_functions) 
    # define CMN_DELETED_OR_UNDEFINED  = delete
    # define CMN_DISALLOW_COPY_AND_ASSIGN(TypeName) \
        TypeName(const TypeName&) = delete; \
        TypeName& operator=(const TypeName&) = delete
#else
    # define CMN_DELETED_OR_UNDEFINED
    # define CMN_DISALLOW_COPY_AND_ASSIGN(TypeName) \
        TypeName(const TypeName&); \
        TypeName& operator=(const TypeName&)
#endif

// 
// #ifndef CMN_USE_USER_DEFINED_LITERALS
// // All compilers which support UDLs also support variadic templates. This
// // makes the fmt::literals implementation easier. However, an explicit check
// // for variadic templates is added here just in case.
// // For Intel's compiler both it and the system gcc/msc must support UDLs.
// # define CMN_USE_USER_DEFINED_LITERALS \
//    CMN_USE_VARIADIC_TEMPLATES && CMN_USE_RVALUE_REFERENCES && \
//    (CMN_HAS_FEATURE(cxx_user_literals) || \
//      (CMN_GCC_VERSION >= 407 && CMN_HAS_GXX_CXX11) || CMN_MSC_VER >= _MSVC2015_VER)
// #endif
// 
// #ifndef CMN_USE_UNIQUE_PTR
// #if defined(__GNUC__) || (CMN_MSC_VER >= _MSVC2010_VER)
// #define CMN_USE_UNIQUE_PTR 1 
// #else 
// #define CMN_USE_UNIQUE_PTR 0
// #endif
// #endif

#if defined(CMN_COMPILER_RVALUE_REFS)
    #define cmnMove(x) std::move(x)
    #define CMN_RVALUE_PARAM(x, arg) x&& arg
#else
    #define cmnMove(x) (x)
    #define CMN_RVALUE_PARAM(x, arg) const x& arg 
#endif
