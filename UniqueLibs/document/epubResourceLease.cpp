#include "stdafx.h"
#include "epubResourceLease.hpp"
#include <Windows.h>
#include <sddl.h>
#include <array>
#include <vector>
namespace hc::document {
namespace {
QString identity() {
    HANDLE Token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&Token))return {};
    DWORD Size=0;GetTokenInformation(Token,TokenUser,nullptr,0,&Size);std::vector<unsigned char> Bytes(Size);
    const bool Ok=Size && GetTokenInformation(Token,TokenUser,Bytes.data(),Size,&Size);CloseHandle(Token);if(!Ok)return {};
    LPWSTR Sid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(Bytes.data())->User.Sid,&Sid))return {};
    const auto Result=QString::fromWCharArray(Sid);LocalFree(Sid);return Result;
}
}
struct EpubResourceLease::State {
    std::array<HANDLE,TotalLimit> pending_{};std::array<HANDLE,ActiveLimit> active_{};
    int pendingIndex_=-1,activeIndex_=-1;QString prefix_,error_;DWORD thread_=GetCurrentThreadId();
    QString job(int Index)const{return prefix_+".job."+QString::number(Index);}
    ~State(){
        Q_ASSERT(GetCurrentThreadId()==thread_);
        if(activeIndex_>=0)ReleaseMutex(active_[activeIndex_]);if(pendingIndex_>=0)ReleaseMutex(pending_[pendingIndex_]);
        for(const auto Handle:active_)if(Handle)CloseHandle(Handle);for(const auto Handle:pending_)if(Handle)CloseHandle(Handle);
    }
};
EpubResourceLease::EpubResourceLease(const QString& Domain):state_(std::make_unique<State>()) {
    const auto Sid=identity();if(Sid.isEmpty()){state_->error_="epub-resource-user";return;}
    state_->prefix_="Local\\HyperCommander.Epub.v1."+Sid+(Domain.isEmpty()?QString{}:".test."+Domain);
    for(unsigned I=0;I<TotalLimit;++I)state_->pending_[I]=CreateMutexW(nullptr,FALSE,reinterpret_cast<LPCWSTR>((state_->prefix_+".pending."+QString::number(I)).utf16()));
    for(unsigned I=0;I<ActiveLimit;++I)state_->active_[I]=CreateMutexW(nullptr,FALSE,reinterpret_cast<LPCWSTR>((state_->prefix_+".active."+QString::number(I)).utf16()));
    for(const auto H:state_->pending_)if(!H)state_->error_="epub-resource-mutex";for(const auto H:state_->active_)if(!H)state_->error_="epub-resource-mutex";
}
EpubResourceLease::~EpubResourceLease()=default;
QString EpubResourceLease::Error()const{return state_->error_;}
QString EpubResourceLease::JobName()const{return state_->activeIndex_<0?QString{}:state_->job(state_->activeIndex_);}
bool EpubResourceLease::Acquire(const std::function<bool()>& Cancelled,const std::function<void()>& Queued) {
    auto& S=*state_;if(!S.error_.isEmpty() || S.pendingIndex_>=0)return false;
    if(Cancelled && Cancelled()){S.error_="cancelled";return false;}
    auto Wait=WaitForMultipleObjects(TotalLimit,S.pending_.data(),FALSE,0);
    if(Wait<TotalLimit)S.pendingIndex_=int(Wait);
    else if(Wait>=WAIT_ABANDONED_0 && Wait<WAIT_ABANDONED_0+TotalLimit)S.pendingIndex_=int(Wait-WAIT_ABANDONED_0);
    else {S.error_=Wait==WAIT_TIMEOUT?"epub-resource-queue-full":"epub-resource-wait";return false;}
    bool Reported=false;
    while(!Cancelled || !Cancelled()) {
        Wait=WaitForMultipleObjects(ActiveLimit,S.active_.data(),FALSE,50);bool Abandoned=false;
        if(Wait<ActiveLimit)S.activeIndex_=int(Wait);
        else if(Wait>=WAIT_ABANDONED_0 && Wait<WAIT_ABANDONED_0+ActiveLimit){S.activeIndex_=int(Wait-WAIT_ABANDONED_0);Abandoned=true;}
        else if(Wait==WAIT_TIMEOUT){if(!Reported && Queued){Queued();Reported=true;}continue;}
        else {S.error_="epub-resource-wait";return false;}
        // Recover a crashed worker's entire process tree before reusing its slot.
        const auto Job=OpenJobObjectW(JOB_OBJECT_QUERY|JOB_OBJECT_TERMINATE,FALSE,reinterpret_cast<LPCWSTR>(S.job(S.activeIndex_).utf16()));
        if(Job) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting{};
            if(Abandoned)TerminateJobObject(Job,1);
            bool Drained=false;
            for(unsigned Retry=0;Retry<200;++Retry) {
                if(!QueryInformationJobObject(Job,JobObjectBasicAccountingInformation,&Accounting,sizeof(Accounting),nullptr))break;
                if(!Accounting.ActiveProcesses){Drained=true;break;}
                if(Cancelled && Cancelled())break;Sleep(25);
            }
            CloseHandle(Job);if(!Drained){S.error_="epub-resource-stale-job";return false;}
        } else if(GetLastError()!=ERROR_FILE_NOT_FOUND){S.error_="epub-resource-job-query";return false;}
        return true;
    }
    S.error_="cancelled";return false;
}
}
