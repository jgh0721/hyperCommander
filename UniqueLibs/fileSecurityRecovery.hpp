#pragma once
#include "fileBatchOperations.hpp"
#include <Aclapi.h>
#include <Sddl.h>
namespace hc::permissions::security {
struct Result { DWORD code_=0,rollbackCode_=0,statusCode_=0; QJsonObject backup_; };
inline bool Privilege(const wchar_t* Name) {
    HANDLE Raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&Raw))return false;
    filetools::Handle Token(Raw);TOKEN_PRIVILEGES P{};P.PrivilegeCount=1;
    if(!LookupPrivilegeValueW(nullptr,Name,&P.Privileges[0].Luid))return false;P.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);return AdjustTokenPrivileges(Raw,FALSE,&P,sizeof(P),nullptr,nullptr)&&GetLastError()==ERROR_SUCCESS;
}
inline QString Descriptor(HANDLE File,DWORD& Code) {
    PSECURITY_DESCRIPTOR SD=nullptr;Code=GetSecurityInfo(File,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,nullptr,nullptr,nullptr,nullptr,&SD);
    if(Code)return {};LPWSTR Text=nullptr;
    if(!ConvertSecurityDescriptorToStringSecurityDescriptorW(SD,SDDL_REVISION_1,OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&Text,nullptr))Code=GetLastError();
    const auto Value=Text?QString::fromWCharArray(Text):QString();if(Text)LocalFree(Text);LocalFree(SD);return Value;
}
inline QString Digest(const QString& SD){return QString::fromLatin1(QCryptographicHash::hash(SD.toUtf8(),QCryptographicHash::Sha256).toHex());}
inline DWORD Restore(HANDLE File,const QString& Original,bool Directory=false) {
    PSECURITY_DESCRIPTOR SD=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(Original.utf16()),SDDL_REVISION_1,&SD,nullptr))return GetLastError();
    PSID Owner=nullptr,Group=nullptr;PACL Dacl=nullptr;BOOL Default=FALSE,Present=FALSE;SECURITY_DESCRIPTOR_CONTROL Control=0;DWORD Revision=0;
    GetSecurityDescriptorOwner(SD,&Owner,&Default);GetSecurityDescriptorGroup(SD,&Group,&Default);GetSecurityDescriptorDacl(SD,&Present,&Dacl,&Default);GetSecurityDescriptorControl(SD,&Control,&Revision);
    const DWORD Flags=OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|(Directory?0:DACL_SECURITY_INFORMATION|(Control&SE_DACL_PROTECTED?PROTECTED_DACL_SECURITY_INFORMATION:UNPROTECTED_DACL_SECURITY_INFORMATION));
    // SetSecurityInfo preserves owner/group/ACE data and protection. Windows
    // may normalize a legacy ACL by adding SE_DACL_AUTO_INHERITED (AI).
    const auto Code=SetSecurityInfo(File,SE_FILE_OBJECT,Flags,Owner,Group,Dacl,nullptr);LocalFree(SD);return Code;
}
inline bool TrustedBackup(HANDLE File) {
    PSID Owner=nullptr;PACL Dacl=nullptr;PSECURITY_DESCRIPTOR SD=nullptr;
    if(GetSecurityInfo(File,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&Owner,nullptr,&Dacl,nullptr,&SD))return false;
    BYTE Admin[SECURITY_MAX_SID_SIZE]{},System[SECURITY_MAX_SID_SIZE]{};DWORD A=sizeof(Admin),S=sizeof(System);
    CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,Admin,&A);CreateWellKnownSid(WinLocalSystemSid,nullptr,System,&S);
    SECURITY_DESCRIPTOR_CONTROL Control=0;DWORD Revision=0;GetSecurityDescriptorControl(SD,&Control,&Revision);
    bool Safe=(Control&SE_DACL_PROTECTED)&&Owner&&(EqualSid(Owner,Admin)||EqualSid(Owner,System))&&Dacl;
    // A caller-owned replacement is rejected even if its contents/hash match.
    // Only administrators and SYSTEM may alter the durable backup descriptor.
    for(DWORD I=0;Safe&&I<Dacl->AceCount;++I){void* Raw=nullptr;if(!GetAce(Dacl,I,&Raw)){Safe=false;break;}auto* Ace=static_cast<ACCESS_ALLOWED_ACE*>(Raw);
        if(Ace->Header.AceType!=ACCESS_ALLOWED_ACE_TYPE){Safe=false;break;}
        if((Ace->Mask&(FILE_WRITE_DATA|FILE_APPEND_DATA|WRITE_DAC|WRITE_OWNER|DELETE|GENERIC_WRITE|GENERIC_ALL))&&!EqualSid(&Ace->SidStart,Admin)&&!EqualSid(&Ace->SidStart,System))Safe=false;
    }
    LocalFree(SD);return Safe;
}
inline DWORD Save(HANDLE Backup,const QJsonObject& Record) {
    const auto Bytes=QJsonDocument(Record).toJson(QJsonDocument::Compact);LARGE_INTEGER Zero{};DWORD Count=0;
    if(!SetFilePointerEx(Backup,Zero,nullptr,FILE_BEGIN)||!WriteFile(Backup,Bytes.constData(),DWORD(Bytes.size()),&Count,nullptr)||Count!=Bytes.size()||!SetEndOfFile(Backup)||!FlushFileBuffers(Backup))return GetLastError();return ERROR_SUCCESS;
}
inline DWORD SaveStatus(const QString& OriginalPath,QJsonObject& Record,const QString& CallerSid) {
    const auto Path=OriginalPath+".state-"+QUuid::createUuid().toString(QUuid::Id128)+".json",Native=filetools::NativePath(Path);
    Record["statusPath"]=Path;const auto Sddl="O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;"+CallerSid+")";
    PSECURITY_DESCRIPTOR SD=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(Sddl.utf16()),SDDL_REVISION_1,&SD,nullptr))return GetLastError();
    SECURITY_ATTRIBUTES Attributes{sizeof(Attributes),SD,FALSE};filetools::Handle File(CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()),GENERIC_WRITE,FILE_SHARE_READ,&Attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));LocalFree(SD);
    if(File.value_==INVALID_HANDLE_VALUE)return GetLastError();return Save(File.value_,Record);
}
// File operations never invoke this service implicitly. A separate job owns one
// explicitly selected object. SACL and recursive inheritance are not changed.
inline Result Execute(const QString& Target,const QString& ExpectedIdentity,const QString& CallerSid,const QString& BackupFolder,
    bool Grant,bool Restoring,const QString& BackupPath={},const QString& BackupIdentity={},const QString& ExpectedDigest={},const QString& JobId={}) {
    Result R;Privilege(SE_BACKUP_NAME);Privilege(SE_TAKE_OWNERSHIP_NAME);
    if(!Privilege(SE_RESTORE_NAME)){R.code_=ERROR_PRIVILEGE_NOT_HELD;return R;}
    const auto Native=filetools::NativePath(Target);filetools::Handle File(CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()),READ_CONTROL|WRITE_OWNER|WRITE_DAC|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(File.value_==INVALID_HANDLE_VALUE){R.code_=GetLastError();return R;}BY_HANDLE_FILE_INFORMATION Info{};
    if(!GetFileInformationByHandle(File.value_,&Info)||Info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT||filetools::Identity(File.value_)!=ExpectedIdentity.toLatin1()){R.code_=ERROR_FILE_INVALID;return R;}
    if(Grant&&(Info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){R.code_=ERROR_NOT_SUPPORTED;return R;}
    const auto Before=Descriptor(File.value_,R.code_);if(R.code_)return R;
    if(Restoring) {
        if(QFileInfo(BackupPath).absolutePath().compare(QDir::cleanPath(BackupFolder),Qt::CaseInsensitive)){R.code_=ERROR_INVALID_PARAMETER;return R;}
        const auto B=filetools::NativePath(BackupPath);filetools::Handle Backup(CreateFileW(reinterpret_cast<LPCWSTR>(B.utf16()),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(Backup.value_==INVALID_HANDLE_VALUE){R.code_=GetLastError();return R;}
        BY_HANDLE_FILE_INFORMATION BI{};if(!GetFileInformationByHandle(Backup.value_,&BI)||BI.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)||!TrustedBackup(Backup.value_)||filetools::Identity(Backup.value_)!=BackupIdentity.toLatin1()){R.code_=ERROR_FILE_INVALID;return R;}
        LARGE_INTEGER Size{};if(!GetFileSizeEx(Backup.value_,&Size)||Size.QuadPart<1||Size.QuadPart>1024*1024){R.code_=ERROR_INVALID_DATA;return R;}
        QByteArray Bytes(qsizetype(Size.QuadPart),0);DWORD Count=0;if(!ReadFile(Backup.value_,Bytes.data(),DWORD(Bytes.size()),&Count,nullptr)||Count!=Bytes.size()){R.code_=ERROR_READ_FAULT;return R;}
        R.backup_=QJsonDocument::fromJson(Bytes).object();
        if(R.backup_["identity"]!=ExpectedIdentity||R.backup_["target"].toString().compare(Target,Qt::CaseInsensitive)||ExpectedDigest.isEmpty()||Digest(Before)!=ExpectedDigest||Digest(R.backup_["original"].toString())!=R.backup_["originalDigest"]){R.code_=ERROR_FILE_INVALID;return R;}
        R.backup_["restoreJobId"]=JobId;R.backup_["appliedDigest"]=ExpectedDigest;
        R.code_=Restore(File.value_,R.backup_["original"].toString(),Info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY);R.backup_["state"]=R.code_?"restore-failed":"restored";
        DWORD CurrentCode=0;const auto Current=Descriptor(File.value_,CurrentCode);
        if(!CurrentCode)R.backup_["appliedDigest"]=Digest(Current);else if(!R.code_)R.code_=CurrentCode;
        R.backup_["restoreCode"]=int(R.code_);R.statusCode_=SaveStatus(BackupPath,R.backup_,CallerSid);if(!R.code_)R.code_=R.statusCode_;return R;
    }
    const auto Path=QDir(BackupFolder).filePath(QUuid::createUuid().toString(QUuid::Id128)+".json"),B=filetools::NativePath(Path);
    const auto Sddl="O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;"+CallerSid+")";PSECURITY_DESCRIPTOR SD=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(Sddl.utf16()),SDDL_REVISION_1,&SD,nullptr)){R.code_=GetLastError();return R;}
    SECURITY_ATTRIBUTES Attributes{sizeof(Attributes),SD,FALSE};filetools::Handle Backup(CreateFileW(reinterpret_cast<LPCWSTR>(B.utf16()),GENERIC_READ|GENERIC_WRITE|READ_CONTROL,FILE_SHARE_READ,&Attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));LocalFree(SD);
    if(Backup.value_==INVALID_HANDLE_VALUE){R.code_=GetLastError();return R;}
    R.backup_={{"target",Target},{"jobId",JobId},{"utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},{"identity",ExpectedIdentity},{"original",Before},{"originalDigest",Digest(Before)},{"backupPath",Path},{"backupIdentity",QString::fromLatin1(filetools::Identity(Backup.value_))},{"state","prepared"}};
    R.code_=Save(Backup.value_,R.backup_);if(R.code_)return R; // No mutation without a flushed backup.
    PSID Caller=nullptr;if(!ConvertStringSidToSidW(reinterpret_cast<LPCWSTR>(CallerSid.utf16()),&Caller)){R.code_=GetLastError();return R;}
    R.code_=SetSecurityInfo(File.value_,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,Caller,nullptr,nullptr,nullptr);
    if(!R.code_&&Grant){PACL Old=nullptr,Updated=nullptr;PSECURITY_DESCRIPTOR Existing=nullptr;
        R.code_=GetSecurityInfo(File.value_,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&Old,nullptr,&Existing);
        if(!R.code_&&!Old)R.code_=ERROR_NOT_SUPPORTED;
        if(!R.code_){EXPLICIT_ACCESSW Access{};Access.grfAccessPermissions=FILE_GENERIC_READ|FILE_GENERIC_WRITE|DELETE;Access.grfAccessMode=GRANT_ACCESS;Access.grfInheritance=NO_INHERITANCE;
            Access.Trustee.TrusteeForm=TRUSTEE_IS_SID;Access.Trustee.TrusteeType=TRUSTEE_IS_USER;Access.Trustee.ptstrName=static_cast<LPWSTR>(Caller);
            R.code_=SetEntriesInAclW(1,&Access,Old,&Updated);if(!R.code_)R.code_=SetSecurityInfo(File.value_,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,Updated,nullptr);}
        if(Updated)LocalFree(Updated);if(Existing)LocalFree(Existing);
    }
    LocalFree(Caller);if(R.code_)R.rollbackCode_=Restore(File.value_,Before,Info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY);
    DWORD ReadCode=0;const auto After=Descriptor(File.value_,ReadCode);R.backup_["appliedDigest"]=Digest(After);R.backup_["state"]=R.code_?(R.rollbackCode_?"rollback-failed":"rolled-back"):"applied";
    R.backup_["code"]=int(R.code_);R.backup_["rollbackCode"]=int(R.rollbackCode_);
    // The original descriptor file is immutable after its first durable save.
    // Updating it in place after changing an ACL could destroy the only backup
    // on a partial write. Outcomes use independent protected status files.
    R.statusCode_=SaveStatus(Path,R.backup_,CallerSid);
    if(!R.code_&&(ReadCode||R.statusCode_)) {
        R.code_=ReadCode?ReadCode:R.statusCode_;R.rollbackCode_=Restore(File.value_,Before,Info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY);
        DWORD CurrentCode=0;R.backup_["appliedDigest"]=Digest(Descriptor(File.value_,CurrentCode));
        R.backup_["state"]=R.rollbackCode_?"rollback-failed":"rolled-back";R.backup_["code"]=int(R.code_);R.backup_["rollbackCode"]=int(R.rollbackCode_);
        SaveStatus(Path,R.backup_,CallerSid); // Original backup survives either failure.
    }
    return R;
}
}
