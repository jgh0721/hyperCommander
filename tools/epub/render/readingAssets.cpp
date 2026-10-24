#include "stdafx.h"
#include "readingAssets.hpp"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QRegularExpression>
#include <QSaveFile>
namespace hc::epub::poc {
namespace {
constexpr qint64 MaxBytes=8*1024*1024;
QString directory(const QString& Settings) { return QDir(QFileInfo(Settings).absolutePath()).filePath("epub-assets"); }
QByteArray read(const QString& Path) {
    QFile File(Path);
    if(!File.open(QIODevice::ReadOnly) || File.size()>MaxBytes) return {};
    const auto Data=File.read(MaxBytes+1); return Data.size()<=MaxBytes ? Data : QByteArray{};
}
bool dimensions(const QSize& Size) { return Size.isValid() && Size.width()<=4096 && Size.height()<=4096 && qint64(Size.width())*Size.height()<=4194304; }
}
ReadingAsset ReadingAssets::Import(const QString& Input,const QString& Settings) {
    if(Settings.isEmpty()) return {{},QStringLiteral("설정 저장 경로가 없는 모드에서는 이미지를 보관할 수 없습니다.")};
    const auto Data=read(Input); QBuffer Buffer; Buffer.setData(Data); Buffer.open(QIODevice::ReadOnly);
    QImageReader Reader(&Buffer); Reader.setAutoTransform(true); const auto Format=Reader.format();
    if(Data.isEmpty() || !QList<QByteArray>{"png","jpeg","bmp"}.contains(Format) || !dimensions(Reader.size()))
        return {{},QStringLiteral("배경은 8 MiB·400만 화소·한 변 4096 픽셀 이하의 PNG/JPEG/BMP를 사용하세요.")};
    QImageReader::setAllocationLimit(32); const auto Image=Reader.read();
    if(Image.isNull() || !dimensions(Image.size())) return {{},QStringLiteral("배경 이미지를 읽을 수 없습니다.")};
    QByteArray Encoded; QBuffer Output(&Encoded); Output.open(QIODevice::WriteOnly);
    if(!Image.save(&Output,"PNG") || Encoded.size()>MaxBytes) return {{},QStringLiteral("변환한 배경 이미지가 8 MiB를 초과합니다.")};
    const auto Id=QString::fromLatin1(QCryptographicHash::hash(Encoded,QCryptographicHash::Sha256).toHex());
    const auto Directory=directory(Settings); QDir Folder(Directory); const auto Path=Folder.filePath("bg-"+Id+".png");
    if(QFileInfo(Directory).isSymLink() || QFileInfo(Path).isSymLink()) return {{},QStringLiteral("연결된 자산 경로에는 저장하지 않습니다.")};
    if(!QDir().mkpath(Directory)) return {{},QStringLiteral("배경 보관 폴더를 만들 수 없습니다.")};
    if(!QFileInfo::exists(Path)) {
        qint64 Bytes=0; const auto Files=Folder.entryInfoList(QStringList{"bg-*.png"},QDir::Files);
        for(const auto& File:Files) Bytes+=File.size();
        if(Files.size()>=128 || Bytes+Encoded.size()>128*1024*1024) return {{},QStringLiteral("배경 보관 한도(128개·128 MiB)를 초과했습니다.")};
    }
    QSaveFile File(Path); File.setDirectWriteFallback(false);
    if(!File.open(QIODevice::WriteOnly) || File.write(Encoded)!=Encoded.size() || !File.commit()) return {{},QStringLiteral("배경 이미지를 보관하지 못했습니다.")};
    return {Id,{}};
}
QByteArray ReadingAssets::Load(const QString& Settings,const QString& Id,QString& Error) {
    if(Settings.isEmpty() || !QRegularExpression("^[0-9a-f]{64}$").match(Id).hasMatch()) { Error=QStringLiteral("배경 식별자가 올바르지 않습니다."); return {}; }
    const auto Data=read(QDir(directory(Settings)).filePath("bg-"+Id+".png"));
    QBuffer Buffer; Buffer.setData(Data); Buffer.open(QIODevice::ReadOnly); QImageReader Reader(&Buffer);
    if(Data.isEmpty() || QCryptographicHash::hash(Data,QCryptographicHash::Sha256).toHex()!=Id.toLatin1() || Reader.format()!="png" || !dimensions(Reader.size())) {
        Error=QStringLiteral("배경 이미지가 없거나 손상되어 테마 배경으로 표시합니다."); return {};
    }
    Error.clear(); return Data;
}
}
