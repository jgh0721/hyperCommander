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
namespace {
// Element children of <body> as byte ranges. Only whitespace may lie between them: a text node or
// a comment directly in <body> has no element to replace, so such a chapter is not split.
struct BodyBlocks {bool ok_=false;qsizetype open_=0,close_=0;QByteArray namespaces_;std::vector<std::pair<qsizetype,qsizetype>> blocks_;};
bool TagIs(const QByteArray& Xhtml,const detail::MarkupToken& Token,std::string_view Name) {
    const std::string_view Tag(Xhtml.constData()+Token.begin_+1,size_t(Token.end_-Token.begin_-1));
    return Tag.substr(0,Name.size())==Name && (Tag.size()==Name.size() || std::string_view(" \t\r\n/>").find(Tag[Name.size()])!=std::string_view::npos);
}
// xmlns and xmlns:prefix declarations of one start tag, each with a leading space.
QByteArray Namespaces(const QByteArray& Xhtml,const detail::MarkupToken& Token) {
    QByteArray Out;const std::string_view Tag(Xhtml.constData()+Token.begin_,size_t(Token.end_-Token.begin_));constexpr std::string_view Space(" \t\r\n");
    for(auto At=Tag.find("xmlns");At!=std::string_view::npos;At=Tag.find("xmlns",At+1)) {
        const auto Equal=Tag.find('=',At);if(Equal==std::string_view::npos)break;
        auto Name=Tag.substr(At,Equal-At);while(!Name.empty() && Space.find(Name.back())!=std::string_view::npos)Name.remove_suffix(1);
        if(Space.find(Tag[At-1])==std::string_view::npos || (Name!="xmlns" && !Name.starts_with("xmlns:")) || Name.find_first_of(Space)!=std::string_view::npos)continue;
        auto Quote=Equal+1;while(Quote<Tag.size() && Space.find(Tag[Quote])!=std::string_view::npos)++Quote;
        if(Quote>=Tag.size() || (Tag[Quote]!='"' && Tag[Quote]!='\''))continue;
        const auto End=Tag.find(Tag[Quote],Quote+1);if(End==std::string_view::npos)break;
        Out.append(' ').append(Name.data(),qsizetype(Name.size())).append('=').append(Tag.data()+Quote,qsizetype(End+1-Quote));At=End;
    }
    return Out;
}
BodyBlocks ScanBody(const QByteArray& Xhtml) {
    using Kind=detail::MarkupToken;BodyBlocks Out;int Depth=-1;qsizetype Begin=0;bool Html=false;
    detail::ScanMarkup(Xhtml.constData(),Xhtml.size(),[&](const detail::MarkupToken& Token){
        if(Depth<0) {
            if(Token.kind_==Kind::Start && !Html && TagIs(Xhtml,Token,"html")){Html=true;Out.namespaces_+=Namespaces(Xhtml,Token);}
            else if(Token.kind_==Kind::Start && TagIs(Xhtml,Token,"body")){Depth=0;Out.open_=Token.end_;Out.namespaces_+=Namespaces(Xhtml,Token);}
            return true;
        }
        if(Depth==0) switch(Token.kind_) {
            case Kind::End: Out.close_=Token.begin_;Out.ok_=TagIs(Xhtml,Token,"/body");return false;
            case Kind::Text: return std::string_view(Xhtml.constData()+Token.begin_,size_t(Token.end_-Token.begin_)).find_first_not_of(" \t\r\n")==std::string_view::npos;
            case Kind::Empty: Out.blocks_.push_back({Token.begin_,Token.end_});return Out.blocks_.size()<=32768;
            case Kind::Start: Begin=Token.begin_;Depth=1;return true;
            default: return false;
        }
        if(Token.kind_==Kind::Start)++Depth;
        else if(Token.kind_==Kind::End && --Depth==0){Out.blocks_.push_back({Begin,Token.end_});return Out.blocks_.size()<=32768;}
        return true;});
    return Out.ok_?Out:BodyBlocks{};
}
qint64 LineBreaks(const QByteArray& Xhtml,qsizetype Begin,qsizetype End) {
    qint64 Count=0;for(auto I=Begin;I<End;++I)if(Xhtml[I]=='\n' || (Xhtml[I]=='\r' && (I+1>=Xhtml.size() || Xhtml[I+1]!='\n')))++Count;return Count;
}
}
ElementPatch DiffBodyElements(const QByteArray& OldSource,const QByteArray& NewSource,const QByteArray& NewAnnotated) {
    const auto Old=ScanBody(OldSource),New=ScanBody(NewSource),Marked=ScanBody(NewAnnotated);
    // The head and the <body> tag are outside every element; a change there needs the whole chapter.
    if(!Old.ok_ || !New.ok_ || !Marked.ok_ || Marked.blocks_.size()!=New.blocks_.size() || OldSource.left(Old.open_)!=NewSource.left(New.open_)
        || OldSource.mid(Old.close_)!=NewSource.mid(New.close_))return {};
    const auto View=[](const QByteArray& Xhtml,qsizetype Begin,qsizetype End){return std::string_view(Xhtml.constData()+Begin,size_t(End-Begin));};
    const auto Gap=[&](const BodyBlocks& Body,const QByteArray& Xhtml,size_t Index){return View(Xhtml,Index?Body.blocks_[Index-1].second:Body.open_,Index<Body.blocks_.size()?Body.blocks_[Index].first:Body.close_);};
    const auto Block=[&](const BodyBlocks& Body,const QByteArray& Xhtml,size_t Index){return View(Xhtml,Body.blocks_[Index].first,Body.blocks_[Index].second);};
    const size_t OldCount=Old.blocks_.size(),NewCount=New.blocks_.size();
    // Unchanged leading elements keep their offsets, so their line attributes stay correct.
    size_t Prefix=0;
    while(Prefix<OldCount && Prefix<NewCount && Gap(Old,OldSource,Prefix)==Gap(New,NewSource,Prefix) && Block(Old,OldSource,Prefix)==Block(New,NewSource,Prefix))++Prefix;
    // Unchanged trailing elements keep their content and move by whole lines.
    size_t Suffix=0;
    while(Suffix<OldCount-Prefix && Suffix<NewCount-Prefix && Gap(Old,OldSource,OldCount-Suffix)==Gap(New,NewSource,NewCount-Suffix)
        && Block(Old,OldSource,OldCount-1-Suffix)==Block(New,NewSource,NewCount-1-Suffix))++Suffix;
    ElementPatch Result;Result.valid_=true;Result.from_=int(Prefix);Result.removed_=int(OldCount-Suffix-Prefix);Result.inserted_=int(NewCount-Suffix-Prefix);
    Result.blocks_=int(NewCount);Result.namespaces_=New.namespaces_;
    if(Result.inserted_>0)Result.html_=NewAnnotated.mid(Marked.blocks_[Prefix].first,Marked.blocks_[NewCount-Suffix-1].second-Marked.blocks_[Prefix].first);
    if(Suffix>0) {
        const auto Anchor=Prefix?Old.blocks_[Prefix-1].second:Old.open_;
        Result.shift_=LineBreaks(NewSource,Anchor,New.blocks_[NewCount-Suffix].first)-LineBreaks(OldSource,Anchor,Old.blocks_[OldCount-Suffix].first);
    }
    return Result;
}
}
