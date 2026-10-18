#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Shellapi.h>
#include "commonLibs/document/documentHostProtocol.hpp"
#include <string>
#include <vector>
using namespace hc::document;
namespace
{
    bool Transfer(HANDLE Pipe,void* Data,DWORD Size,bool Write)
    {
        DWORD Offset=0;
        while(Offset<Size)
        {
            DWORD Count=0;
            if(!(Write?WriteFile(Pipe,static_cast<char*>(Data)+Offset,Size-Offset,&Count,nullptr)
                :ReadFile(Pipe,static_cast<char*>(Data)+Offset,Size-Offset,&Count,nullptr)) || !Count) return false;
            Offset+=Count;
        }
        return true;
    }
    // Control files sit in the runtime root beside this exe; the owner starts it in another working directory.
    std::wstring Beside(const wchar_t* Name)
    {
        wchar_t Program[32768]{}; const auto Length=GetModuleFileNameW(nullptr,Program,32768);
        std::wstring Path(Program,Length); return Path.substr(0,Path.find_last_of(L"\\/")+1)+Name;
    }
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int)
{
    int Count=0; auto Args=CommandLineToArgvW(GetCommandLineW(),&Count);
    std::wstring PipeName,Nonce; DWORD Owner=0; bool Child=false, Proxy=false;
    for(int I=1;I<Count;++I)
    {
        const std::wstring Arg=Args[I];
        if(Arg==L"--child") Child=true;
        if(Arg==L"--proxy") Proxy=true;
        if(I+1<Count && Arg==L"--pipe") PipeName=Args[++I];
        else if(I+1<Count && Arg==L"--connection") Nonce=Args[++I];
        else if(I+1<Count && Arg==L"--parent-process") Owner=std::stoul(Args[++I]);
    }
    LocalFree(Args);
    if(Child) { Sleep(60000); return 0; }
    char Mode[64]{}; HANDLE File=CreateFileW(Beside(L"mode.txt").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    DWORD Read=0; if(File!=INVALID_HANDLE_VALUE) { ReadFile(File,Mode,63,&Read,nullptr); CloseHandle(File); }
    const std::string ModeText(Mode);
    if(ModeText=="proxy-peer" && !Proxy)
    {
        auto Command=std::wstring(GetCommandLineW())+L" --proxy";
        STARTUPINFOW Startup{}; Startup.cb=sizeof(Startup); PROCESS_INFORMATION Process{};
        if(!CreateProcessW(nullptr,Command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&Startup,&Process)) return 6;
        CloseHandle(Process.hThread); CloseHandle(Process.hProcess); Sleep(60000); return 0;
    }
    if(ModeText=="hang-ready-tree")
    {
        wchar_t Program[32768]{}; GetModuleFileNameW(nullptr,Program,32768);
        auto Command=L"\"" + std::wstring(Program) + L"\" --child";
        STARTUPINFOW Startup{}; Startup.cb=sizeof(Startup); PROCESS_INFORMATION Process{};
        if(!CreateProcessW(Program,Command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&Startup,&Process)) return 7;
        const auto Pids=std::to_string(GetCurrentProcessId())+" "+std::to_string(Process.dwProcessId);
        const auto Marker=CreateFileW(Beside(L"tree.txt").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
        DWORD Written=0;
        if(Marker!=INVALID_HANDLE_VALUE) { WriteFile(Marker,Pids.data(),DWORD(Pids.size()),&Written,nullptr); CloseHandle(Marker); }
        CloseHandle(Process.hThread); CloseHandle(Process.hProcess); Sleep(60000); return 0;
    }
    if(ModeText=="hang-ready") { Sleep(60000); return 0; }
    if(Nonce.size()!=32 || PipeName.empty()) return 2;
    HANDLE Pipe=CreateFileW(PipeName.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(Pipe==INVALID_HANDLE_VALUE) return 3;
    ULONG Server=0; if(!GetNamedPipeServerProcessId(Pipe,&Server) || Server!=Owner) return 4;
    HostFrame Ready; Ready.type_=0;
    for(int I=0;I<16;++I) Ready.connection_[I]=static_cast<unsigned char>(std::stoul(Nonce.substr(I*2,2),nullptr,16));
    if(ModeText=="wrong-nonce") Ready.connection_[0]^=1;
    if(!Transfer(Pipe,&Ready,sizeof(Ready),true)) return 5;
    for(;;)
    {
        HostFrame Frame; if(!Transfer(Pipe,&Frame,sizeof(Frame),false)) break;
        if(!ValidFrame(Frame,Ready.connection_) || Frame.type_!=1) break;
        std::vector<char> Bytes(Frame.length_);
        if(!Transfer(Pipe,Bytes.data(),Frame.length_,false)) break;
        const std::string Request(Bytes.begin(),Bytes.end());
        std::string Reply=Request;
        if(Request=="spawn")
        {
            wchar_t Program[32768]{}; GetModuleFileNameW(nullptr,Program,32768);
            auto Command=L"\""+std::wstring(Program)+L"\" --child";
            STARTUPINFOW Startup{}; Startup.cb=sizeof(Startup); PROCESS_INFORMATION Process{};
            if(!CreateProcessW(Program,Command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&Startup,&Process)) Reply="failed";
            else { Reply=std::to_string(Process.dwProcessId); CloseHandle(Process.hProcess); CloseHandle(Process.hThread); }
        }
        if(Request=="environment")
        {
            wchar_t Value[16]{}; Reply=GetEnvironmentVariableW(L"HC_TEST_SECRET",Value,16)?"leaked":"clean";
        }
        if(Request=="system-drive")
        {
            char Value[MAX_PATH]{}; const auto Length=GetEnvironmentVariableA("SystemDrive",Value,MAX_PATH);
            Reply=Length && Length<MAX_PATH?std::string(Value,Length):"missing";
        }
        if(Request=="policy")
        {
            JOBOBJECT_BASIC_UI_RESTRICTIONS Ui{}; JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
            if(!QueryInformationJobObject(nullptr,JobObjectBasicUIRestrictions,&Ui,sizeof(Ui),nullptr)
                || !QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,&Limits,sizeof(Limits),nullptr)) Reply="query-failed";
            else Reply=std::to_string(Ui.UIRestrictionsClass)+","+std::to_string(Limits.BasicLimitInformation.ActiveProcessLimit);
        }
        if(Request=="hang") { Sleep(60000); break; }
        Frame.type_=2; Frame.length_=DWORD(Reply.size());
        if(Request=="stale") ++Frame.requestId_;
        if(Request=="oversize") Frame.length_=HostPayloadLimit+1;
        if(!Transfer(Pipe,&Frame,sizeof(Frame),true) || !Transfer(Pipe,Reply.data(),DWORD(Reply.size()),true)) break;
    }
    CloseHandle(Pipe); return 0;
}
