#include "stdafx.h"
#include "epubSourceEditor.hpp"
#ifdef Q_OS_WIN
#include <Windows.h>
#endif
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/settings/unifiedConfig.hpp"
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
namespace hc::document {
void EpubSourceEditor::ReloadValidationSettings() {
    auto Config=qApp->property("documentViewsConfigDir").toString();if(Config.isEmpty())Config=hc::settings::DocumentPaths().directory_;
    QJsonObject Root;QString Error;
    if(!hc::settings::UnifiedConfig::Load(hc::settings::UnifiedConfig::FileName(Config),Root,Error))return;
    const auto Next=Root["documentViews"].toObject()["epubEditor"].toObject();
    if(Next==checkerSettings_)return;
    checkerSettings_=Next;checkerVerifiedToken_.clear();if(checkerProcess_ && checkerProcess_->state()!=QProcess::NotRunning)cancelEpubCheck("settings-changed");
    emit StateChanged();
}
void EpubSourceEditor::setupValidation(QVBoxLayout* Layout) {
    auto* Actions=new QHBoxLayout;auto* Basic=new QPushButton(tr("기본 검사"),this);Basic->setObjectName("epubBasicValidation");
    auto* Standard=new QPushButton(tr("EPUBCheck 검사"),this);Standard->setObjectName("epubStandardValidation");
    checkerCancel_=new QPushButton(tr("검사 취소"),this);checkerCancel_->setEnabled(false);Actions->addWidget(Basic);Actions->addWidget(Standard);Actions->addWidget(checkerCancel_);Layout->addLayout(Actions);
    diagnostics_=new QTreeWidget(this);diagnostics_->setObjectName("epubEditDiagnostics");diagnostics_->setAccessibleName(tr("EPUB 소스 검사 결과"));diagnostics_->setHeaderLabels({tr("수준"),tr("리소스 · 행"),tr("설명")});diagnostics_->setRootIsDecorated(false);diagnostics_->setMinimumHeight(90);diagnostics_->setMaximumHeight(170);diagnostics_->setColumnWidth(0,55);diagnostics_->setColumnWidth(1,150);Layout->addWidget(diagnostics_);diagnostics_->hide();
    checkerProcess_=new QProcess(this);checkerProcess_->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    checkerProcess_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* Args){Args->flags|=CREATE_NO_WINDOW;});
