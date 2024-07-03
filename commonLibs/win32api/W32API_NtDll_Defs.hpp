#ifndef __HDR_CMNLIB_W32API_NTDLL_DEFS__
#define __HDR_CMNLIB_W32API_NTDLL_DEFS__

namespace nsW32API
{
    namespace nsNtDeclare
    {

        //////////////////////////////////////////////////////////////////////////
/// STRING

#ifndef UNICODE_STRING_MAX_BYTES
#define UNICODE_STRING_MAX_BYTES ((USHORT) 65534L) // winnt
#endif
#ifndef UNICODE_STRING_MAX_CHARS
#define UNICODE_STRING_MAX_CHARS (32767L) // winnt
#endif

        typedef struct _STRING
        {
            USHORT Length;
            USHORT MaximumLength;
#if !defined( __GNUC__)
            __field_bcount_part_opt( MaximumLength, Length )
#endif
                PCHAR Buffer;
        } STRING, * PSTRING, ANSI_STRING, * PANSI_STRING, OEM_STRING, * POEM_STRING;

        typedef const STRING* PCSTRING;
        typedef const ANSI_STRING* PCANSI_STRING;
        typedef const OEM_STRING* PCOEM_STRING;

        typedef struct _STRING32
        {
            USHORT Length;
            USHORT MaximumLength;
            ULONG Buffer;
        } STRING32, * PSTRING32;

        typedef STRING32 UNICODE_STRING32;
        typedef UNICODE_STRING32* PUNICODE_STRING32;

        typedef STRING32 ANSI_STRING32;
        typedef ANSI_STRING32* PANSI_STRING32;

        typedef struct _UNICODE_STRING
        {
            USHORT Length;
            USHORT MaximumLength;
#if !defined( __GNUC__)
            __field_bcount_part_opt( MaximumLength, Length )
#endif
                PWCH Buffer;
        } UNICODE_STRING;
        typedef UNICODE_STRING* PUNICODE_STRING;
        typedef const UNICODE_STRING* PCUNICODE_STRING;

        typedef USHORT RTL_STRING_LENGTH_TYPE;

        //////////////////////////////////////////////////////////////////////////
        /// OBJECT

        typedef enum _SECTION_INHERIT
        {
            ViewShare = 1,
            ViewUnmap = 2
        } SECTION_INHERIT;

        //
        // Valid values for the Attributes field
        //
#ifndef OBJ_INHERIT
#define OBJ_INHERIT             0x00000002L
#endif
#ifndef OBJ_PERMANENT
#define OBJ_PERMANENT           0x00000010L
#endif
#ifndef OBJ_EXCLUSIVE
#define OBJ_EXCLUSIVE           0x00000020L
#endif
#ifndef OBJ_CASE_INSENSITIVE
#define OBJ_CASE_INSENSITIVE    0x00000040L
#endif
#ifndef OBJ_OPENIF
#define OBJ_OPENIF              0x00000080L
#endif
#ifndef OBJ_OPENLINK
#define OBJ_OPENLINK            0x00000100L
#endif
#ifndef OBJ_KERNEL_HANDLE
#define OBJ_KERNEL_HANDLE       0x00000200L
#endif
#ifndef OBJ_FORCE_ACCESS_CHECK
#define OBJ_FORCE_ACCESS_CHECK  0x00000400L
#endif
#ifndef OBJ_VALID_ATTRIBUTES
#define OBJ_VALID_ATTRIBUTES    0x000007F2L
#endif

        typedef struct _OBJECT_ATTRIBUTES
        {
            ULONG Length;
            HANDLE RootDirectory;
            PUNICODE_STRING ObjectName;
            ULONG Attributes;
            PVOID SecurityDescriptor; // PSECURITY_DESCRIPTOR;
            PVOID SecurityQualityOfService; // PSECURITY_QUALITY_OF_SERVICE
        } OBJECT_ATTRIBUTES, * POBJECT_ATTRIBUTES;

        typedef const OBJECT_ATTRIBUTES* PCOBJECT_ATTRIBUTES;

        typedef struct _OBJECT_ATTRIBUTES64
        {
            ULONG Length;
            ULONG64 RootDirectory;
            ULONG64 ObjectName;
            ULONG Attributes;
            ULONG64 SecurityDescriptor;
            ULONG64 SecurityQualityOfService;
        } OBJECT_ATTRIBUTES64, * POBJECT_ATTRIBUTES64;

        typedef const OBJECT_ATTRIBUTES64* PCOBJECT_ATTRIBUTES64;

        typedef struct _OBJECT_ATTRIBUTES32
        {
            ULONG Length;
            ULONG RootDirectory;
            ULONG ObjectName;
            ULONG Attributes;
            ULONG SecurityDescriptor;
            ULONG SecurityQualityOfService;
        } OBJECT_ATTRIBUTES32, * POBJECT_ATTRIBUTES32;

        typedef const OBJECT_ATTRIBUTES32* PCOBJECT_ATTRIBUTES32;

    } // nsNtDeclare

} // nsW32API

#endif // __HDR_CMNLIB_W32API_NTDLL_DEFS__
