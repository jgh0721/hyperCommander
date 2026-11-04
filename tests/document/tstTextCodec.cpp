#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <QtTest>
#include <QElapsedTimer>
#include <Windows.h>
#include "UniqueLibs/document/documentTextCodec.hpp"
#include "UniqueLibs/document/textDocumentMetrics.hpp"
using namespace hc::document;
namespace {
template<class T> QString Code(const hc::storage::Result<T>& Result){return Result.HasValue()?QString():QString::fromStdString(Result.Failure().code_);}
// The open-time scan of documentStorageAdapter.cpp:154-161, kept as the reference classification.
QString LegacyEol(const QString& Text){
    bool CrLf=false,Cr=false,Lf=false;
    for(qsizetype I=0;I<Text.size();++I){
        if(Text[I]=='\r'){if(I+1<Text.size()&&Text[I+1]=='\n'){CrLf=true;++I;}else Cr=true;}
        else if(Text[I]=='\n')Lf=true;
        if(int(CrLf)+int(Cr)+int(Lf)>1)break;
    }
    return int(CrLf)+int(Cr)+int(Lf)>1?QString("MIXED"):CrLf?QString("CRLF"):Cr?QString("CR"):QString("LF");
}
const QStringList ReadNames{"UTF-8","UTF-16LE","UTF-16BE","CP949","Shift_JIS","GB18030","ISO-8859-1","System"};
}
class TextCodecTests final:public QObject {
Q_OBJECT
private slots:
void roundTripsEveryEncoding_data(){
    QTest::addColumn<QString>("encoding");QTest::addColumn<QString>("text");
    const auto Mixed=QStringLiteral("한글 日本語 中文 Ünïcödé 🙂 A\u00a0B\u2028C\r\nLF\nCR\rEND");
    QTest::newRow("utf8")<<QString("UTF-8")<<Mixed;
    QTest::newRow("utf16le")<<QString("UTF-16LE")<<Mixed;
    QTest::newRow("utf16be")<<QString("UTF-16BE")<<Mixed;
    QTest::newRow("cp949")<<QString("CP949")<<QStringLiteral("한글 English 漢字 ひらがな カタカナ\r\nLF\nCR\r");
    QTest::newRow("euc-kr-alias")<<QString("EUC-KR")<<QStringLiteral("한글\r\n");
    QTest::newRow("euc-kr-label")<<QString("EUC-KR (CP949)")<<QStringLiteral("한글\r\n");
    QTest::newRow("shift-jis")<<QString("Shift_JIS")<<QStringLiteral("日本語 ひらがな カタカナ ｶﾀｶﾅ 漢字 表示\r\nLF\n");
    QTest::newRow("gb18030")<<QString("GB18030")<<QStringLiteral("中文 们 한글 日本語 Ünïcödé 🙂 𠀀\r\nLF\n");
    QTest::newRow("iso-8859-1")<<QString("ISO-8859-1")<<QStringLiteral("Café déjà vu, naïve ±§ ÿ\u0080\u009f\r\nLF\n");
    QTest::newRow("system")<<QString("System")<<QString("System ANSI text\r\nline\n");
}
void roundTripsEveryEncoding(){
    QFETCH(QString,encoding);QFETCH(QString,text);
    const auto Info=EncodingInfo(encoding);QVERIFY(!Info.name_.isEmpty());QCOMPARE(CanonicalEncodingName(encoding),Info.name_);
    const auto Plain=EncodeDocumentText(text,encoding,false);QVERIFY2(Plain.HasValue(),qPrintable(Code(Plain)));
    const auto Decoded=DecodeDocumentText(Plain.Value(),encoding);QVERIFY2(Decoded.HasValue(),qPrintable(Code(Decoded)));
    QCOMPARE(Decoded.Value().text_,text);QCOMPARE(Decoded.Value().encoding_,Info.name_);QVERIFY(!Decoded.Value().bom_);
    QCOMPARE(int(Decoded.Value().detection_),int(TextDetection::Explicit));
    const auto Again=EncodeDocumentText(Decoded.Value().text_,Decoded.Value().encoding_,false);QVERIFY(Again.HasValue());QCOMPARE(Again.Value(),Plain.Value());
    const auto WithBom=EncodeDocumentText(text,encoding,true);QVERIFY(WithBom.HasValue());
    if(!Info.bomCapable_){QCOMPARE(WithBom.Value(),Plain.Value());return;}
    QVERIFY(WithBom.Value().size()>Plain.Value().size());
    const auto BomDecoded=DecodeDocumentText(WithBom.Value(),encoding);QVERIFY(BomDecoded.HasValue());
    QCOMPARE(BomDecoded.Value().text_,text);QVERIFY(BomDecoded.Value().bom_);
    QCOMPARE(EncodeDocumentText(BomDecoded.Value().text_,encoding,true).Value(),WithBom.Value());
}
void writesKnownBytes(){
    const auto Bytes=[](const QString& Text,const QString& Encoding){const auto Result=EncodeDocumentText(Text,Encoding,false);return Result.HasValue()?Result.Value():QByteArray("<failed>");};
    QCOMPARE(Bytes(QStringLiteral("가한글"),"CP949"),QByteArray("\xb0\xa1\xc7\xd1\xb1\xdb"));
    QCOMPARE(Bytes(QStringLiteral("가"),"euc-kr"),QByteArray("\xb0\xa1"));
    QCOMPARE(Bytes(QStringLiteral("日ｱ"),"Shift_JIS"),QByteArray("\x93\xfa\xb1"));
    QCOMPARE(Bytes(QStringLiteral("中🙂"),"GB18030"),QByteArray("\xd6\xd0\x95\x30\x85\x32"));
    QCOMPARE(Bytes(QStringLiteral("éÿ"),"ISO-8859-1"),QByteArray("\xe9\xff"));
    QCOMPARE(Bytes(QStringLiteral("가"),"UTF-8"),QByteArray("\xea\xb0\x80"));
    QCOMPARE(Bytes("A","UTF-16LE"),QByteArray("A\0",2));
    QCOMPARE(Bytes("A","UTF-16BE"),QByteArray("\0A",2));
    QCOMPARE(Bytes("abc","System"),QByteArray("abc"));
    if(GetACP()==949)QCOMPARE(Bytes(QStringLiteral("한글"),"System"),QByteArray("\xc7\xd1\xb1\xdb"));
    const auto Gb=DecodeDocumentText(QByteArray("\xd6\xd0\x95\x30\x85\x32"),"GB18030");QVERIFY(Gb.HasValue());QCOMPARE(Gb.Value().text_,QStringLiteral("中🙂"));
    // ISO-8859-1 maps every byte to the scalar of the same value.
    QByteArray Latin;for(int Value=1;Value<256;++Value)Latin.append(char(Value));
    const auto Decoded=DecodeDocumentText(Latin,"ISO-8859-1");QVERIFY2(Decoded.HasValue(),qPrintable(Code(Decoded)));QCOMPARE(Decoded.Value().text_.size(),qsizetype(255));
    for(int Value=1;Value<256;++Value)QCOMPARE(int(Decoded.Value().text_[Value-1].unicode()),Value);
    QCOMPARE(EncodeDocumentText(Decoded.Value().text_,"ISO-8859-1",false).Value(),Latin);
}
void bomBelongsToUtfEncodingsOnly(){
    QCOMPARE(EncodeDocumentText("A","UTF-8",true).Value(),QByteArray("\xef\xbb\xbf" "A"));
    QCOMPARE(EncodeDocumentText("A","UTF-16LE",true).Value(),QByteArray("\xff\xfe" "A\0",4));
    QCOMPARE(EncodeDocumentText("A","UTF-16BE",true).Value(),QByteArray("\xfe\xff\0A",4));
    for(const auto& Name:QStringList{"CP949","Shift_JIS","GB18030","ISO-8859-1","System"})QCOMPARE(EncodeDocumentText("A",Name,true).Value(),QByteArray("A"));
    // Read as another encoding, BOM bytes are content and must come back unchanged on save.
    const QByteArray Utf8Bom("\xef\xbb\xbf" "A");
    const auto Latin=DecodeDocumentText(Utf8Bom,"ISO-8859-1");QVERIFY(Latin.HasValue());
    QVERIFY(!Latin.Value().bom_);QCOMPARE(Latin.Value().text_,QStringLiteral("\u00ef\u00bb\u00bfA"));
    QCOMPARE(EncodeDocumentText(Latin.Value().text_,"ISO-8859-1",Latin.Value().bom_).Value(),Utf8Bom);
    const auto Wide=DecodeDocumentText(Utf8Bom,"UTF-16LE");QVERIFY(Wide.HasValue());QVERIFY(!Wide.Value().bom_);
    QCOMPARE(Wide.Value().text_,QStringLiteral("믯䆿"));
    QCOMPARE(EncodeDocumentText(Wide.Value().text_,"UTF-16LE",Wide.Value().bom_).Value(),Utf8Bom);
    const auto Matching=DecodeDocumentText(QByteArray("\xff\xfe" "A\0",4),"UTF-16LE");QVERIFY(Matching.HasValue());QVERIFY(Matching.Value().bom_);QCOMPARE(Matching.Value().text_,QString("A"));
}
void rejectsInvalidBytesPerEncoding_data(){
    QTest::addColumn<QString>("encoding");QTest::addColumn<QByteArray>("bytes");QTest::addColumn<QString>("code");
    QTest::newRow("utf8-overlong")<<QString("UTF-8")<<QByteArray("\xc0\xaf")<<QString("document-decode-invalid");
    QTest::newRow("utf16-odd")<<QString("UTF-16LE")<<QByteArray("\xff\xfe\x00",3)<<QString("document-decode-truncated-utf16");
    QTest::newRow("cp949-lone-lead")<<QString("CP949")<<QByteArray("A\xb0")<<QString("document-decode-invalid-cp949");
    QTest::newRow("euc-kr-lone-lead")<<QString("EUC-KR")<<QByteArray("A\xb0")<<QString("document-decode-invalid-cp949");
    QTest::newRow("shift-jis-lone-lead")<<QString("Shift_JIS")<<QByteArray("A\x93")<<QString("document-decode-invalid-shift-jis");
    QTest::newRow("gb18030-lone-lead")<<QString("GB18030")<<QByteArray("A\x81")<<QString("document-decode-invalid-gb18030");
    QTest::newRow("gb18030-truncated-four")<<QString("GB18030")<<QByteArray("A\x81\x30")<<QString("document-decode-invalid-gb18030");
    QTest::newRow("cp949-nul")<<QString("CP949")<<QByteArray("A\0B",3)<<QString("document-text-nul-or-surrogate");
    QTest::newRow("latin1-nul")<<QString("ISO-8859-1")<<QByteArray("A\0B",3)<<QString("document-text-nul-or-surrogate");
    QTest::newRow("unknown")<<QString("KOI8-R")<<QByteArray("A")<<QString("document-encoding-unavailable");
    // The system page depends on the machine: probe the failure that page actually has.
    if(GetACP()==CP_UTF8)QTest::newRow("system-utf8")<<QString("System")<<QByteArray("\xc0\xaf")<<QString("document-decode-invalid-system");
    else for(int Lead=0x81;Lead<=0xfe;++Lead)if(IsDBCSLeadByteEx(CP_ACP,BYTE(Lead))){
        QTest::newRow("system-lone-lead")<<QString("System")<<(QByteArray("A")+char(Lead))<<QString("document-decode-invalid-system");break;}
}
void rejectsInvalidBytesPerEncoding(){
    QFETCH(QString,encoding);QFETCH(QByteArray,bytes);QFETCH(QString,code);
    const auto Decoded=DecodeDocumentText(bytes,encoding);QVERIFY(!Decoded.HasValue());QCOMPARE(Code(Decoded),code);
}
void rejectsUnknownEncodingOnSave(){
    QCOMPARE(Code(EncodeDocumentText("A","KOI8-R",false)),QString("document-encoding-unavailable"));
    QCOMPARE(Code(EncodeDocumentText(QStringLiteral("한글🙂"),"CP949",false)),QString("document-encoding-unrepresentable"));
    QCOMPARE(Code(EncodeDocumentText(QStringLiteral("한"),"ISO-8859-1",false)),QString("document-encoding-unrepresentable"));
    QCOMPARE(Code(EncodeDocumentText(QStringLiteral("中"),"Shift_JIS",false)),QString());
    QCOMPARE(Code(EncodeDocumentText(QStringLiteral("们"),"Shift_JIS",false)),QString("document-encoding-unrepresentable"));
    QCOMPARE(Code(EncodeDocumentText(QString("a")+QChar(0xd800),"GB18030",false)),QString("document-encode-invalid-text"));
}
void detectionReportsConfidence(){
    const auto Detect=[](const QByteArray& Bytes,const QString& Encoding={}){return DecodeDocumentText(Bytes,Encoding);};
    auto Ascii=Detect("plain ascii\r\n");QVERIFY(Ascii.HasValue());QCOMPARE(int(Ascii.Value().detection_),int(TextDetection::AsciiOnly));QCOMPARE(Ascii.Value().encoding_,QString("UTF-8"));
    auto Empty=Detect({});QVERIFY(Empty.HasValue());QCOMPARE(int(Empty.Value().detection_),int(TextDetection::AsciiOnly));
    auto Utf8=Detect(QStringLiteral("한글").toUtf8());QVERIFY(Utf8.HasValue());QCOMPARE(int(Utf8.Value().detection_),int(TextDetection::Utf8Strict));QCOMPARE(Utf8.Value().encoding_,QString("UTF-8"));
    auto Bom=Detect("\xef\xbb\xbf" "abc");QVERIFY(Bom.HasValue());QCOMPARE(int(Bom.Value().detection_),int(TextDetection::Bom));QVERIFY(Bom.Value().bom_);QCOMPARE(Bom.Value().text_,QString("abc"));
    auto Wide=Detect(QByteArray("\xff\xfe" "a\0",4));QVERIFY(Wide.HasValue());QCOMPARE(int(Wide.Value().detection_),int(TextDetection::Bom));QCOMPARE(Wide.Value().encoding_,QString("UTF-16LE"));
    const auto Korean=EncodeDocumentText(QStringLiteral("한글 문서\r\n"),"CP949",false);QVERIFY(Korean.HasValue());
    auto Legacy=Detect(Korean.Value());QVERIFY(Legacy.HasValue());QCOMPARE(int(Legacy.Value().detection_),int(TextDetection::Cp949Fallback));
    QCOMPARE(Legacy.Value().encoding_,QString("CP949"));QCOMPARE(Legacy.Value().text_,QStringLiteral("한글 문서\r\n"));QVERIFY(!Legacy.Value().bom_);
    auto Explicit=Detect("abc","UTF-8");QVERIFY(Explicit.HasValue());QCOMPARE(int(Explicit.Value().detection_),int(TextDetection::Explicit));
    auto Alias=Detect(Korean.Value(),"euc-kr");QVERIFY(Alias.HasValue());QCOMPARE(Alias.Value().encoding_,QString("CP949"));QCOMPARE(int(Alias.Value().detection_),int(TextDetection::Explicit));
    // A UTF-8 BOM in front of invalid UTF-8 is not a legacy file.
    QCOMPARE(Code(Detect("\xef\xbb\xbf\xc0\xaf")),QString("document-decode-invalid"));
    QCOMPARE(TextDetectionLabel(TextDetection::Bom),QStringLiteral("신뢰도 높음"));
    QCOMPARE(TextDetectionLabel(TextDetection::Utf8Strict),QStringLiteral("신뢰도 높음"));
    QCOMPARE(TextDetectionLabel(TextDetection::AsciiOnly),QStringLiteral("신뢰도 보통"));
    QCOMPARE(TextDetectionLabel(TextDetection::Cp949Fallback),QStringLiteral("신뢰도 낮음"));
    QCOMPARE(TextDetectionLabel(TextDetection::Explicit),QStringLiteral("직접 지정"));
}
void listsReadAndSaveEncodings(){
    const auto Read=ReadEncodings();QStringList Names,Labels;for(const auto& Info:Read){Names<<Info.name_;Labels<<Info.label_;}
    QCOMPARE(Names,ReadNames);
    QCOMPARE(Labels,(QStringList{"UTF-8","UTF-16LE","UTF-16BE","EUC-KR (CP949)","Shift_JIS","GB18030","ISO-8859-1",QStringLiteral("시스템 기본값")}));
    QCOMPARE(Read[0].note_,QStringLiteral("유니코드"));QCOMPARE(Read[3].note_,QStringLiteral("한국어"));QCOMPARE(Read[4].note_,QStringLiteral("일본어"));
    QCOMPARE(Read[5].note_,QStringLiteral("중국어"));QCOMPARE(Read[6].note_,QStringLiteral("서유럽어"));
    QCOMPARE(Read[7].note_,GetACP()==CP_UTF8?QString("UTF-8"):QString("CP%1").arg(GetACP()));
    for(int Index=0;Index<Read.size();++Index)QCOMPARE(Read[Index].bomCapable_,Index<3);
    const auto Save=SaveEncodings();QCOMPARE(Save.size(),qsizetype(4));
    for(int Index=0;Index<Save.size();++Index){QCOMPARE(Save[Index].name_,Read[Index].name_);QCOMPARE(Save[Index].label_,Read[Index].label_);}
    for(const auto& Info:Read){
        QCOMPARE(CanonicalEncodingName(Info.name_),Info.name_);QCOMPARE(CanonicalEncodingName(Info.label_),Info.name_);
        QCOMPARE(CanonicalEncodingName(Info.name_.toLower()),Info.name_);QCOMPARE(EncodingInfo(Info.label_).name_,Info.name_);
        // Every read encoding is also a valid save target (the current read encoding joins the save list).
        const auto Bytes=EncodeDocumentText("abc\r\n",Info.name_,false);QVERIFY2(Bytes.HasValue(),qPrintable(Info.name_));
    }
    QVERIFY(CanonicalEncodingName("KOI8-R").isEmpty());QVERIFY(CanonicalEncodingName({}).isEmpty());QVERIFY(EncodingInfo("KOI8-R").name_.isEmpty());
}
void findsUnrepresentableScalars_data(){
    QTest::addColumn<QString>("text");QTest::addColumn<QString>("encoding");QTest::addColumn<qsizetype>("count");QTest::addColumn<qsizetype>("first");
    QTest::newRow("cp949-clean")<<QStringLiteral("한글 漢字 ひらがな")<<QString("CP949")<<qsizetype(0)<<qsizetype(-1);
    QTest::newRow("cp949-emoji-simplified-hanzi")<<QStringLiteral("가们나🙂다")<<QString("CP949")<<qsizetype(2)<<qsizetype(1);
    QTest::newRow("latin1-korean-best-fit-emoji")<<QStringLiteral("Aé한Ā🙂")<<QString("ISO-8859-1")<<qsizetype(3)<<qsizetype(2);
    QTest::newRow("latin1-fullwidth")<<QStringLiteral("Ａ")<<QString("ISO-8859-1")<<qsizetype(1)<<qsizetype(0);
    QTest::newRow("latin1-pairs-once")<<QStringLiteral("🙂🙂")<<QString("ISO-8859-1")<<qsizetype(2)<<qsizetype(0);
    QTest::newRow("shift-jis-emoji")<<QStringLiteral("日本🙂")<<QString("Shift_JIS")<<qsizetype(1)<<qsizetype(2);
    QTest::newRow("gb18030-everything")<<QStringLiteral("中文们한글🙂𠀀")<<QString("GB18030")<<qsizetype(0)<<qsizetype(-1);
    QTest::newRow("utf8-everything")<<QStringLiteral("한🙂")<<QString("UTF-8")<<qsizetype(0)<<qsizetype(-1);
    QTest::newRow("utf16-everything")<<QStringLiteral("한🙂")<<QString("UTF-16BE")<<qsizetype(0)<<qsizetype(-1);
    QTest::newRow("utf8-lone-surrogate")<<QString(QString("a")+QChar(0xd800)+QString("b"))<<QString("UTF-8")<<qsizetype(1)<<qsizetype(1);
    QTest::newRow("cp949-trailing-high")<<QString(QStringLiteral("가")+QChar(0xd83d))<<QString("CP949")<<qsizetype(1)<<qsizetype(1);
    QTest::newRow("unknown")<<QStringLiteral("한")<<QString("KOI8-R")<<qsizetype(0)<<qsizetype(-1);
}
void findsUnrepresentableScalars(){
    QFETCH(QString,text);QFETCH(QString,encoding);QFETCH(qsizetype,count);QFETCH(qsizetype,first);
    const auto Scan=FindUnrepresentable(text,encoding);QCOMPARE(Scan.count_,count);QCOMPARE(Scan.firstOffset_,first);
    // The scan and the save agree on valid text.
    if(text.isValidUtf16()&&!CanonicalEncodingName(encoding).isEmpty())QCOMPARE(EncodeDocumentText(text,encoding,false).HasValue(),count==0);
}
void findsUnrepresentableInLargeText(){
    QString Clean=QStringLiteral("한글 문서 줄\r\n").repeated(300000);
    QElapsedTimer Timer;Timer.start();
    QCOMPARE(FindUnrepresentable(Clean,"CP949").count_,qsizetype(0));
    const QString Late=Clean+QStringLiteral("🙂");const auto Scan=FindUnrepresentable(Late,"CP949");
    QCOMPARE(Scan.count_,qsizetype(1));QCOMPARE(Scan.firstOffset_,Clean.size());
    // Lossy throughout: each distinct scalar is converted once, the walk stays linear.
    const auto Lossy=FindUnrepresentable(Clean,"ISO-8859-1");QCOMPARE(Lossy.count_,qsizetype(300000*5));QCOMPARE(Lossy.firstOffset_,qsizetype(0));
    QVERIFY2(Timer.elapsed()<10000,qPrintable(QString::number(Timer.elapsed())));
    const auto Position=PositionOfOffset(Late,Scan.firstOffset_);QCOMPARE(Position.line_,300001);QCOMPARE(Position.column_,1);
}
void countsLineEndings_data(){
    QTest::addColumn<QString>("text");QTest::addColumn<qsizetype>("crlf");QTest::addColumn<qsizetype>("lf");QTest::addColumn<qsizetype>("cr");QTest::addColumn<QString>("dominant");
    QTest::newRow("empty")<<QString()<<qsizetype(0)<<qsizetype(0)<<qsizetype(0)<<QString();
    QTest::newRow("no-breaks")<<QString("abc")<<qsizetype(0)<<qsizetype(0)<<qsizetype(0)<<QString();
    QTest::newRow("crlf")<<QString("a\r\nb\r\n")<<qsizetype(2)<<qsizetype(0)<<qsizetype(0)<<QString("CRLF");
    QTest::newRow("lf")<<QString("a\nb")<<qsizetype(0)<<qsizetype(1)<<qsizetype(0)<<QString("LF");
    QTest::newRow("cr-trailing")<<QString("a\rb\r")<<qsizetype(0)<<qsizetype(0)<<qsizetype(2)<<QString("CR");
    QTest::newRow("lone-cr-at-end")<<QString("abc\r")<<qsizetype(0)<<qsizetype(0)<<qsizetype(1)<<QString("CR");
    QTest::newRow("cr-cr-lf")<<QString("a\r\r\nb")<<qsizetype(1)<<qsizetype(0)<<qsizetype(1)<<QString("MIXED");
    QTest::newRow("lf-cr")<<QString("\n\r")<<qsizetype(0)<<qsizetype(1)<<qsizetype(1)<<QString("MIXED");
    QTest::newRow("crlf-lf")<<QString("\r\n\n")<<qsizetype(1)<<qsizetype(1)<<qsizetype(0)<<QString("MIXED");
    QTest::newRow("all-kinds")<<QString("a\r\nb\nc\rd")<<qsizetype(1)<<qsizetype(1)<<qsizetype(1)<<QString("MIXED");
    QTest::newRow("korean")<<QStringLiteral("한글\r\n문서\r\n끝")<<qsizetype(2)<<qsizetype(0)<<qsizetype(0)<<QString("CRLF");
}
void countsLineEndings(){
    QFETCH(QString,text);QFETCH(qsizetype,crlf);QFETCH(qsizetype,lf);QFETCH(qsizetype,cr);QFETCH(QString,dominant);
    const auto Stats=CountLineEndings(text);
    QCOMPARE(Stats.crlf_,crlf);QCOMPARE(Stats.lf_,lf);QCOMPARE(Stats.cr_,cr);QCOMPARE(Stats.Dominant(),dominant);
    QCOMPARE(Stats.Breaks(),crlf+lf+cr);QCOMPARE(Stats.Lines(),crlf+lf+cr+1);
    // Same verdict as the scan it replaces; that scan reports LF when there is no break.
    QCOMPARE(dominant.isEmpty()?QString("LF"):dominant,LegacyEol(text));
    const auto Bytes=CountLineEndings(QByteArrayView(text.toUtf8()));
    QCOMPARE(Bytes.crlf_,crlf);QCOMPARE(Bytes.lf_,lf);QCOMPARE(Bytes.cr_,cr);
}
void countsLineEndingsInLegacyBytes(){
    // Trail bytes of the double-byte pages never take 0x0A/0x0D (表 is 0x95 0x5C in Shift_JIS).
    const auto Korean=EncodeDocumentText(QStringLiteral("한글\r\n가\n나"),"CP949",false);QVERIFY(Korean.HasValue());
    const auto KoreanStats=CountLineEndings(QByteArrayView(Korean.Value()));QCOMPARE(KoreanStats.crlf_,qsizetype(1));QCOMPARE(KoreanStats.lf_,qsizetype(1));QCOMPARE(KoreanStats.Dominant(),QString("MIXED"));
    const auto Japanese=EncodeDocumentText(QStringLiteral("表示\r\n表\r\n"),"Shift_JIS",false);QVERIFY(Japanese.HasValue());
    const auto JapaneseStats=CountLineEndings(QByteArrayView(Japanese.Value()));QCOMPARE(JapaneseStats.crlf_,qsizetype(2));QCOMPARE(JapaneseStats.Dominant(),QString("CRLF"));
}
void detectsIndent(){
    const auto Empty=DetectIndent(u"");QVERIFY(!Empty.useTabs_);QCOMPARE(Empty.width_,4);QVERIFY(!Empty.confident_);QCOMPARE(Empty.tabLines_+Empty.spaceLines_,qsizetype(0));
    const auto Tabs=DetectIndent(u"int f()\n{\n\tif (x)\n\t\treturn 1;\n\treturn 0;\n}\n");
    QVERIFY(Tabs.useTabs_);QCOMPARE(Tabs.tabLines_,qsizetype(3));QCOMPARE(Tabs.spaceLines_,qsizetype(0));QVERIFY(Tabs.confident_);
    const auto Two=DetectIndent(u"a:\r\n  b:\r\n    c: 1\r\n  d: 2\r\n  e:\r\n    f: 3\r\n");
    QVERIFY(!Two.useTabs_);QCOMPARE(Two.width_,2);QCOMPARE(Two.spaceLines_,qsizetype(5));QVERIFY(Two.confident_);
    const auto Four=DetectIndent(u"def f():\n    if x:\n        return 1\n    return 0\n\nclass A:\n    def g(self):\n        pass\n");
    QVERIFY(!Four.useTabs_);QCOMPARE(Four.width_,4);QVERIFY(Four.confident_);
    const auto Eight=DetectIndent(u"a\n        b\n                c\n        d\nx\n        y\n");
    QVERIFY(!Eight.useTabs_);QCOMPARE(Eight.width_,8);QVERIFY(Eight.confident_);
    const auto Mixed=DetectIndent(u"a\n\tb\n\tc\nd\n    e\n    f\n");
    QCOMPARE(Mixed.tabLines_,qsizetype(2));QCOMPARE(Mixed.spaceLines_,qsizetype(2));QVERIFY(!Mixed.useTabs_);QVERIFY(!Mixed.confident_);
    // Whitespace-only lines are blank; one-space comment continuations are no indentation level.
    const auto Blank=DetectIndent(u"a\n\n    \n\t\n    b\n");QCOMPARE(Blank.tabLines_,qsizetype(0));QCOMPARE(Blank.spaceLines_,qsizetype(1));QVERIFY(!Blank.confident_);
    const auto Comment=DetectIndent(u"/**\n * doc\n * more\n */\nint f()\n{\n\treturn 1;\n\tx;\n\ty;\n}\n");
    QCOMPARE(Comment.spaceLines_,qsizetype(0));QVERIFY(Comment.useTabs_);QVERIFY(Comment.confident_);
    // Most lines sit at a multiple of 8; the nesting steps still say 4.
    const auto Deep=DetectIndent(u"class A {\n    void f() {\n        a();\n        if (x) {\n            b();\n        }\n        c();\n        d();\n    }\n}\n");
    QCOMPARE(Deep.width_,4);QVERIFY(Deep.confident_);
    // No usable step: the common divisor of the depths decides, without confidence for one line.
    const auto Divisor=DetectIndent(u"a\n                b\n");QCOMPARE(Divisor.width_,8);QVERIFY(!Divisor.confident_);
    // MaxLines limits the sample.
    QString Text("x\n");Text+=QString("\tt\n").repeated(10);Text+=QString("    s\n").repeated(100);
    const auto Head=DetectIndent(Text,11);QVERIFY(Head.useTabs_);QCOMPARE(Head.tabLines_,qsizetype(10));QCOMPARE(Head.spaceLines_,qsizetype(0));
    const auto Whole=DetectIndent(Text);QVERIFY(!Whole.useTabs_);QCOMPARE(Whole.width_,4);QVERIFY(Whole.confident_);
    QCOMPARE(DetectIndent(Text,0).spaceLines_,qsizetype(100));
}
void mapsOffsetsToPositions(){
    const auto At=[](QStringView Text,qsizetype Offset){const auto Position=PositionOfOffset(Text,Offset);return QPoint(Position.line_,Position.column_);};
    QCOMPARE(At(u"",0),QPoint(1,1));
    QCOMPARE(At(u"abc",2),QPoint(1,3));QCOMPARE(At(u"abc",3),QPoint(1,4));
    QCOMPARE(At(u"ab\r\ncd",4),QPoint(2,1));QCOMPARE(At(u"ab\r\ncd",5),QPoint(2,2));
    QCOMPARE(At(u"ab\r\ncd",3),QPoint(1,4));
    QCOMPARE(At(u"a\rb\nc",2),QPoint(2,1));QCOMPARE(At(u"a\rb\nc",4),QPoint(3,1));
    QCOMPARE(At(u"abc\r",4),QPoint(2,1));
    // Columns count UTF-16 units, as QTextView::goToPosition expects.
    QCOMPARE(At(u"🙂x",2),QPoint(1,3));
    QCOMPARE(At(u"abc",-5),QPoint(1,1));QCOMPARE(At(u"a\nbc",1000),QPoint(2,3));
    const QString Text=QStringLiteral("가\r\n나们");const auto Scan=FindUnrepresentable(Text,"CP949");QCOMPARE(Scan.firstOffset_,qsizetype(4));
    QCOMPARE(At(Text,Scan.firstOffset_),QPoint(2,2));
}
};
QTEST_GUILESS_MAIN(TextCodecTests)
#include "tstTextCodec.moc"