#endif
    checkerTimer_.setInterval(250);
    connect(Basic,&QPushButton::clicked,this,[this]{validate(false);});connect(Standard,&QPushButton::clicked,this,[this]{validate(false,true);});
    connect(checkerCancel_,&QPushButton::clicked,this,[this]{cancelEpubCheck("cancelled");});
    connect(diagnostics_,&QTreeWidget::itemActivated,this,[this](QTreeWidgetItem* Item,int){selectDiagnostic(diagnostics_->indexOfTopLevelItem(Item));});
    connect(this,&EpubSourceEditor::Changed,this,[this]{if(checkerProcess_->state()!=QProcess::NotRunning)cancelEpubCheck("source-changed");});
    connect(checkerProcess_,&QProcess::readyRead,this,[this]{if(checkerProcess_->isOpen())checkerLog_+=checkerProcess_->readAll();if(checkerLog_.size()>128*1024)cancelEpubCheck("output-limit");});
    connect(checkerProcess_,&QProcess::finished,this,[this](int Code,QProcess::ExitStatus Status){finishEpubCheck(Code,Status==QProcess::NormalExit);});
    connect(checkerProcess_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError Error){if(Error==QProcess::FailedToStart)finishEpubCheck(-1,false);});
    connect(&checkerTimer_,&QTimer::timeout,this,[this]{
        if(checkerElapsed_.elapsed()>120000)cancelEpubCheck("timeout");
        else if(checkerDirectory_ && QFileInfo(QDir(checkerDirectory_->path()).filePath("report.json")).size()>8*1024*1024)cancelEpubCheck("report-limit");
    });ReloadValidationSettings();
}
void EpubSourceEditor::setDiagnostics(QJsonArray Rows) {
    diagnosticRows_=std::move(Rows);diagnostics_->clear();
    for(const auto& Value:diagnosticRows_){const auto Row=Value.toObject();const auto Path=Row["resource"].toString();const int Line=Row["line"].toInt(-1);
        const auto Severity=Row["severity"].toString();const auto Level=Severity=="ERROR" || Severity=="FATAL"?tr("오류"):Severity=="WARNING"?tr("경고"):tr("정보");
        auto* Item=new QTreeWidgetItem(diagnostics_,{Level,Path+(Line>0?":"+QString::number(Line):QString{}),Row["code"].toString()+" · "+Row["message"].toString()});Item->setToolTip(0,Severity);Item->setToolTip(2,Row["message"].toString());}
    diagnostics_->setVisible(!diagnosticRows_.isEmpty());
}
void EpubSourceEditor::nativeDiagnostic(const epub::EditOutput& Result) {
    if(Result){setDiagnostics({});return;}
    QString Resource=Result.location_;int Line=-1,Column=-1;const auto Match=QRegularExpression("^(.+):(\\d+):(\\d+)$").match(Resource);
    if(Match.hasMatch()){Resource=Match.captured(1);Line=Match.captured(2).toInt();Column=Match.captured(3).toInt();}
    setDiagnostics({QJsonObject{{"severity","ERROR"},{"code","BASIC"},{"message",Result.error_},{"resource",Resource},{"line",Line},{"column",Column}}});
}
bool EpubSourceEditor::selectDiagnostic(int Index) {
    if(Index<0 || Index>=diagnosticRows_.size() || !ready_)return false;const auto Row=diagnosticRows_[Index].toObject();const auto Path=Row["resource"].toString();const auto Resource=resources_->findText(Path);
    if(Resource<0){status_->setText(tr("이 진단은 패키지 또는 편집할 수 없는 리소스에 관한 것입니다."));return false;}
    resources_->setCurrentIndex(Resource);if(current_!=Path || text_->isHidden())return false;
    auto Cursor=text_->textCursor();const auto Block=text_->document()->findBlockByNumber(qMax(0,Row["line"].toInt(1)-1));
    if(Block.isValid())Cursor.setPosition(Block.position()+qBound(0,Row["column"].toInt(1)-1,Block.length()-1));text_->setTextCursor(Cursor);text_->ensureCursorVisible();text_->setFocus();return true;
}
bool EpubSourceEditor::standardValidationRequired()const{return checkerSettings_["requireEpubCheck"].toBool(false);}
bool EpubSourceEditor::standardValidationAccepted()const{return !standardValidationRequired() || checkerVerifiedToken_==Token();}
void EpubSourceEditor::startEpubCheck() {
    checkerVerifiedToken_.clear();checkerWorkToken_=validationToken_;checkerLog_.clear();checkerReason_.clear();checkerState_="running";
    auto Java=checkerSettings_["javaPath"].toString().trimmed();if(Java.isEmpty())Java=QStandardPaths::findExecutable("java");const auto Jar=checkerSettings_["epubcheckJar"].toString().trimmed();
    if(Java.isEmpty() || !QFileInfo(Java).isFile() || !QFileInfo(Java).isAbsolute() || !QFileInfo(Jar).isFile() || !QFileInfo(Jar).isAbsolute() || QFileInfo(Jar).suffix().compare("jar",Qt::CaseInsensitive)!=0){checkerReason_="configure-java-and-epubcheck";finishEpubCheck(-1,false);return;}
    checkerDirectory_=std::make_unique<QTemporaryDir>();
    if(!checkerDirectory_->isValid()){checkerReason_="temporary-directory";finishEpubCheck(-1,false);return;}
    const auto Candidate=QDir(checkerDirectory_->path()).filePath("candidate.epub");QSaveFile File(Candidate);
    if(!File.open(QIODevice::WriteOnly) || File.write(validatedBytes_)!=validatedBytes_.size() || !File.commit()){checkerReason_="temporary-publication";finishEpubCheck(-1,false);return;}
    checkerProcess_->setWorkingDirectory(checkerDirectory_->path());checkerProcess_->setProgram(Java);
    checkerProcess_->setArguments({"-Xmx256m","-Djava.awt.headless=true","-jar",Jar,Candidate,"--json",QDir(checkerDirectory_->path()).filePath("report.json"),"--locale","en"});
    checkerElapsed_.start();checkerTimer_.start();checkerCancel_->setEnabled(true);status_->setText(tr("EPUBCheck로 저장 후보의 표준 적합성을 검사하고 있습니다…"));checkerProcess_->start();emit StateChanged();
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
    checkerTimer_.stop();checkerCancel_->setEnabled(false);if(checkerProcess_->isOpen())checkerLog_+=checkerProcess_->readAll();
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
    if(Stale){checkerState_="stale";status_->setText(tr("검사 중 소스 또는 설정이 변경되었습니다. 다시 검사해 주세요."));}
    else if(checkerReason_=="cancelled"){checkerState_="cancelled";status_->setText(tr("EPUBCheck 검사를 취소했습니다."));}
    else if(!checkerReason_.isEmpty() || !Normal || !ReportValid || (Code!=0 && !HasErrors)) {
        checkerState_="error";if(checkerReason_.isEmpty())checkerReason_="checker-failed-or-invalid-report";
        status_->setText(tr("EPUBCheck를 완료하지 못했습니다: %1\n설정 → EPUB에서 Java 실행 파일과 EPUBCheck JAR 경로를 확인해 주세요.").arg(checkerReason_)+(checkerLog_.isEmpty()?QString{}:"\n"+QString::fromUtf8(checkerLog_.left(2048))));
    } else {
        setDiagnostics(Rows);checkerState_=HasErrors?"failed":"passed";
        if(!HasErrors && Code==0)checkerVerifiedToken_=checkerWorkToken_;
        status_->setText(HasErrors?tr("EPUBCheck %1 · 오류가 있습니다. 목록에서 진단을 활성화하면 해당 소스로 이동합니다.").arg(Version):tr("EPUBCheck %1 · 오류 없음 · 진단 %2개 (경고 포함)").arg(Version).arg(Rows.size()));
    }
    checkerVersion_=Version;emit StateChanged();if(!Stale && IsValidated() && saveAfter_)emit SaveRequested();saveAfter_=false;
}
QJsonObject EpubSourceEditor::validationSnapshot()const {
    return {{"required",standardValidationRequired()},{"state",checkerState_},{"version",checkerVersion_},{"reason",checkerReason_},{"current",!checkerVerifiedToken_.isEmpty() && checkerVerifiedToken_==Token()},{"diagnostics",diagnosticRows_}};
}
}
