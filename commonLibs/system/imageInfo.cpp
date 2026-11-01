#include "imageInfo.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <SoftPub.h>
#include <WinTrust.h>
#include <mscat.h>
#include <wincrypt.h>

#include <vector>

namespace hc::system {

namespace {
std::wstring QueryString( const std::vector<unsigned char>& Data, WORD Language, WORD CodePage, const wchar_t* Key )
{
    wchar_t Path[96]{};
    swprintf_s( Path, L"\\StringFileInfo\\%04x%04x\\%s", Language, CodePage, Key );
    LPVOID Value = nullptr;
    UINT Length = 0;
    if( !VerQueryValueW( Data.data(), Path, &Value, &Length ) || !Value || Length == 0 )
        return {};
    std::wstring Text( static_cast<const wchar_t*>( Value ), Length );
    while( !Text.empty() && Text.back() == L'\0' )
        Text.pop_back();
    return Text;
}

std::wstring SignerName( HANDLE StateData )
{
    std::wstring Result;
    auto* Provider = WTHelperProvDataFromStateData( StateData );
    if( !Provider )
        return Result;
    auto* Signer = WTHelperGetProvSignerFromChain( Provider, 0, FALSE, 0 );
    if( !Signer || !Signer->pasCertChain || Signer->csCertChain == 0 )
        return Result;
    const auto* Certificate = Signer->pasCertChain[0].pCert;
    if( !Certificate )
        return Result;
    wchar_t Name[256]{};
    if( CertGetNameStringW( Certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, Name, 256 ) > 1 )
        Result = Name;
    return Result;
}

SignatureInfo Verify( WINTRUST_DATA& Data )
{
    GUID Action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    Data.cbStruct = sizeof( Data );
    Data.dwUIChoice = WTD_UI_NONE;
    Data.fdwRevocationChecks = WTD_REVOKE_NONE;
    Data.dwStateAction = WTD_STATEACTION_VERIFY;
    Data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    const LONG Status = WinVerifyTrust( static_cast<HWND>( INVALID_HANDLE_VALUE ), &Action, &Data );
    SignatureInfo Info;
    Info.code = uint32_t( Status );
    if( Status == ERROR_SUCCESS )
    {
        Info.state = SignatureState::Signed;
        Info.signer = SignerName( Data.hWVTStateData );
    }
    else if( Status == TRUST_E_NOSIGNATURE || Status == TRUST_E_SUBJECT_FORM_UNKNOWN || Status == TRUST_E_PROVIDER_UNKNOWN )
        Info.state = SignatureState::Unsigned;
    else
        Info.state = SignatureState::Invalid;
    Data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust( static_cast<HWND>( INVALID_HANDLE_VALUE ), &Action, &Data );
    return Info;
}

SignatureInfo VerifyCatalog( const std::wstring& Path, HANDLE File )
{
    SignatureInfo Info;
    Info.state = SignatureState::Unsigned;
    HCATADMIN Admin = nullptr;
    GUID Driver = DRIVER_ACTION_VERIFY;
    if( !CryptCATAdminAcquireContext2( &Admin, &Driver, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) )
        return Info;
    DWORD HashSize = 0;
    CryptCATAdminCalcHashFromFileHandle2( Admin, File, &HashSize, nullptr, 0 );
    std::vector<BYTE> Hash( HashSize );
    if( HashSize && CryptCATAdminCalcHashFromFileHandle2( Admin, File, &HashSize, Hash.data(), 0 ) )
    {
        HCATINFO Catalog = CryptCATAdminEnumCatalogFromHash( Admin, Hash.data(), HashSize, 0, nullptr );
        if( Catalog )
        {
            CATALOG_INFO CatalogInfo{};
            CatalogInfo.cbStruct = sizeof( CatalogInfo );
            if( CryptCATCatalogInfoFromContext( Catalog, &CatalogInfo, 0 ) )
            {
                std::wstring Tag;
                static const wchar_t Digits[] = L"0123456789ABCDEF";
                for( const auto Byte : Hash )
                {
                    Tag.push_back( Digits[Byte >> 4] );
                    Tag.push_back( Digits[Byte & 15] );
                }
                WINTRUST_CATALOG_INFO Member{};
                Member.cbStruct = sizeof( Member );
                Member.pcwszCatalogFilePath = CatalogInfo.wszCatalogFile;
                Member.pcwszMemberTag = Tag.c_str();
                Member.pcwszMemberFilePath = Path.c_str();
                Member.hMemberFile = File;
                Member.pbCalculatedFileHash = Hash.data();
                Member.cbCalculatedFileHash = HashSize;
                Member.hCatAdmin = Admin;
                WINTRUST_DATA Data{};
                Data.dwUnionChoice = WTD_CHOICE_CATALOG;
                Data.pCatalog = &Member;
                Info = Verify( Data );
                Info.catalog = true;
            }
            CryptCATAdminReleaseCatalogContext( Admin, Catalog, 0 );
        }
    }
    CryptCATAdminReleaseContext( Admin, 0 );
    return Info;
}
} // namespace

bool ReadVersionInfo( const std::wstring& Path, VersionInfo& Out )
{
    Out = {};
    DWORD Ignored = 0;
    const DWORD Size = GetFileVersionInfoSizeExW( FILE_VER_GET_NEUTRAL, Path.c_str(), &Ignored );
    if( Size == 0 || Size > 1024 * 1024 )
        return false;
    std::vector<unsigned char> Data( Size );
    if( !GetFileVersionInfoExW( FILE_VER_GET_NEUTRAL, Path.c_str(), 0, Size, Data.data() ) )
        return false;
    struct Translation { WORD language, codePage; };
    Translation* Pairs = nullptr;
    UINT Length = 0;
    std::vector<Translation> Candidates;
    if( VerQueryValueW( Data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>( &Pairs ), &Length ) && Pairs )
        Candidates.assign( Pairs, Pairs + Length / sizeof( Translation ) );
    Candidates.push_back( { 0x0409, 0x04B0 } );
    Candidates.push_back( { 0x0409, 0x04E4 } );
    for( const auto& Pair : Candidates )
    {
        Out.description = QueryString( Data, Pair.language, Pair.codePage, L"FileDescription" );
        Out.company = QueryString( Data, Pair.language, Pair.codePage, L"CompanyName" );
        Out.product = QueryString( Data, Pair.language, Pair.codePage, L"ProductName" );
        Out.version = QueryString( Data, Pair.language, Pair.codePage, L"FileVersion" );
        if( !Out.description.empty() || !Out.company.empty() )
            break;
    }
    if( Out.version.empty() )
    {
        VS_FIXEDFILEINFO* Fixed = nullptr;
        UINT FixedLength = 0;
        if( VerQueryValueW( Data.data(), L"\\", reinterpret_cast<LPVOID*>( &Fixed ), &FixedLength ) && Fixed )
            Out.version = std::to_wstring( HIWORD( Fixed->dwFileVersionMS ) ) + L"." + std::to_wstring( LOWORD( Fixed->dwFileVersionMS ) ) + L"."
                          + std::to_wstring( HIWORD( Fixed->dwFileVersionLS ) ) + L"." + std::to_wstring( LOWORD( Fixed->dwFileVersionLS ) );
    }
    return true;
}

SignatureInfo VerifySignature( const std::wstring& Path )
{
    HANDLE File = CreateFileW( Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr );
    if( File == INVALID_HANDLE_VALUE )
        return {};
    WINTRUST_FILE_INFO FileInfo{};
    FileInfo.cbStruct = sizeof( FileInfo );
    FileInfo.pcwszFilePath = Path.c_str();
    FileInfo.hFile = File;
    WINTRUST_DATA Data{};
    Data.dwUnionChoice = WTD_CHOICE_FILE;
    Data.pFile = &FileInfo;
    auto Info = Verify( Data );
    if( Info.state == SignatureState::Unsigned )
        Info = VerifyCatalog( Path, File );
    CloseHandle( File );
    return Info;
}

} // namespace hc::system
