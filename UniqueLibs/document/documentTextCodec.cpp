#include "documentTextCodec.hpp"
#include <QStringConverter>
#include <Windows.h>
namespace hc::document
{
    namespace { storage::Error Invalid(const char* Code) {return {storage::ErrorCategory::InvalidRequest,Code};} }
    storage::Result<DecodedText> DecodeDocumentText(const QByteArray& Bytes,const QString& Encoding)
    {
        DecodedText Result;Result.encoding_=Encoding;
        if(Result.encoding_.isEmpty()) {
            if(Bytes.startsWith("\xff\xfe")) Result.encoding_="UTF-16LE";
            else if(Bytes.startsWith("\xfe\xff"))Result.encoding_="UTF-16BE";
            else Result.encoding_="UTF-8";
        }
        Result.bom_=Bytes.startsWith("\xef\xbb\xbf")||Bytes.startsWith("\xff\xfe")||Bytes.startsWith("\xfe\xff");
        if(Result.encoding_=="CP949") {
            const auto Length=MultiByteToWideChar(949,MB_ERR_INVALID_CHARS,Bytes.constData(),int(Bytes.size()),nullptr,0);
            if(!Length&&!Bytes.isEmpty())return Invalid("document-decode-invalid-cp949");
            Result.text_.resize(Length);
            if(Length&&MultiByteToWideChar(949,MB_ERR_INVALID_CHARS,Bytes.constData(),int(Bytes.size()),reinterpret_cast<wchar_t*>(Result.text_.data()),Length)!=Length)return Invalid("document-decode-invalid-cp949");
        } else {
            const auto Kind=Result.encoding_=="UTF-8"?QStringConverter::Utf8:Result.encoding_=="UTF-16LE"?QStringConverter::Utf16LE:
                Result.encoding_=="UTF-16BE"?QStringConverter::Utf16BE:QStringConverter::System;
            if(Kind==QStringConverter::System)return Invalid("document-encoding-unavailable");
            if((Kind==QStringConverter::Utf16LE||Kind==QStringConverter::Utf16BE)&&Bytes.size()%2)return Invalid("document-decode-truncated-utf16");
            QStringDecoder Decoder(Kind);Result.text_=Decoder(Bytes);
            if(Decoder.hasError()) {
                // Invalid UTF-8 can be a legacy Korean file. Validate CP949
                // strictly; never accept replacement characters from a decoder.
                if(Encoding.isEmpty()&&!Result.bom_)return DecodeDocumentText(Bytes,"CP949");
                return Invalid("document-decode-invalid");
            }
        }
        if(Result.text_.contains(QChar::Null)||!Result.text_.isValidUtf16())return Invalid("document-text-nul-or-surrogate");
        return Result;
    }
    storage::Result<QByteArray> EncodeDocumentText(const QString& Text,const QString& Encoding,bool Bom)
    {
        if(!Text.isValidUtf16()||Text.contains(QChar::Null))return Invalid("document-encode-invalid-text");
        if(Encoding=="CP949") {
            BOOL Substituted=FALSE;const auto Length=WideCharToMultiByte(949,WC_NO_BEST_FIT_CHARS,reinterpret_cast<const wchar_t*>(Text.utf16()),int(Text.size()),nullptr,0,nullptr,&Substituted);
            if(!Length&&!Text.isEmpty())return Invalid("document-encode-cp949-failed");
            QByteArray Bytes(Length,Qt::Uninitialized);
            if(Length)WideCharToMultiByte(949,WC_NO_BEST_FIT_CHARS,reinterpret_cast<const wchar_t*>(Text.utf16()),int(Text.size()),Bytes.data(),Length,nullptr,&Substituted);
            if(Substituted)return Invalid("document-encoding-unrepresentable");
            return Bytes;
        }
        const auto Kind=Encoding=="UTF-8"?QStringConverter::Utf8:Encoding=="UTF-16LE"?QStringConverter::Utf16LE:
            Encoding=="UTF-16BE"?QStringConverter::Utf16BE:QStringConverter::System;
        if(Kind==QStringConverter::System)return Invalid("document-encoding-unavailable");
        QStringEncoder Encoder(Kind,Bom?QStringConverter::Flag::WriteBom:QStringConverter::Flag::Default);
        QByteArray Bytes=Encoder(Text);if(Encoder.hasError())return Invalid("document-encoding-unrepresentable");return Bytes;
    }
}
