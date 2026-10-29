#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "epubResourceTree.hpp"
#ifdef Q_OS_WIN
#include <Windows.h>
#endif
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/settings/unifiedConfig.hpp"
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUrl>
#include <Scintilla.h>
namespace hc::document {
namespace {
constexpr int DiagnosticIndicator=26,WarningMarker=10,ErrorMarker=11,DiagnosticMargin=3;
}
void EpubSourceEditor::ReloadValidationSettings() {
    auto Config=qApp->property("documentViewsConfigDir").toString();if(Config.isEmpty())Config=hc::settings::DocumentPaths().directory_;
    QJsonObject Root;QString Error;
    if(!hc::settings::UnifiedConfig::Load(hc::settings::UnifiedConfig::FileName(Config),Root,Error))return;
    // The saved panel layout shares this section but never affects a verdict.
    auto Next=Root["documentViews"].toObject()["epubEditor"].toObject();Next.remove("layout");
    if(Next==checkerSettings_)return;
    checkerSettings_=Next;checkerVerifiedToken_.clear();if(checkerProcess_ && checkerProcess_->state()!=QProcess::NotRunning)cancelEpubCheck("settings-changed");
    emit StateChanged();
}
void EpubSourceEditor::setupValidation() {
    checkerProcess_=new QProcess(this);checkerProcess_->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    checkerProcess_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* Args){Args->flags|=CREATE_NO_WINDOW;});
