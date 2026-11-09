#include "stdafx.h"
#include "runtime/binarySession.hpp"
/// 모듈
#include "widgets/hexCompareView.hpp"
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QtTest>
#include <QtWidgets>
#include <fmstyle/ThemeManager.h>
using namespace hc::compare;
class HexTests final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/malgun.ttf");
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/consola.ttf");
        qApp->setFont(QFont("Malgun Gothic",9));
    }
    void scale()
    {
        for(qint64 Total:{qint64(1),qint64(4096),qint64(1)<<33,std::numeric_limits<qint64>::max()}){
            QCOMPARE(HexScale(0,Total,4096),qint64(0));QCOMPARE(HexScale(Total,Total,4096),qint64(4096));
            if(Total>1){QVERIFY(HexScale(Total/2,Total,4096)>=2047);QVERIFY(HexScale(Total/2,Total,4096)<=2048);}
        }
        for(int I=0;I<1000;++I)QCOMPARE(HexScale(I,1000,4096),qint64(I)*4096/1000);
    }
    void fullIndex()
    {
        QByteArray A(20000,'a'),B=A;for(int I=0;I<20000;I+=2)B[I]='b';
        auto C=CompareBinary({BinaryFromBytes(A),BinaryFromBytes(B)},"none",{});
        QCOMPARE(C->changeCount_,qint64(10000));QCOMPARE(C->different_,qint64(10000));QCOMPARE(C->Change(9999).begin_[0],qint64(19998));
        QCOMPARE(C->At(19998).changed_,quint64(1));QCOMPARE(C->At(19999).changed_,quint64(0));
        QVERIFY(C->overview_[0][4094]!=0);QCOMPARE(C->Json()["ranges"].toArray().size(),4096);
    }
    void alignment()
    {
        QByteArray A;for(int I=0;I<512;++I)A.append(char((I*37+I/7)%256));auto B=A;B.insert(64,"inserted!");
        for(auto Mode:{QString("none"),QString("fast"),QString("complete")}){
            auto C=CompareBinary({BinaryFromBytes(A),BinaryFromBytes(B)},Mode,{});qint64 Left=0,Right=0,Position=0;
            for(qint64 I=0;I<C->runCount_;++I){auto R=C->Run(I);QCOMPARE(R.begin_[0],Left);QCOMPARE(R.begin_[1],Right);QCOMPARE(R.position_,Position);
                if(!R.changed_)QCOMPARE(ReadBinary(C->documents_[0],Left,R.size_[0]),ReadBinary(C->documents_[1],Right,R.size_[1]));
                Left+=R.size_[0];Right+=R.size_[1];Position+=R.length_;}
            QCOMPARE(Left,qint64(A.size()));QCOMPARE(Right,qint64(B.size()));
            if(Mode!="none"){QCOMPARE(C->changeCount_,qint64(1));QCOMPARE(C->Change(0).size_[0],qint64(0));QCOMPARE(C->Change(0).size_[1],qint64(9));QCOMPARE(C->Position(0,64),qint64(73));}
        }
    }
    void editsAndSearch()
    {
        auto Original=BinaryFromBytes("abcdefgh"),Insert=BinaryFromBytes("XYZ");auto Changed=ReplaceBinary(Original,2,4,Insert,0,3);
        QCOMPARE(ReadBinary(Changed,0,100),QByteArray("abXYZefgh"));QCOMPARE(ReadBinary(Original,0,100),QByteArray("abcdefgh"));
        auto Deleted=ReplaceBinary(Changed,2,5,BinaryFromBytes({}),0,0);QCOMPARE(ReadBinary(Deleted,0,100),QByteArray("abefgh"));
        QByteArray Big(2*1024*1024,'x');Big.replace(1024*1024-2,6,"needle");auto Document=BinaryFromBytes(Big);
        QCOMPARE(FindBinary(Document,"needle",0,false,{}),qint64(1024*1024-2));QCOMPARE(FindBinary(Document,"needle",Big.size(),true,{}),qint64(1024*1024-2));
        QVERIFY_EXCEPTION_THROWN(ReplaceBinary(Original,-1,0,Insert,0,3),Failure);
    }
    void conversions()
    {
        QCOMPARE(ReadBinary(ConvertBinary(BinaryFromBytes("41 00 42\r\n"),"hex",{}),0,100),QByteArray("A\0B",3));
        QCOMPARE(ReadBinary(ConvertBinary(BinaryFromBytes("YQ==\r\n"),"base64",{}),0,100),QByteArray("a"));
        QCOMPARE(ReadBinary(ConvertBinary(BinaryFromBytes("YWJjZGU="),"base64",{}),0,100),QByteArray("abcde"));
        QVERIFY_EXCEPTION_THROWN(ConvertBinary(BinaryFromBytes("4Z"),"hex",{}),Failure);
        QVERIFY_EXCEPTION_THROWN(ConvertBinary(BinaryFromBytes("YQ==Z"),"base64",{}),Failure);
        QVERIFY_EXCEPTION_THROWN(ConvertBinary(BinaryFromBytes("4"),"hex",{}),Failure);
    }
    void shortAlignmentAndSourceLock()
    {
        auto C=CompareBinary({BinaryFromBytes("abc"),BinaryFromBytes("aXbc")},"complete",{});
        QCOMPARE(C->changeCount_,qint64(1));QCOMPARE(C->Change(0).size_[0],qint64(0));QCOMPARE(C->Change(0).size_[1],qint64(1));
        QTemporaryDir Dir;auto Path=Dir.filePath("lock.bin");{QFile F(Path);QVERIFY(F.open(QIODevice::WriteOnly));F.write("abc");}
        auto D=OpenBinary(Path,{});SetBinarySourceLock(D,0,{});{QFile F(Path);QVERIFY(!F.open(QIODevice::WriteOnly));}
        SetBinarySourceLock(D,-1,{});{QFile F(Path);QVERIFY(F.open(QIODevice::WriteOnly));F.write("new");}
    }
    void externalConversion()
    {
        const auto Config=QJsonObject{{"program",QCoreApplication::applicationFilePath()},{"arguments",QJsonArray{"--hex-converter","%s","%t"}}};
        BinaryDocument Converted;try{Converted=ConvertBinary(BinaryFromBytes("converter data"),"external:"+QString::fromUtf8(QJsonDocument(Config).toJson(QJsonDocument::Compact)),{});}catch(const std::exception& E){QFAIL(E.what());}
        QCOMPARE(ReadBinary(Converted,0,100),QByteArray("converter data"));
    }
    void emptyAndEof()
    {
        auto C=CompareBinary({BinaryFromBytes({}),BinaryFromBytes("a")},"complete",{});
        QCOMPARE(C->length_,qint64(1));QCOMPARE(C->Change(0).size_[0],qint64(0));QVERIFY(C->overview_[0][0]&8);
        C=CompareBinary({BinaryFromBytes({}),BinaryFromBytes({})},"none",{});QCOMPARE(C->runCount_,qint64(0));QCOMPARE(C->Position(0,0),qint64(0));
    }
    void highAddress()
    {
        auto Backing=std::make_shared<BinaryBacking>();const qint64 Size=(qint64(1)<<32)+17;
        QVERIFY(Backing->file_.resize(Size));QVERIFY(Backing->file_.seek(Size-1));QCOMPARE(Backing->file_.write("Z",1),qint64(1));QVERIFY(Backing->file_.flush());
        BinaryDocument D;D.size_=Size;D.pieces_.push_back({Backing,0,Size});QCOMPARE(ReadBinary(D,Size-1,1),QByteArray("Z"));
        BinaryComparison C;C.documents_={D,D};C.runs_=std::make_shared<BinaryBacking>();C.runCount_=1;C.length_=Size;
        HexRun R{{0,0},{Size,Size},0,Size,0};QCOMPARE(C.runs_->file_.write(reinterpret_cast<char*>(&R),sizeof(R)),qint64(sizeof(R)));QVERIFY(C.runs_->file_.flush());
        QCOMPARE(C.Position(1,Size-1),Size-1);QCOMPARE(C.At(Size-1).length_,Size);
    }
    void saveConflict()
    {
        QTemporaryDir Dir;QVERIFY(Dir.isValid());QString Path=Dir.filePath("test.bin");{QFile F(Path);QVERIFY(F.open(QIODevice::WriteOnly));F.write("original");}
        auto D=OpenBinary(Path,{});auto Changed=ReplaceBinary(D,0,D.size_,BinaryFromBytes("changed"),0,7);Request Input;
        auto Saved=SaveBinary(Input,Changed,Path,{true,D.original_.entry_.id_.revision_},{});QCOMPARE(ReadBinary(Saved,0,100),QByteArray("changed"));
        QVERIFY_EXCEPTION_THROWN(SaveBinary(Input,D,Path,{true,D.original_.entry_.id_.revision_},{}),Failure);
        QVERIFY_EXCEPTION_THROWN(SaveBinary(Input,D,Path,InspectBinaryTarget(Path,{}),{}),Failure);
        QFile F(Path);QVERIFY(F.open(QIODevice::ReadOnly));QCOMPARE(F.readAll(),QByteArray("changed"));
    }
    void canceled()
    {
        std::atomic_bool Canceled{true};Budget B;B.canceled_=&Canceled;
        QVERIFY_EXCEPTION_THROWN(CompareBinary({BinaryFromBytes("a"),BinaryFromBytes("b")},"none",B),Failure);
    }
    void view()
    {
        auto& Theme=fm::style::ThemeManager::instance();Theme.setDesign(fm::style::Design::Watercolor);Theme.install(*qobject_cast<QApplication*>(qApp));
        QByteArray A(4096,'A'),B=A;B.replace(128,8,"HEX TEST");B.append("END");auto C=CompareBinary({BinaryFromBytes(A),BinaryFromBytes(B)},"none",{});
        HexCompareView View({},C);View.resize(1500,800);View.show();QTRY_VERIFY_WITH_TIMEOUT(!View.Busy(),10000);
        View.SetOverviewVisible(true);auto* Overview=View.findChild<QWidget*>("hexOverview");QVERIFY(Overview&&Overview->isVisible());QVERIFY(!View.Dirty());
        QTest::mouseClick(Overview,Qt::LeftButton,Qt::NoModifier,QPoint(20,100));QTRY_VERIFY(!View.Busy());QVERIFY(!View.Dirty());
        View.NavigateDifference(1);QTRY_VERIFY(!View.Busy());View.ShowFind();QVERIFY(View.findChild<QLineEdit*>("hexFind")->hasFocus());
        Overview->setFocus();View.SetOverviewVisible(false);QVERIFY(!Overview->isVisible());QVERIFY(View.findChild<QWidget*>("hexPane0")->hasFocus());
        auto* ReadOnly=View.findChild<QCheckBox*>("hexReadOnly");ReadOnly->setChecked(false);
        for(auto* A:View.findChildren<QAction*>())if(A->text()==QStringLiteral("복사 →")){A->trigger();break;}
        QTRY_VERIFY(!View.Busy());QVERIFY(View.Dirty());QVERIFY(View.CanUndo(false));View.Undo(false);QTRY_VERIFY(!View.Busy());QVERIFY(!View.Dirty());View.Undo(true);QTRY_VERIFY(!View.Busy());QVERIFY(View.Dirty());View.Undo(false);QTRY_VERIFY(!View.Busy());
        auto* Pane=View.findChild<QWidget*>("hexPane0");Pane->setFocus();QTest::keyClick(Pane,Qt::Key_Home);QTRY_VERIFY(!View.Busy());QTest::keyClicks(Pane,"ff");QTRY_VERIFY(!View.Busy());QCOMPARE(ReadBinary(View.Comparison()->documents_[0],0,1),QByteArray(1,char(255)));View.Undo(false);QTRY_VERIFY(!View.Busy());QVERIFY(!View.Dirty());
        auto* List=View.findChild<QListWidget*>("hexDifferenceList");QVERIFY(List);QVERIFY(List->count()>0);
        const QString Capture=qEnvironmentVariable("HC_HEX_CAPTURE");if(!Capture.isEmpty()){QDir().mkpath(Capture);View.SetOverviewVisible(true);Theme.setScheme(fm::style::ThemeManager::Scheme::Light);QTest::qWait(100);View.grab().save(Capture+"/hex-product-light.png");Theme.setScheme(fm::style::ThemeManager::Scheme::Dark);QTest::qWait(100);View.grab().save(Capture+"/hex-product-dark.png");}
    }
};
int main(int Argc,char** Argv)
{
    QApplication App(Argc,Argv);const auto Args=App.arguments();
    if(Args.size()==4&&Args[1]=="--hex-converter"){QFile Source(Args[2]),Target(Args[3]);if(!Source.open(QIODevice::ReadOnly)||!Target.open(QIODevice::WriteOnly|QIODevice::Truncate))return 1;auto Bytes=Source.readAll();return Target.write(Bytes)==Bytes.size()?0:1;}
    HexTests Tests;return QTest::qExec(&Tests,Argc,Argv);
}
#include "tstHex.moc"
