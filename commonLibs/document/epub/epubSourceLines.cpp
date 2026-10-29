#include "stdafx.h"
#include "epubSourceLines.hpp"
#include <algorithm>
#include <string_view>
#include <vector>

namespace hc::document::epub {
SourceLines AnnotateSourceLines(const QByteArray& Xhtml) {
    SourceLines Result;Result.bytes_=Xhtml;
    const auto Skip=[&](const char* Reason){Result.skipped_=QString::fromLatin1(Reason);return Result;};
    if(Xhtml.size()>2*1024*1024)return Skip("size");
    // Byte offsets only make sense for an 8-bit encoding; UTF-16 shows as a BOM, a declaration or early NUL bytes.
    const auto Head=Xhtml.left(256);
    if(Head.startsWith("\xFF\xFE") || Head.startsWith("\xFE\xFF") || Head.left(64).contains('\0'))return Skip("utf-16");
    if(Head.startsWith("<?xml") && Head.left(qMax<qsizetype>(0,Head.indexOf("?>"))).toLower().contains("utf-16"))return Skip("utf-16");
    struct Element {qsizetype begin_=0,tagEnd_=0,end_=0;bool empty_=false;};std::vector<Element> Elements;std::vector<size_t> Stack;bool Existing=false;
    const bool Scanned=detail::ScanMarkup(Xhtml.constData(),Xhtml.size(),[&](const detail::MarkupToken& Token){
        if(Elements.size()>32768 || Stack.size()>128)return false;
        if(Token.kind_==detail::MarkupToken::End){if(Stack.empty())return false;Elements[Stack.back()].end_=Token.end_;Stack.pop_back();return true;}
        if(Token.kind_!=detail::MarkupToken::Start && Token.kind_!=detail::MarkupToken::Empty)return true;
        // A second data-hc- attribute is a well-formedness error that would blank the whole chapter.
        if(std::string_view(Xhtml.constData()+Token.begin_,size_t(Token.end_-Token.begin_)).find("data-hc-")!=std::string_view::npos)Existing=true;
        const bool Empty=Token.kind_==detail::MarkupToken::Empty;Elements.push_back({Token.begin_,Token.end_-1,Token.end_,Empty});
        if(!Empty)Stack.push_back(Elements.size()-1);
        return true;});
    if(!Scanned || !Stack.empty())return Skip("markup");
    if(Existing)return Skip("existing-attribute");
    const auto NameIs=[&](const Element& Value,std::string_view Name){
        const std::string_view Tag(Xhtml.constData()+Value.begin_+1,size_t(Value.tagEnd_-Value.begin_-1));
        return Tag.substr(0,Name.size())==Name && (Tag.size()==Name.size() || std::string_view(" \t\r\n/>").find(Tag[Name.size()])!=std::string_view::npos);};
    const auto Body=std::find_if(Elements.begin(),Elements.end(),[&](const Element& Value){return NameIs(Value,"body");});
    if(Body==Elements.end() || Body->empty_)return Skip("no-body");
    std::vector<qsizetype> Starts{0};
    for(qsizetype I=0;I<Xhtml.size();++I){if(Xhtml[I]=='\r'){if(I+1<Xhtml.size() && Xhtml[I+1]=='\n')++I;Starts.push_back(I+1);}else if(Xhtml[I]=='\n')Starts.push_back(I+1);}
    const auto LineOf=[&](qsizetype Offset){return qsizetype(std::upper_bound(Starts.begin(),Starts.end(),Offset)-Starts.begin());};
    QByteArray Out;Out.reserve(Xhtml.size()+qsizetype(Elements.size())*24);qsizetype Copied=0;int Count=0;
    for(auto At=Body+1;At!=Elements.end() && At->begin_<Body->end_;++At) {
        // Inside "/>" the attribute goes before the slash.
        const qsizetype Insert=At->empty_?At->tagEnd_-1:At->tagEnd_;const auto Start=LineOf(At->begin_),Stop=LineOf(At->end_-1);
        Out.append(Xhtml.constData()+Copied,Insert-Copied);Out.append(" data-hc-l=\"").append(QByteArray::number(Start)).append('"');
        if(Stop!=Start)Out.append(" data-hc-e=\"").append(QByteArray::number(Stop)).append('"');
        Copied=Insert;++Count;
    }
    Out.append(Xhtml.constData()+Copied,Xhtml.size()-Copied);
    Result.bytes_=std::move(Out);Result.elements_=Count;return Result;
}
}
