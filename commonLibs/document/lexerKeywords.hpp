#pragma once
// Lexilla keyword lists shared by HyperEditor and HyperCompare. List 0 styles
// as a keyword (WORD), list 1 as a secondary keyword or type (WORD2).
namespace hc::lexer
{
    inline constexpr char cKeywords[] =
        "alignas alignof auto bool break case char const constexpr continue default do double else enum "
        "extern false float for goto if inline int long nullptr register restrict return short signed sizeof "
        "static static_assert struct switch thread_local true typedef typeof typeof_unqual union unsigned void "
        "volatile while _Alignas _Alignof _Atomic _BitInt _Bool _Complex _Decimal128 _Decimal32 _Decimal64 "
        "_Generic _Imaginary _Noreturn _Static_assert _Thread_local "
        "__attribute__ __declspec __cdecl __stdcall __fastcall";
    inline constexpr char cTypes[] =
        "size_t ssize_t ptrdiff_t intptr_t uintptr_t intmax_t uintmax_t max_align_t nullptr_t "
        "int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t wchar_t char16_t char32_t "
        "FILE va_list";
    inline constexpr char cppKeywords[] =
        "alignas alignof and and_eq asm auto bitand bitor bool break case catch "
        "char char8_t char16_t char32_t class compl concept const consteval constexpr "
        "constinit const_cast continue co_await co_return co_yield decltype default "
        "delete do double dynamic_cast else enum explicit export extern false final "
        "float for friend goto if import inline int long module mutable namespace new "
        "noexcept not not_eq nullptr operator or or_eq override private protected public "
        "register reinterpret_cast requires return short signed sizeof static "
        "static_assert static_cast struct switch template this thread_local throw true "
        "try typedef typeid typename union unsigned using virtual void volatile "
        "wchar_t while xor xor_eq "
        "_Alignas _Alignof _Atomic _Bool _Complex _Generic _Imaginary _Noreturn "
        "_Static_assert _Thread_local restrict "
        "__attribute__ __declspec __cdecl __stdcall __fastcall __thiscall "
        "int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t "
        "size_t ssize_t ptrdiff_t intptr_t uintptr_t";
    inline constexpr char cppTypes[] =
        "string wstring vector map unordered_map set unordered_set list deque queue "
        "stack array pair tuple optional variant any shared_ptr unique_ptr weak_ptr "
        "function thread mutex atomic future promise "
        "QString QStringList QWidget QObject QList QVector QHash QMap QSet "
        "QPointer QSharedPointer QScopedPointer QByteArray QVariant";
    inline constexpr char pythonKeywords[] =
        "False None True and as assert async await break class continue def del "
        "elif else except finally for from global if import in is lambda nonlocal "
        "not or pass raise return try while with yield";
    inline constexpr char pythonBuiltins[] =
        "abs all any bin bool bytearray bytes callable chr classmethod compile complex "
        "delattr dict dir divmod enumerate eval exec filter float format frozenset "
        "getattr globals hasattr hash help hex id input int isinstance issubclass iter "
        "len list locals map max memoryview min next object oct open ord pow print "
        "property range repr reversed round set setattr slice sorted staticmethod str "
        "sum super tuple type vars zip __import__";
    // Soft keywords are identifiers outside their statements, so they style as list 1.
    inline constexpr char pythonSoftKeywords[] = "match case";
}