#endif
    checkerTimer_.setInterval(250);
    connect(diagnostics_,&QTreeWidget::itemActivated,this,[this](QTreeWidgetItem* Item,int){selectDiagnostic(Item->data(0,Qt::UserRole).toInt());});
    connect(this,&EpubSourceEditor::Changed,this,[this]{if(checkerProcess_->state()!=QProcess::NotRunning)cancelEpubCheck("source-changed");});
    connect(checkerProcess_,&QProcess::readyRead,this,[this]{if(checkerProcess_->isOpen())checkerLog_+=checkerProcess_->readAll();checkerOutput_->setPlainText(QString::fromUtf8(checkerLog_.left(128*1024)));if(checkerLog_.size()>128*1024)cancelEpubCheck("output-limit");});
    connect(checkerProcess_,&QProcess::finished,this,[this](int Code,QProcess::ExitStatus Status){finishEpubCheck(Code,Status==QProcess::NormalExit);});
    connect(checkerProcess_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError Error){if(Error==QProcess::FailedToStart)finishEpubCheck(-1,false);});
    connect(&checkerTimer_,&QTimer::timeout,this,[this]{
        if(checkerElapsed_.elapsed()>120000)cancelEpubCheck("timeout");
        else if(checkerDirectory_ && QFileInfo(QDir(checkerDirectory_->path()).filePath("report.json")).size()>8*1024*1024)cancelEpubCheck("report-limit");
    });ReloadValidationSettings();
    auto* Backend=static_cast<ScintillaQtDirectBackend*>(text_->Backend());
    Backend->Send(SCI_INDICSETSTYLE,DiagnosticIndicator,INDIC_SQUIGGLE);Backend->Send(SCI_INDICSETFORE,DiagnosticIndicator,0x0677d9);
    Backend->Send(SCI_MARKERDEFINE,WarningMarker,SC_MARK_SMALLRECT);Backend->Send(SCI_MARKERSETBACK,WarningMarker,0x0677d9);Backend->Send(SCI_MARKERSETFORE,WarningMarker,0x0677d9);
    Backend->Send(SCI_MARKERDEFINE,ErrorMarker,SC_MARK_SMALLRECT);Backend->Send(SCI_MARKERSETBACK,ErrorMarker,0x1c1cb9);Backend->Send(SCI_MARKERSETFORE,ErrorMarker,0x1c1cb9);
    Backend->Send(SCI_SETMARGINTYPEN,DiagnosticMargin,SC_MARGIN_SYMBOL);Backend->Send(SCI_SETMARGINMASKN,DiagnosticMargin,(1<<WarningMarker)|(1<<ErrorMarker));Backend->Send(SCI_SETMARGINWIDTHN,DiagnosticMargin,0);
}
void EpubSourceEditor::setDiagnostics(QJsonArray Rows) {
    diagnosticRows_=std::move(Rows);diagnostics_->clear();
    for(int I=0;I<diagnosticRows_.size();++I){
        const auto Row=diagnosticRows_[I].toObject();const auto Path=Row["resource"].toString();const int Line=Row["line"].toInt(-1);const auto Severity=Row["severity"].toString();
        if(!problemFilter_.isEmpty() && !problemFilter_.contains(Severity=="FATAL"?"ERROR":Severity=="ERROR" || Severity=="WARNING"?Severity:"INFO"))continue;
        if(!problemResource_.isEmpty() && Path!=problemResource_)continue;
        const auto Level=Severity=="ERROR" || Severity=="FATAL"?tr("오류"):Severity=="WARNING"?tr("경고"):tr("정보");
        auto* Item=new QTreeWidgetItem(diagnostics_,{Level,Path,Line>0?QString::number(Line):QStringLiteral("—"),Row["code"].toString()+" · "+Row["message"].toString()});Item->setData(0,Qt::UserRole,I);Item->setToolTip(0,Severity);Item->setToolTip(3,Row["message"].toString());
    }
    if(!diagnosticRows_.isEmpty())problems_->setCurrentWidget(diagnostics_);if(resourceModel_)resourceModel_->Refresh();markDiagnostics();updateStatus();
}
void EpubSourceEditor::markDiagnostics() {
    if(!sourceShown())return;auto* Backend=static_cast<ScintillaQtDirectBackend*>(text_->Backend());
    Backend->Send(SCI_SETINDICATORCURRENT,DiagnosticIndicator);Backend->Send(SCI_INDICATORCLEARRANGE,0,Backend->Send(SCI_GETLENGTH));Backend->Send(SCI_MARKERDELETEALL,WarningMarker);Backend->Send(SCI_MARKERDELETEALL,ErrorMarker);
    bool Any=false;
    for(const auto& Value:diagnosticRows_) {
        const auto Row=Value.toObject();const int Line=Row["line"].toInt(-1);if(Row["resource"].toString()!=current_ || Line<1 || Line>Backend->Send(SCI_GETLINECOUNT))continue;
        const auto Start=Backend->Send(SCI_POSITIONFROMLINE,Line-1),End=Backend->Send(SCI_GETLINEENDPOSITION,Line-1);auto First=Start;
        if(Row["column"].toInt(-1)>1)First=qMin(End,Backend->Send(SCI_POSITIONRELATIVECODEUNITS,Start,Row["column"].toInt()-1));else while(First<End && QChar(char(Backend->Send(SCI_GETCHARAT,First))).isSpace())++First;
        const auto Severity=Row["severity"].toString();Backend->Send(SCI_MARKERADD,Line-1,Severity=="ERROR" || Severity=="FATAL"?ErrorMarker:WarningMarker);
        if(End>First)Backend->Send(SCI_INDICATORFILLRANGE,First,End-First);Any=true;
    }
    Backend->Send(SCI_SETMARGINWIDTHN,DiagnosticMargin,Any?10:0);
}
void EpubSourceEditor::nativeDiagnostic(const epub::EditOutput& Result) {
    if(Result){setDiagnostics({});return;}
    QString Resource=Result.location_;int Line=-1,Column=-1;const auto Match=QRegularExpression("^(.+):(\\d+):(\\d+)$").match(Resource);
    if(Match.hasMatch()){Resource=Match.captured(1);Line=Match.captured(2).toInt();Column=Match.captured(3).toInt();}
    setDiagnostics({QJsonObject{{"severity","ERROR"},{"code","BASIC"},{"message",Result.error_},{"resource",Resource},{"line",Line},{"column",Column}}});
}
bool EpubSourceEditor::selectDiagnostic(int Index) {
    if(Index<0 || Index>=diagnosticRows_.size() || !ready_)return false;const auto Row=diagnosticRows_[Index].toObject();const auto Path=Row["resource"].toString();
    if(!model_.IsEditable(Path)){report(tr("이 진단은 패키지 또는 편집할 수 없는 리소스에 관한 것입니다."));return false;}
    if(!openResource(Path) || !sourceShown())return false;
    text_->goToPosition(qMax(1,Row["line"].toInt(1)),qMax(1,Row["column"].toInt(1)));static_cast<ScintillaQtDirectBackend*>(text_->Backend())->Send(SCI_SCROLLCARET);focusSource();return true;
}
bool EpubSourceEditor::standardValidationRequired()const{return checkerSettings_["requireEpubCheck"].toBool(false);}
bool EpubSourceEditor::standardValidationAccepted()const{return !standardValidationRequired() || checkerVerifiedToken_==Token();}
void EpubSourceEditor::startEpubCheck() {
    checkerVerifiedToken_.clear();checkerWorkToken_=validationToken_;checkerLog_.clear();checkerOutput_->clear();checkerReason_.clear();checkerState_="running";
    auto Java=checkerSettings_["javaPath"].toString().trimmed();if(Java.isEmpty())Java=QStandardPaths::findExecutable("java");const auto Jar=checkerSettings_["epubcheckJar"].toString().trimmed();
    if(Java.isEmpty() || !QFileInfo(Java).isFile() || !QFileInfo(Java).isAbsolute() || !QFileInfo(Jar).isFile() || !QFileInfo(Jar).isAbsolute() || QFileInfo(Jar).suffix().compare("jar",Qt::CaseInsensitive)!=0){checkerReason_="configure-java-and-epubcheck";finishEpubCheck(-1,false);return;}
    checkerDirectory_=std::make_unique<QTemporaryDir>();
    if(!checkerDirectory_->isValid()){checkerReason_="temporary-directory";finishEpubCheck(-1,false);return;}
    const auto Candidate=QDir(checkerDirectory_->path()).filePath("candidate.epub");QSaveFile File(Candidate);
    if(!File.open(QIODevice::WriteOnly) || File.write(validatedBytes_)!=validatedBytes_.size() || !File.commit()){checkerReason_="temporary-publication";finishEpubCheck(-1,false);return;}
    checkerProcess_->setWorkingDirectory(checkerDirectory_->path());checkerProcess_->setProgram(Java);
    checkerProcess_->setArguments({"-Xmx256m","-Djava.awt.headless=true","-jar",Jar,Candidate,"--json",QDir(checkerDirectory_->path()).filePath("report.json"),"--locale","en"});
    checkerElapsed_.start();checkerTimer_.start();report(tr("EPUBCheck로 저장 후보의 표준 적합성을 검사하고 있습니다…"));checkerProcess_->start();emit StateChanged();
}
void EpubSourceEditor::cancelEpubCheck(const QString& Reason) {
    if(checkerState_!="running")return;checkerReason_=Reason;
    if(checkerProcess_->state()!=QProcess::NotRunning)checkerProcess_->kill();else finishEpubCheck(-1,false);
}
void EpubSourceEditor::stopEpubCheck() {
    checkerTimer_.stop();if(!checkerProcess_)return;checkerProcess_->disconnect(this);
    if(checkerProcess_->state()!=QProcess::NotRunning){checkerProcess_->kill();checkerProcess_->waitForFinished(1000);}checkerDirectory_.reset();
}
void EpubSourceEditor::finishEpubCheck(int Code,bool Normal) {
    if(checkerState_!="running")return;
    checkerTimer_.stop();if(checkerProcess_->isOpen())checkerLog_+=checkerProcess_->readAll();checkerOutput_->setPlainText(QString::fromUtf8(checkerLog_.left(128*1024)));
    const bool Stale=checkerWorkToken_!=Token() || checkerReason_=="source-changed" || checkerReason_=="settings-changed";
    QJsonArray Rows;bool HasErrors=false,ReportValid=false;QString Version;
    if(checkerReason_.isEmpty() && checkerDirectory_ && checkerLog_.size()<=128*1024) {
        QFile Report(QDir(checkerDirectory_->path()).filePath("report.json"));
        if(Report.size()>0 && Report.size()<=8*1024*1024 && Report.open(QIODevice::ReadOnly)) {
            QJsonParseError Parse;const auto Document=QJsonDocument::fromJson(Report.readAll(),&Parse);const auto Root=Document.object();const auto Checker=Root["checker"].toObject();
            Version=Checker["checkerVersion"].toString();const auto Messages=Root["messages"];
            ReportValid=Parse.error==QJsonParseError::NoError && Document.isObject() && Messages.isArray() && Messages.toArray().size()<=2000 && !Version.isEmpty() && Version.size()<=64 && Checker["nError"].toInt(-1)>=0 && Checker["nFatal"].toInt(-1)>=0;
            HasErrors=Checker["nError"].toInt()>0 || Checker["nFatal"].toInt()>0;
            if(ReportValid)for(const auto& Value:Messages.toArray()) {
                const auto Message=Value.toObject();const auto Severity=Message["severity"].toString();
                if(Severity.isEmpty() || !Message["ID"].isString() || !Message["message"].isString()){ReportValid=false;break;}
                HasErrors|=Severity=="ERROR" || Severity=="FATAL";
                auto Locations=Message["locations"].toArray();if(Locations.isEmpty())Locations.append(QJsonObject{});
                if(Rows.size()+Locations.size()>2000){ReportValid=false;checkerReason_="report-limit";break;}
                for(const auto& LocationValue:Locations) {
                    const auto Location=LocationValue.toObject();auto Path=QUrl::fromPercentEncoding(Location["path"].toString().toUtf8());
                    if(QFileInfo(Path).isAbsolute() || Path.contains(":/") || Path.endsWith("candidate.epub"))Path.clear();
                    Rows.append(QJsonObject{{"severity",Severity.left(32)},{"code",Message["ID"].toString().left(128)},{"message",Message["message"].toString().left(4096)},{"resource",Path.left(4096)},{"line",Location["line"].toInt(-1)},{"column",Location["column"].toInt(-1)}});
                }
            }
        }
    }
    checkerDirectory_.reset();
    if(Stale){checkerState_="stale";report(tr("검사 중 소스 또는 설정이 변경되었습니다. 다시 검사해 주세요."));}
    else if(checkerReason_=="cancelled"){checkerState_="cancelled";report(tr("EPUBCheck 검사를 취소했습니다."));}
    else if(!checkerReason_.isEmpty() || !Normal || !ReportValid || (Code!=0 && !HasErrors)) {
        checkerState_="error";if(checkerReason_.isEmpty())checkerReason_="checker-failed-or-invalid-report";
        report(tr("EPUBCheck를 완료하지 못했습니다: %1 · 설정 → EPUB에서 Java 실행 파일과 EPUBCheck JAR 경로를 확인해 주세요.").arg(checkerReason_));problems_->setCurrentWidget(checkerOutput_);
    } else {
        setDiagnostics(Rows);checkerState_=HasErrors?"failed":"passed";
        if(!HasErrors && Code==0)checkerVerifiedToken_=checkerWorkToken_;
        report(HasErrors?tr("EPUBCheck %1 · 오류가 있습니다. 문제 패널의 항목을 두 번 누르면 해당 소스로 이동합니다.").arg(Version):tr("EPUBCheck %1 · 오류 없음 · 진단 %2개 (경고 포함)").arg(Version).arg(Rows.size()));
    }
    checkerVersion_=Version;emit StateChanged();if(!Stale && IsValidated() && saveAfter_)emit SaveRequested();saveAfter_=false;
}
QJsonObject EpubSourceEditor::validationSnapshot()const {
    return {{"required",standardValidationRequired()},{"state",checkerState_},{"version",checkerVersion_},{"reason",checkerReason_},{"current",!checkerVerifiedToken_.isEmpty() && checkerVerifiedToken_==Token()},{"diagnostics",diagnosticRows_}};
}
}
