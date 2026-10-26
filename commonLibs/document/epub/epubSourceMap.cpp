#include "stdafx.h"
#include "epubSourceMap.hpp"
#include "epubArchive.hpp"
#include <QJsonArray>
#include <QXmlStreamReader>
#include <algorithm>

namespace hc::document::epub {
bool SourceMap::Load(const QString& Source) {
    spans_.clear();if(Source.size()>2*1024*1024)return false;
    QXmlStreamReader Reader(Source);detail::XhtmlEntities Entities;Entities.enabled_=true;Reader.setEntityResolver(&Entities);
    int Nodes=0,Depth=0;while(!Reader.atEnd()){const auto Token=Reader.readNext();if(Token==QXmlStreamReader::StartElement && (++Nodes>32768 || ++Depth>128))return false;if(Token==QXmlStreamReader::EndElement)--Depth;if(Token==QXmlStreamReader::DTD && !detail::PassiveDoctype(Reader))return false;}if(Reader.hasError())return false;
    struct Frame {int span_,elements_=0,children_=0;};QList<Frame> Stack;
    const auto Text=[&](int Begin,int End,bool Cdata){
        if(Stack.isEmpty() || Begin==End)return;
        Span Value;Value.path_=spans_[Stack.last().span_].path_;Value.child_=Stack.last().children_++;Value.begin_=Begin;Value.end_=End;
        for(int I=Begin;I<End;) {
            const int Start=I;int Units=1;
            if(!Cdata && Source[I]=='&') {
                const int Semi=Source.indexOf(';',I+1);if(Semi<0 || Semi>=End)return;
                const auto Entity=Source.mid(I+1,Semi-I-1);if(Entity.startsWith('#')){bool Ok=false;const auto Point=Entity.mid(Entity.startsWith("#x")?2:1).toUInt(&Ok,Entity.startsWith("#x")?16:10);if(!Ok)return;Units=Point>0xffff?2:1;}I=Semi+1;
            }else if(Source[I]=='\r'){++I;if(I<End && Source[I]=='\n')++I;}else ++I;
            for(int J=0;J<Units;++J)Value.offsets_.append(Start);
        }
        Value.offsets_.append(End);spans_.append(std::move(Value));
    };
    for(int I=0;I<Source.size();) {
        if(spans_.size()>32768 || Stack.size()>128){spans_.clear();return false;}
        if(Source[I]!='<'){int End=Source.indexOf('<',I);if(End<0)End=Source.size();Text(I,End,false);I=End;continue;}
        if(Source.mid(I,4)=="<!--"){const int End=Source.indexOf("-->",I+4);if(End<0)return false;if(!Stack.isEmpty())++Stack.last().children_;I=End+3;continue;}
        if(Source.mid(I,9)=="<![CDATA["){const int End=Source.indexOf("]]>",I+9);if(End<0)return false;Text(I+9,End,true);I=End+3;continue;}
        int End=I+1,Brackets=0;QChar Quote;
        for(;End<Source.size();++End){const auto C=Source[End];if(!Quote.isNull()){if(C==Quote)Quote={};}else if(C=='\'' || C=='"')Quote=C;else if(C=='[')++Brackets;else if(C==']')--Brackets;else if(C=='>' && !Brackets)break;}
        if(End>=Source.size())return false;
        if(Source[I+1]=='/'){if(Stack.isEmpty())return false;spans_[Stack.takeLast().span_].end_=End+1;}
        else if(Source[I+1]!='!' && Source[I+1]!='?') {
            Span Value;Value.begin_=I;Value.end_=End+1;
            if(!Stack.isEmpty()){Value.path_=spans_[Stack.last().span_].path_;Value.path_.append(Stack.last().elements_++);++Stack.last().children_;}
            spans_.append(std::move(Value));if(Source[End-1]!='/')Stack.append({int(spans_.size()-1)});
        }else if(Source[I+1]=='?' && !Stack.isEmpty())++Stack.last().children_;
        I=End+1;
    }
    return Stack.isEmpty() && !spans_.isEmpty();
}
QJsonObject SourceMap::Anchor(int Position) const {
    const Span* Best=nullptr;for(const auto& Span:spans_)if(Position>=Span.begin_ && Position<Span.end_ && (!Best || Span.end_-Span.begin_<Best->end_-Best->begin_))Best=&Span;
    if(!Best)return {};int Offset=0;
    if(!Best->offsets_.isEmpty()){const auto Upper=std::upper_bound(Best->offsets_.begin(),Best->offsets_.end(),Position);if(Upper!=Best->offsets_.begin())Offset=int(std::lower_bound(Best->offsets_.begin(),Best->offsets_.end(),*(Upper-1))-Best->offsets_.begin());}
    return {{"path",Best->path_},{"child",Best->child_},{"offset",Offset}};
}
int SourceMap::Position(const QJsonObject& Anchor) const {
    if(!Anchor["path"].isArray() || Anchor["path"].toArray().size()>128)return -1;
    for(const auto& Span:spans_)if(Span.path_==Anchor["path"].toArray() && Span.child_==Anchor["child"].toInt(-1)) {
        const int Offset=Anchor["offset"].toInt(-1);if(Span.child_>=0)return Offset>=0 && Offset<Span.offsets_.size()?Span.offsets_[Offset]:-1;return Span.begin_;
    }
    return -1;
}
}
