#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QTimer>
#include <functional>
#include <memory>

// A separate endpoint per executable, settings and explicit appearance keeps
// portable installations and caller-specific themes independent.
class EditorInstance final : public QObject
{
public:
    enum Result { Primary, Forwarded, Failed };
    explicit EditorInstance(QObject* Parent=nullptr):QObject(Parent),server_(this){}
    Result Start(const QByteArray& Scope,const QStringList& Files)
    {
        const auto Name="HyperEditor-"+QString::fromLatin1(QCryptographicHash::hash(Scope,QCryptographicHash::Sha256).toHex());
        QLockFile Lock(QDir::temp().filePath(Name+".lock"));
        if(!Lock.tryLock(5000))return Failed;
        QLocalSocket Peer;Peer.connectToServer(Name);
        if(Peer.waitForConnected(300)){
            QJsonArray Paths;for(const auto& File:Files)Paths.append(File.isEmpty()?QString{}:QFileInfo(File).absoluteFilePath());
            const auto Message=QJsonDocument(Paths).toJson(QJsonDocument::Compact)+'\n';
            if(Message.size()>1024*1024)return Failed;
            Peer.write(Message);if(!Peer.waitForBytesWritten(5000))return Failed;
            if(!Peer.waitForReadyRead(10000))return Failed;
            return Peer.readAll()=="ok\n"?Forwarded:Failed;
        }
        server_.setSocketOptions(QLocalServer::UserAccessOption);
        if(!server_.listen(Name))return Failed;
        connect(&server_,&QLocalServer::newConnection,this,[this]{
            while(auto* Peer=server_.nextPendingConnection()){
                Peer->setReadBufferSize(1024*1024+1);
                auto Buffer=std::make_shared<QByteArray>();
                connect(Peer,&QLocalSocket::disconnected,Peer,&QObject::deleteLater);
                QTimer::singleShot(10000,Peer,[Peer]{Peer->disconnectFromServer();});
                connect(Peer,&QLocalSocket::readyRead,this,[this,Peer,Buffer]{
                    *Buffer+=Peer->readAll();
                    if(Buffer->size()>1024*1024){Peer->abort();return;}
                    if(!Buffer->endsWith('\n'))return;
                    const auto Document=QJsonDocument::fromJson(*Buffer);QStringList Files;
                    if(!Document.isArray() || Document.array().size()>256){Peer->abort();return;}
                    for(const auto& Value:Document.array()){
                        if(!Value.isString()){Peer->abort();return;}
                        const auto Path=Value.toString();
                        if(Path.contains(QChar::Null)||(!Path.isEmpty()&&!QFileInfo(Path).isAbsolute())){Peer->abort();return;}
                        Files.append(Path);
                    }
                    if(!open_){Peer->abort();return;}
                    // Acceptance acknowledges a queued request, including errors
                    // that will be shown in the owning document window.
                    QTimer::singleShot(0,this,[this,Files]{open_(Files);});
                    Peer->write("ok\n");Peer->disconnectFromServer();
                });
            }
        });
        return Primary;
    }
    void SetOpenHandler(std::function<void(QStringList)> Open){open_=std::move(Open);}
private:
    QLocalServer server_;
    std::function<void(QStringList)> open_;
};
