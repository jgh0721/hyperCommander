#include "documentTextCodec.hpp"
#include <QCoreApplication>
#include <QStringConverter>
#include <climits>
#include <optional>
#include <unordered_map>
#include <vector>
#include <Windows.h>
namespace hc::document
{
    namespace
    {
        storage::Error Invalid(std::string Code) {return {storage::ErrorCategory::InvalidRequest,std::move(Code)};}
        // Legacy encodings go through the Windows NLS tables. tag_ completes the error codes
        // "document-decode-invalid-<tag>" and "document-encode-<tag>-failed"; CP949 keeps its original codes.
        struct LegacyEncoding { const char* name_; UINT codePage_; const char* tag_; };
        constexpr LegacyEncoding legacyEncodings[]{{"CP949",949,"cp949"},{"Shift_JIS",932,"shift-jis"},{"GB18030",54936,"gb18030"},
            {"ISO-8859-1",28591,"iso-8859-1"},{"System",CP_ACP,"system"}};
        const LegacyEncoding* FindLegacy(const QString& Name) {for(const auto& Entry:legacyEncodings)if(Name==QLatin1String(Entry.name_))return &Entry;return nullptr;}
        // "System" follows the active ANSI code page; resolve it so the flag rules see the real page (65001 included).
        UINT CodePageOf(const LegacyEncoding& Entry) {return Entry.codePage_==CP_ACP?GetACP():Entry.codePage_;}
        // Flag rules from the MultiByteToWideChar/WideCharToMultiByte remarks:
        // * 65001 and 54936 take only MB_/WC_ERR_INVALID_CHARS and no default-character arguments.
        //   GB18030 maps every scalar, so only lone surrogates can fail there.
        // * 50220-50229, 52936, 57002-57011, 65000 and 42 take no flags. Nothing reports loss or invalid
        //   input for them, so the result is verified by converting it back.
        // * Every other page takes MB_ERR_INVALID_CHARS, and WC_NO_BEST_FIT_CHARS with lpUsedDefaultChar
        //   reports each character that would not survive the round trip.
        // A page that still rejects the strict flags falls back to the verified flagless conversion.
        enum class CodePageRule { Substitution, Unicode, Plain };
        CodePageRule RuleOf(UINT CodePage)
        {
            if(CodePage==CP_UTF8||CodePage==54936)return CodePageRule::Unicode;
            if((CodePage>=50220&&CodePage<=50229)||CodePage==52936||(CodePage>=57002&&CodePage<=57011)||CodePage==CP_UTF7||CodePage==42)return CodePageRule::Plain;
            return CodePageRule::Substitution;
        }
        bool FlagsRejected(DWORD Error) {return Error==ERROR_INVALID_FLAGS||Error==ERROR_INVALID_PARAMETER;}
        std::optional<QString> RawDecode(QByteArrayView Bytes,UINT CodePage,DWORD Flags,DWORD& Error)
        {
            Error=0;QString Text;if(Bytes.isEmpty())return Text;
            const auto Length=MultiByteToWideChar(CodePage,Flags,Bytes.constData(),int(Bytes.size()),nullptr,0);
            if(Length<=0){Error=GetLastError();return std::nullopt;}
            Text.resize(Length);
            if(MultiByteToWideChar(CodePage,Flags,Bytes.constData(),int(Bytes.size()),reinterpret_cast<wchar_t*>(Text.data()),Length)!=Length){Error=GetLastError();return std::nullopt;}
            return Text;
        }
        // Lossy reports a substituted character (Report) or one the page refused to translate.
        std::optional<QByteArray> RawEncode(QStringView Text,UINT CodePage,DWORD Flags,bool Report,bool& Lossy,DWORD& Error)
        {
            Lossy=false;Error=0;QByteArray Bytes;if(Text.isEmpty())return Bytes;
            const auto* Source=reinterpret_cast<const wchar_t*>(Text.utf16());BOOL Used=FALSE;
            auto Length=WideCharToMultiByte(CodePage,Flags,Source,int(Text.size()),nullptr,0,nullptr,Report?&Used:nullptr);
            if(Length>0){Bytes.resize(Length);if(WideCharToMultiByte(CodePage,Flags,Source,int(Text.size()),Bytes.data(),Length,nullptr,Report?&Used:nullptr)!=Length)Length=0;}
            if(Length<=0){Error=GetLastError();Lossy=Error==ERROR_NO_UNICODE_TRANSLATION;return std::nullopt;}
            Lossy=Used!=FALSE;return Bytes;
        }
        enum class Conversion { Exact, Lossy, Failed };
        Conversion EncodeCodePage(QStringView Text,UINT CodePage,QByteArray& Bytes)
        {
            if(Text.size()>INT_MAX)return Conversion::Failed;
            bool Lossy=false;DWORD Error=0;const auto Rule=RuleOf(CodePage);
            if(Rule!=CodePageRule::Plain) {
                const bool Report=Rule==CodePageRule::Substitution;
                auto Result=RawEncode(Text,CodePage,Report?WC_NO_BEST_FIT_CHARS:WC_ERR_INVALID_CHARS,Report,Lossy,Error);
                if(Lossy)return Conversion::Lossy;
                if(Result) {
                    // 54936 cannot report substitution. GB18030 covers Unicode; the check guards the NLS table only.
                    if(Rule==CodePageRule::Unicode&&CodePage!=CP_UTF8) {
                        const auto Back=RawDecode(*Result,CodePage,MB_ERR_INVALID_CHARS,Error);
                        if(!Back||QStringView(*Back)!=Text)return Conversion::Lossy;
                    }
                    Bytes=std::move(*Result);return Conversion::Exact;
                }
                if(!FlagsRejected(Error))return Conversion::Failed;
            }
            auto Result=RawEncode(Text,CodePage,0,false,Lossy,Error);if(!Result)return Lossy?Conversion::Lossy:Conversion::Failed;
            const auto Back=RawDecode(*Result,CodePage,0,Error);if(!Back||QStringView(*Back)!=Text)return Conversion::Lossy;
            Bytes=std::move(*Result);return Conversion::Exact;
        }
        std::optional<QString> DecodeCodePage(QByteArrayView Bytes,UINT CodePage)
        {
            if(Bytes.size()>INT_MAX)return std::nullopt;
            DWORD Error=0;
            if(RuleOf(CodePage)!=CodePageRule::Plain) {auto Text=RawDecode(Bytes,CodePage,MB_ERR_INVALID_CHARS,Error);if(Text||!FlagsRejected(Error))return Text;}
            // Without MB_ERR_INVALID_CHARS a page substitutes silently; only an exact round trip proves the bytes valid.
            auto Text=RawDecode(Bytes,CodePage,0,Error);QByteArray Again;
            if(!Text||EncodeCodePage(*Text,CodePage,Again)!=Conversion::Exact||QByteArrayView(Again)!=Bytes)return std::nullopt;
            return Text;
        }
    }
    storage::Result<DecodedText> DecodeDocumentText(const QByteArray& Bytes,const QString& Encoding)
    {
        DecodedText Result;
        const bool Utf8Bom=Bytes.startsWith("\xef\xbb\xbf"),Utf16LeBom=Bytes.startsWith("\xff\xfe"),Utf16BeBom=Bytes.startsWith("\xfe\xff");
        if(Encoding.isEmpty()) {
            Result.encoding_=Utf16LeBom?"UTF-16LE":Utf16BeBom?"UTF-16BE":"UTF-8";
            Result.bom_=Utf8Bom||Utf16LeBom||Utf16BeBom;Result.detection_=Result.bom_?TextDetection::Bom:TextDetection::Utf8Strict;
        } else {
            Result.encoding_=CanonicalEncodingName(Encoding);
            if(Result.encoding_.isEmpty())return Invalid("document-encoding-unavailable");
            // A byte order mark belongs to the UTF encoding it introduces. Read as another encoding the
            // same bytes are content, so the next save must write them back instead of a different mark.
            Result.bom_=Result.encoding_=="UTF-8"?Utf8Bom:Result.encoding_=="UTF-16LE"?Utf16LeBom:Result.encoding_=="UTF-16BE"&&Utf16BeBom;
        }
        if(const auto* Legacy=FindLegacy(Result.encoding_)) {
            auto Text=DecodeCodePage(Bytes,CodePageOf(*Legacy));
            if(!Text)return Invalid(std::string("document-decode-invalid-")+Legacy->tag_);
            Result.text_=std::move(*Text);
        } else {
            const auto Kind=Result.encoding_=="UTF-8"?QStringConverter::Utf8:Result.encoding_=="UTF-16LE"?QStringConverter::Utf16LE:QStringConverter::Utf16BE;
            if(Kind!=QStringConverter::Utf8&&Bytes.size()%2)return Invalid("document-decode-truncated-utf16");
            QStringDecoder Decoder(Kind);Result.text_=Decoder(Bytes);
            if(Decoder.hasError()) {
                // Invalid UTF-8 can be a legacy Korean file. Validate CP949
                // strictly; never accept replacement characters from a decoder.
                if(Encoding.isEmpty()&&!Result.bom_) {
                    auto Fallback=DecodeDocumentText(Bytes,"CP949");
                    if(Fallback.HasValue())Fallback.Value().detection_=TextDetection::Cp949Fallback;
                    return Fallback;
                }
                return Invalid("document-decode-invalid");
            }
            // UTF-8 shortens every non-ASCII sequence, so equal lengths mean the bytes were ASCII only.
            if(Encoding.isEmpty()&&!Result.bom_&&Result.text_.size()==Bytes.size())Result.detection_=TextDetection::AsciiOnly;
        }
        if(Result.text_.contains(QChar::Null)||!Result.text_.isValidUtf16())return Invalid("document-text-nul-or-surrogate");
        return Result;
    }
    storage::Result<QByteArray> EncodeDocumentText(const QString& Text,const QString& Encoding,bool Bom)
    {
        if(!Text.isValidUtf16()||Text.contains(QChar::Null))return Invalid("document-encode-invalid-text");
        const auto Name=CanonicalEncodingName(Encoding);
        if(const auto* Legacy=FindLegacy(Name)) {
            QByteArray Bytes;
            switch(EncodeCodePage(Text,CodePageOf(*Legacy),Bytes)) {
                case Conversion::Exact:return Bytes;
                case Conversion::Lossy:return Invalid("document-encoding-unrepresentable");
                case Conversion::Failed:break;
            }
            return Invalid(std::string("document-encode-")+Legacy->tag_+"-failed");
        }
        const auto Kind=Name=="UTF-8"?QStringConverter::Utf8:Name=="UTF-16LE"?QStringConverter::Utf16LE:
            Name=="UTF-16BE"?QStringConverter::Utf16BE:QStringConverter::System;
        if(Kind==QStringConverter::System)return Invalid("document-encoding-unavailable");
        QStringEncoder Encoder(Kind,Bom?QStringConverter::Flag::WriteBom:QStringConverter::Flag::Default);
        QByteArray Bytes=Encoder(Text);if(Encoder.hasError())return Invalid("document-encoding-unrepresentable");return Bytes;
    }
    QString CanonicalEncodingName(const QString& Encoding)
    {
        static constexpr const char* Names[]{"UTF-8","UTF-16LE","UTF-16BE","CP949","Shift_JIS","GB18030","ISO-8859-1","System"};
        static constexpr const char* Aliases[][2]{{"EUC-KR","CP949"},{"EUC-KR (CP949)","CP949"},{"CP932","Shift_JIS"},{"Shift-JIS","Shift_JIS"},
            {"SJIS","Shift_JIS"},{"CP54936","GB18030"},{"CP28591","ISO-8859-1"},{"Latin1","ISO-8859-1"},{"Latin-1","ISO-8859-1"}};
        if(Encoding.isEmpty())return {};
        for(const char* Name:Names)if(Encoding.compare(QLatin1String(Name),Qt::CaseInsensitive)==0)return QString::fromLatin1(Name);
        for(const auto& Alias:Aliases)if(Encoding.compare(QLatin1String(Alias[0]),Qt::CaseInsensitive)==0)return QString::fromLatin1(Alias[1]);
        // Menus show labels such as "시스템 기본값"; accept them as well.
        for(const auto& Info:ReadEncodings())if(Encoding==Info.label_)return Info.name_;
        return {};
    }
    TextEncodingInfo EncodingInfo(const QString& Encoding)
    {
        const auto Name=CanonicalEncodingName(Encoding);
        for(const auto& Info:ReadEncodings())if(!Name.isEmpty()&&Info.name_==Name)return Info;
        return {};
    }
    QList<TextEncodingInfo> ReadEncodings()
    {
        // The note under "System" names the page it resolves to on this machine.
        const auto Acp=GetACP();
        return {{"UTF-8","UTF-8",QCoreApplication::translate("DocumentTextCodec","유니코드"),true},
            {"UTF-16LE","UTF-16LE",{},true},{"UTF-16BE","UTF-16BE",{},true},
            {"CP949","EUC-KR (CP949)",QCoreApplication::translate("DocumentTextCodec","한국어"),false},
            {"Shift_JIS","Shift_JIS",QCoreApplication::translate("DocumentTextCodec","일본어"),false},
            {"GB18030","GB18030",QCoreApplication::translate("DocumentTextCodec","중국어"),false},
            {"ISO-8859-1","ISO-8859-1",QCoreApplication::translate("DocumentTextCodec","서유럽어"),false},
            {"System",QCoreApplication::translate("DocumentTextCodec","시스템 기본값"),Acp==CP_UTF8?QStringLiteral("UTF-8"):QStringLiteral("CP%1").arg(Acp),false}};
    }
    QList<TextEncodingInfo> SaveEncodings() {return ReadEncodings().mid(0,4);}
    QString TextDetectionLabel(TextDetection Detection)
    {
        switch(Detection) {
            case TextDetection::Bom:
            case TextDetection::Utf8Strict:return QCoreApplication::translate("DocumentTextCodec","신뢰도 높음");
            case TextDetection::AsciiOnly:return QCoreApplication::translate("DocumentTextCodec","신뢰도 보통");
            case TextDetection::Cp949Fallback:return QCoreApplication::translate("DocumentTextCodec","신뢰도 낮음");
            case TextDetection::Explicit:break;
        }
        return QCoreApplication::translate("DocumentTextCodec","직접 지정");
    }
    UnrepresentableScan FindUnrepresentable(const QString& Text,const QString& Encoding)
    {
        UnrepresentableScan Scan;
        const auto Name=CanonicalEncodingName(Encoding);if(Name.isEmpty())return Scan;
        const auto* Legacy=FindLegacy(Name);const UINT CodePage=Legacy?CodePageOf(*Legacy):0;
        // One whole-text conversion settles the common case. Only a lossy text is walked scalar by scalar,
        // and each distinct scalar is converted once, so even a text that is lossy throughout stays linear.
        if(Text.isValidUtf16()) {
            if(!Legacy)return Scan;
            QByteArray Ignored;if(EncodeCodePage(Text,CodePage,Ignored)==Conversion::Exact)return Scan;
        }
        std::vector<quint8> Bmp(Legacy?0x10000:0);std::unordered_map<char32_t,bool> Supplementary;
        const auto Representable=[&](QStringView Scalar,char32_t Code) {
            if(!Legacy)return true;
            QByteArray Ignored;
            if(Code<0x10000) {auto& Slot=Bmp[Code];if(!Slot)Slot=quint8(EncodeCodePage(Scalar,CodePage,Ignored)==Conversion::Exact?1:2);return Slot==1;}
            const auto Found=Supplementary.find(Code);if(Found!=Supplementary.end())return Found->second;
            return Supplementary[Code]=EncodeCodePage(Scalar,CodePage,Ignored)==Conversion::Exact;
        };
        const QStringView View(Text);
        for(qsizetype At=0;At<View.size();) {
            const char16_t Unit=View[At].unicode();qsizetype Width=1;bool Ok=false;
            if(QChar::isHighSurrogate(Unit)&&At+1<View.size()&&QChar::isLowSurrogate(View[At+1].unicode())) {
                Width=2;Ok=Representable(View.sliced(At,2),QChar::surrogateToUcs4(Unit,View[At+1].unicode()));
            } else if(!QChar::isSurrogate(Unit))Ok=Representable(View.sliced(At,1),Unit);
            if(!Ok){if(!Scan.count_)Scan.firstOffset_=At;++Scan.count_;}
            At+=Width;
        }
        return Scan;
    }
}
