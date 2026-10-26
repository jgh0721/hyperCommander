#pragma once
#include <QImage>
#include <QtEndian>
#include <zlib.h>
#include <cmath>
#include <cstring>

namespace hc::document
{
    inline QImage DecodePngPreview(const QByteArray& Bytes)
    {
        if(Bytes.size()<33 || !Bytes.startsWith(QByteArray("\x89PNG\r\n\x1a\n",8)) || Bytes.mid(12,4)!="IHDR")return {};
        const char* Header=Bytes.constData()+16;
        if(qFromBigEndian<quint32>(Bytes.constData()+8)!=13 || quint32(crc32(0,reinterpret_cast<const Bytef*>(Bytes.constData()+12),17))!=qFromBigEndian<quint32>(Bytes.constData()+29))return {};
        const auto Width=qFromBigEndian<quint32>(Header),Height=qFromBigEndian<quint32>(Header+4);
        const int Type=quint8(Header[9]);
        const int Channels=Type==0||Type==3?1:Type==2?3:Type==4?2:Type==6?4:0;
        if(!Width||!Height||Width>16384||Height>16384||quint64(Width)*Height>16*1024*1024
            || quint8(Header[8])!=8 || Header[10] || Header[11] || Header[12] || !Channels)return {};
        QImage Result(int(Width),int(Height),QImage::Format_RGBA8888);if(Result.isNull())return {};Result.fill(Qt::transparent);
        const int RowBytes=int(Width)*Channels;
        QByteArray Row(RowBytes+1,Qt::Uninitialized),Previous(RowBytes,'\0'),Palette,Alpha;
        z_stream Stream{};if(inflateInit(&Stream)!=Z_OK)return {};
        struct Guard {z_stream* stream_;~Guard(){inflateEnd(stream_);}} Guard{&Stream};
        int Filled=0,Rows=0,Status=Z_OK;bool Invalid=false;
        for(qsizetype Position=33;Position+8<=Bytes.size() && Rows<int(Height) && !Invalid;){
            const auto Length=qFromBigEndian<quint32>(Bytes.constData()+Position);
            const auto Name=Bytes.mid(Position+4,4);
            const auto Available=qMin(qsizetype(Length),Bytes.size()-Position-8);
            const char* Data=Bytes.constData()+Position+8;
            if(Name=="PLTE" && Available==Length && Length<=768 && Length%3==0)Palette=QByteArray(Data,Length);
            if(Name=="tRNS" && Available==Length && Length<=256)Alpha=QByteArray(Data,Length);
            if(Name=="IDAT"){
                if(Type==3 && Palette.isEmpty())break;
                Stream.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(Data));Stream.avail_in=uInt(Available);
                do{
                    Stream.next_out=reinterpret_cast<Bytef*>(Row.data()+Filled);Stream.avail_out=uInt(Row.size()-Filled);
                    Status=inflate(&Stream,Z_NO_FLUSH);Filled=int(Row.size())-int(Stream.avail_out);
                    if(Filled==Row.size()){
                        const int Filter=quint8(Row[0]);if(Filter>4){Invalid=true;break;}
                        for(int X=0;X<RowBytes;++X){
                            const int Left=X>=Channels?quint8(Row[1+X-Channels]):0,Up=quint8(Previous[X]),Corner=X>=Channels?quint8(Previous[X-Channels]):0;
                            int Predict=0;
                            if(Filter==1)Predict=Left;else if(Filter==2)Predict=Up;else if(Filter==3)Predict=(Left+Up)/2;
                            else if(Filter==4){const int P=Left+Up-Corner,A=std::abs(P-Left),B=std::abs(P-Up),C=std::abs(P-Corner);Predict=A<=B&&A<=C?Left:B<=C?Up:Corner;}
                            Row[1+X]=char((quint8(Row[1+X])+Predict)&255);
                        }
                        auto* Pixel=Result.scanLine(Rows);
                        for(quint32 X=0;X<Width;++X){
                            const auto* Source=reinterpret_cast<const uchar*>(Row.constData()+1+X*Channels);
                            if(Type==3){const int I=Source[0];if(I*3+2>=Palette.size()){Invalid=true;break;}
                                Pixel[0]=quint8(Palette[I*3]);Pixel[1]=quint8(Palette[I*3+1]);Pixel[2]=quint8(Palette[I*3+2]);Pixel[3]=I<Alpha.size()?quint8(Alpha[I]):255;
                            }else if(Type==0||Type==4){Pixel[0]=Pixel[1]=Pixel[2]=Source[0];Pixel[3]=Type==4?Source[1]:255;}
                            else{Pixel[0]=Source[0];Pixel[1]=Source[1];Pixel[2]=Source[2];Pixel[3]=Type==6?Source[3]:255;}
                            Pixel+=4;
                        }
                        if(Invalid){memset(Result.scanLine(Rows),0,size_t(Result.bytesPerLine()));break;}
                        Previous=Row.mid(1);Filled=0;++Rows;
                    }
                    if(Status!=Z_OK || Rows==int(Height))break;
                }while(Stream.avail_in || !Stream.avail_out);
                if(Status!=Z_OK && Status!=Z_BUF_ERROR && Status!=Z_STREAM_END)break;
            }
            if(quint64(Position)+12+Length>quint64(Bytes.size()))break;
            Position+=12+Length;
        }
        if(!Rows)return {};
        if(Result.width()>1024||Result.height()>1024)Result=Result.scaled(1024,1024,Qt::KeepAspectRatio);Result.setText("PreviewRows",QString::number(Rows));return Result;
    }
}
