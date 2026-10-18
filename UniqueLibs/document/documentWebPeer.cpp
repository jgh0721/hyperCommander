#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "documentWebPeer.hpp"

namespace hc::document
{
    DocumentWebPeer::DocumentWebPeer(QObject* Parent):QObject(Parent)
    {
        socket_.setReadBufferSize(HostPayloadLimit+sizeof(HostFrame));
        connect(&socket_,&QLocalSocket::readyRead,this,[this] { receive(); });
        connect(&socket_,&QLocalSocket::disconnected,this,[this] { fail(); });
        connect(&socket_,&QLocalSocket::errorOccurred,this,[this](auto) { fail(); });
        ownerTimer_.setInterval(100);
        connect(&ownerTimer_,&QTimer::timeout,this,[this] {
            if(!owner_ || WaitForSingleObject(owner_,0)!=WAIT_TIMEOUT) fail();
        });
    }
    DocumentWebPeer::~DocumentWebPeer()
    {
        disconnected_={}; failed_=true; ownerTimer_.stop(); socket_.abort();
        if(owner_) CloseHandle(owner_);
    }
    bool DocumentWebPeer::Connect(const QString& Pipe,const QByteArray& ConnectionHex,
        std::uint32_t OwnerPid,std::uintptr_t ParentWindow)
    {
        if(connected_ || failed_ || !OwnerPid || ConnectionHex.size()!=32
            || QByteArray::fromHex(ConnectionHex).toHex()!=ConnectionHex) return false;
        const auto Bytes=QByteArray::fromHex(ConnectionHex);
        std::memcpy(connection_.data(),Bytes.constData(),16); parentWindow_=ParentWindow;
        owner_=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,OwnerPid);
        if(!owner_ || WaitForSingleObject(owner_,0)!=WAIT_TIMEOUT) { fail(); return false; }
        if(ParentWindow)
        {
            DWORD Pid=0; GetWindowThreadProcessId(reinterpret_cast<HWND>(ParentWindow),&Pid);
            if(Pid!=OwnerPid) { fail(); return false; }
        }
        socket_.connectToServer(Pipe);
        if(!socket_.waitForConnected(3000)) { fail(); return false; }
        ULONG Server=0;
        if(!GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(socket_.socketDescriptor()),&Server)
            || Server!=OwnerPid) { fail(); return false; }
        connected_=true; ownerTimer_.start(); return true;
    }
    bool DocumentWebPeer::Ready(std::uintptr_t ChildWindow,Handler Request,std::function<void()> Disconnected)
    {
        if(!connected_ || ready_ || failed_ || !Request || !Disconnected) return false;
        if(parentWindow_)
        {
            DWORD Pid=0; GetWindowThreadProcessId(reinterpret_cast<HWND>(ChildWindow),&Pid);
            if(Pid!=GetCurrentProcessId() || !IsChild(reinterpret_cast<HWND>(parentWindow_),reinterpret_cast<HWND>(ChildWindow))) return false;
        }
        else if(ChildWindow) return false;
        request_=std::move(Request); disconnected_=std::move(Disconnected); ready_=true;
        HostFrame Frame; Frame.type_=0; Frame.window_=ChildWindow; Frame.connection_=connection_;
        if(socket_.write(reinterpret_cast<const char*>(&Frame),sizeof(Frame))!=sizeof(Frame)) { fail(); return false; }
        socket_.flush(); return true;
    }
    void DocumentWebPeer::fail()
    {
        if(failed_) return;
        failed_=true; ready_=false; ownerTimer_.stop(); socket_.abort(); buffered_.clear();
        if(disconnected_) disconnected_();
    }
    void DocumentWebPeer::receive()
    {
        if(!ready_ || failed_) { fail(); return; }
        const auto Budget=qsizetype(HostPayloadLimit+sizeof(HostFrame));
        buffered_+=socket_.read(Budget-buffered_.size());
        if(buffered_.size()<qsizetype(sizeof(HostFrame))) return;
        HostFrame Frame; std::memcpy(&Frame,buffered_.constData(),sizeof(Frame));
        if(!ValidFrame(Frame,connection_) || Frame.type_!=1 || Frame.window_ || lastRequest_==UINT64_MAX
            || Frame.requestId_!=lastRequest_+1) { fail(); return; }
        const auto Size=qsizetype(sizeof(Frame))+Frame.length_;
        if(buffered_.size()<Size) return;
        // One outstanding exchange: queued/pipelined frames are not accepted.
        if(buffered_.size()!=Size || socket_.bytesAvailable()) { fail(); return; }
        lastRequest_=Frame.requestId_;
        const auto Reply=request_(buffered_.mid(sizeof(Frame))); buffered_.clear();
        if(failed_) return;
        if(Reply.size()>HostPayloadLimit || socket_.bytesToWrite()+sizeof(Frame)+Reply.size()>Budget) { fail(); return; }
        Frame.type_=2; Frame.length_=std::uint32_t(Reply.size());
        if(socket_.write(reinterpret_cast<const char*>(&Frame),sizeof(Frame))!=sizeof(Frame)
            || socket_.write(Reply)!=Reply.size()) { fail(); return; }
        socket_.flush();
    }
}
