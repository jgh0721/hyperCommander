#include "stdafx.h"
#include "readingNavigation.hpp"
#include "readingAnnotations.hpp"
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonDocument>
#include <QSet>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrentRun>
namespace hc::epub::poc {
void ReadingNavigation::addAnnotations(QTabWidget* Tabs) {
    annotationPanel_=new QWidget(Tabs);auto* Layout=new QVBoxLayout(annotationPanel_);Tabs->addTab(annotationPanel_,tr("형광펜·메모"));
    annotationList_=new QListWidget(annotationPanel_);annotationList_->setObjectName("epubAnnotations");annotationList_->setAccessibleName(tr("형광펜과 메모 목록"));annotationList_->setMaximumHeight(180);Layout->addWidget(annotationList_);
    annotationQuote_=new QLabel(annotationPanel_);annotationQuote_->setTextFormat(Qt::PlainText);annotationQuote_->setWordWrap(true);annotationQuote_->setAccessibleName(tr("메모의 인용 문장"));Layout->addWidget(annotationQuote_);
    auto* Form=new QFormLayout;annotationColor_=new QComboBox(annotationPanel_);annotationColor_->setObjectName("epubAnnotationColor");for(const auto& Entry:QList<QPair<QString,QString>>{{tr("노랑"),"#ffde59"},{tr("민트"),"#63d8ad"},{tr("분홍"),"#f38ab2"},{tr("파랑"),"#78baf5"}})annotationColor_->addItem(Entry.first,Entry.second);Form->addRow(tr("형광펜 색"),annotationColor_);Layout->addLayout(Form);
    annotationNote_=new QPlainTextEdit(annotationPanel_);annotationNote_->setObjectName("epubAnnotationNote");annotationNote_->setAccessibleName(tr("메모 내용"));annotationNote_->setPlaceholderText(tr("메모 입력 후 선택 문장에 추가하거나 선택 메모를 수정하세요."));annotationNote_->setMaximumHeight(110);Layout->addWidget(annotationNote_);
    const auto Button=[&](const QString& Text,const QString& Name){auto* Value=new QPushButton(Text,annotationPanel_);Value->setObjectName(Name);Layout->addWidget(Value);return Value;};
    auto* Add=Button(tr("선택한 본문에 형광펜·메모 추가"),"epubAnnotationAdd");auto* Update=Button(tr("선택 메모 수정"),"epubAnnotationUpdate");auto* Link=Button(tr("선택 메모를 현재 본문 선택에 다시 연결"),"epubAnnotationLink");auto* Go=Button(tr("선택 메모 위치로 이동"),"epubAnnotationGo");auto* Remove=Button(tr("선택 메모 삭제"),"epubAnnotationRemove");
    auto* Reload=Button(tr("미저장 메모 변경 취소·다시 불러오기"),"epubAnnotationReload");
    auto* Import=Button(tr("메모 가져오기·다른 판본으로 연결…"),"epubAnnotationImport");auto* Export=Button(tr("형광펜·메모 내보내기…"),"epubAnnotationExport");auto* Cancel=Button(tr("판본 연결 취소"),"epubAnnotationCancel");Cancel->setEnabled(false);
    annotationStatus_=new QLabel(tr("본문을 선택한 뒤 색과 메모를 지정해 추가합니다. 메모는 EPUB 원본과 별도로 보관합니다."),annotationPanel_);annotationStatus_->setTextFormat(Qt::PlainText);annotationStatus_->setWordWrap(true);Layout->addWidget(annotationStatus_);
    connect(annotationList_,&QListWidget::currentRowChanged,this,[this](int Row){if(Row<0 || Row>=annotations_.size())return;const auto Value=annotations_[Row].toObject();annotationQuote_->setText(Value["quote"].toString());annotationNote_->setPlainText(Value["note"].toString());annotationColor_->setCurrentIndex(qMax(0,annotationColor_->findData(Value["color"])));annotationStatus_->setText(Value["unresolved"].toBool()?tr("이전 판본의 위치를 자동으로 연결하지 못했습니다: %1").arg(Value["reason"].toString()):tr("선택한 메모 · 수정 버튼으로 저장합니다."));});
    connect(Add,&QPushButton::clicked,this,[this]{annotationAction("annotation-add",QJsonObject{{"note",annotationNote_->toPlainText()},{"color",annotationColor_->currentData() .toString()}});});
    connect(Link,&QPushButton::clicked,this,[this]{if(auto* Item=annotationList_->currentItem())annotationAction("annotation-link",QJsonObject{{"id",Item->data(Qt::UserRole).toString()}});});
    connect(Update,&QPushButton::clicked,this,[this]{if(auto* Item=annotationList_->currentItem())annotationAction("annotation-update",QJsonObject{{"id",Item->data(Qt::UserRole).toString()},{"note",annotationNote_->toPlainText()},{"color",annotationColor_->currentData().toString()}});});
    const auto Open=[this]{if(auto* Item=annotationList_->currentItem())annotationAction("annotation-go",Item->data(Qt::UserRole).toString());};connect(Go,&QPushButton::clicked,this,Open);connect(annotationList_,&QListWidget::itemActivated,this,[Open](QListWidgetItem*){Open();});
    connect(Remove,&QPushButton::clicked,this,[this]{if(auto* Item=annotationList_->currentItem())annotationAction("annotation-remove",Item->data(Qt::UserRole).toString());});
    connect(Reload,&QPushButton::clicked,this,[this]{annotationAction("annotation-reload",{});});
    connect(Import,&QPushButton::clicked,this,[this]{annotationFile(true);});connect(Export,&QPushButton::clicked,this,[this]{annotationFile(false);});connect(Cancel,&QPushButton::clicked,this,[this]{annotationAction("annotation-transfer-cancel",{});});refreshAnnotations();
}
void ReadingNavigation::refreshAnnotations() {
    if(!annotationList_)return;const auto Current=annotationList_->currentItem()?annotationList_->currentItem()->data(Qt::UserRole).toString():QString{};const QSignalBlocker Block(annotationList_);annotationList_->clear();
    for(const auto& Value:annotations_){const auto Row=Value.toObject();const auto Label=(Row["unresolved"].toBool()?tr("[미연결] "):QString{})+Row["quote"].toString().simplified().left(80)+(Row["note"].toString().isEmpty()?QString{}:tr(" · 메모"));auto* Item=new QListWidgetItem(Label,annotationList_);Item->setData(Qt::UserRole,Row["id"]);Item->setToolTip(Row["note"].toString());if(Row["id"]==Current)annotationList_->setCurrentItem(Item);}
    const bool Idle=!transient_ && !annotationBusy_ && annotationPending_<0 && transferPending_.isEmpty();
    for(const auto& Name:QStringList{"epubAnnotationAdd","epubAnnotationLink","epubAnnotationUpdate","epubAnnotationRemove","epubAnnotationImport","epubAnnotationReload"})if(auto* Button=findChild<QPushButton*>(Name))Button->setEnabled(Idle && !readFailed_);
    if(auto* Button=findChild<QPushButton*>("epubAnnotationExport"))Button->setEnabled(!transient_ && !annotationBusy_ && !annotations_.isEmpty());
    if(auto* Button=findChild<QPushButton*>("epubAnnotationCancel"))Button->setEnabled(!transferPending_.isEmpty());
    annotationNote_->setEnabled(Idle);annotationColor_->setEnabled(Idle);
    if(transient_)annotationStatus_->setText(tr("편집 미리보기에서는 형광펜·메모와 책갈피를 보관하지 않습니다. 저장한 EPUB을 리더로 열어 추가하세요."));
}
void ReadingNavigation::syncAnnotations() {
    if(!annotationsStarted_ || transient_)return;
    if(annotationPending_>=0 || annotationMode_=="transfer"){annotationNeedsSync_=true;return;}annotationNeedsSync_=false;
    if(!annotations_.isEmpty() || annotationsSynced_){send("annotations-set",annotations_);annotationsSynced_=true;}
}
bool ReadingNavigation::annotationFits(const QJsonArray& Rows) const {
    return ReadingAnnotations::Valid(Rows) && QJsonDocument(QJsonObject{{"schemaVersion",1},{"locator",remember_?locator_:QString{}},{"bookmarks",bookmarks_},{"annotations",Rows}}).toJson().size()<=248*1024;
}
bool ReadingNavigation::putAnnotation(QJsonObject Row) {
    if(!ReadingAnnotations::Valid(Row))return false;int Index=-1;
    for(int I=0;I<annotations_.size();++I)if(annotations_[I].toObject()["id"]==Row["id"])Index=I;
    auto Candidate=annotations_;if(Index<0){if(Candidate.size()>=32)return false;Candidate.append(Row);}else Candidate[Index]=Row;
    if(!annotationFits(Candidate))return false;annotations_=Candidate;
    bool Replaced=false;for(int I=0;I<annotationAdditions_.size();++I)if(annotationAdditions_[I].toObject()["id"]==Row["id"]){annotationAdditions_[I]=Row;Replaced=true;}if(!Replaced)annotationAdditions_.append(Row);
    annotationRemovals_.removeAll(Row["id"].toString());saveTimer_.start();refreshAnnotations();syncAnnotations();return true;
}
bool ReadingNavigation::annotationAction(const QString& Action,const QJsonValue& Value) {
    if(Action=="annotations-transient") {
        if(Value!=QJsonValue(true) || !additions_.isEmpty() || !removals_.isEmpty() || !annotationAdditions_.isEmpty() || !annotationRemovals_.isEmpty())return false;transient_=true;annotationsStarted_=true;
        for(const auto& Name:QStringList{"epubBookmarkAdd","epubBookmarkRemove"})if(auto* Button=findChild<QPushButton*>(Name))Button->setEnabled(false);refreshAnnotations();return true;
    }
    if(Action=="focus-notes"){if(auto* Tabs=findChild<QTabWidget*>())Tabs->setCurrentIndex(3);annotationList_->setFocus(Qt::ShortcutFocusReason);return true;}
    if(transient_ || !started_ || restoring_)return false;
    if(Action=="annotation-transfer-cancel"){send("annotation-cancel",{});transferPending_={};transferOutput_={};annotationPending_=-1;annotationMode_.clear();refreshAnnotations();annotationStatus_->setText(tr("판본 연결을 취소했습니다. 기존 메모는 유지됩니다."));return true;}
    if(Action=="annotation-select"){for(int I=0;I<annotations_.size();++I)if(annotations_[I].toObject()["id"]==Value){annotationList_->setCurrentRow(I);return true;}return false;}
    if(annotationBusy_ || annotationPending_>=0 || !transferPending_.isEmpty() || readFailed_)return false;
    if(Action=="annotation-reload"){reloadAnnotations();return true;}
    if(Action=="annotation-add" || Action=="annotation-link") {
        const auto Input=Value.toObject();annotationDraft_={{"id",QUuid::createUuid().toString(QUuid::Id128)},{"note",Input["note"].toString()},{"color",Input["color"].toString("#ffde59")},{"quote","pending"},{"prefix",""},{"suffix",""},{"locator","epubcfi(/6/2!/4/2:0)"},{"unresolved",false}};
        if(Action=="annotation-link") {bool Found=false;for(const auto& Row:annotations_)if(Row.toObject()["id"]==Input["id"]){annotationDraft_=Row.toObject();Found=true;}if(!Found)return false;}
        if(!ReadingAnnotations::Valid(annotationDraft_) || (Action=="annotation-add" && annotations_.size()>=32))return false;
        QJsonValue Target=Input.contains("locator")?Input["locator"]:QJsonValue{};send("annotation-capture",Target);annotationPending_=serial_;annotationMode_=Action=="annotation-link"?"link":"capture";refreshAnnotations();return true;
    }
    if(Action=="annotations-import") {const auto Root=Value.toObject();if(Root["schemaVersion"]!=QJsonValue(1) || Root["kind"]!="hypercommander.epub.annotations" || !QRegularExpression("^[a-f0-9]{64}$").match(Root["book"].toString()).hasMatch() || !Root["annotations"].isArray() || !ReadingAnnotations::Valid(Root["annotations"].toArray()))return false;beginAnnotationTransfer(Root["book"].toString(),Root["annotations"].toArray());return true;}
    for(int I=0;I<annotations_.size();++I) {
        auto Row=annotations_[I].toObject();const auto Id=Action=="annotation-update"?Value.toObject()["id"].toString():Value.toString();if(Row["id"]!=Id)continue;
        if(Action=="annotation-go"){if(Row["unresolved"].toBool())return false;send("locate",Row["locator"]);return true;}
        if(Action=="annotation-remove"){annotations_.removeAt(I);annotationRemovals_.append(Id);saveTimer_.start();refreshAnnotations();syncAnnotations();return true;}
        if(Action=="annotation-update"){Row["note"]=Value.toObject()["note"];Row["color"]=Value.toObject()["color"];return putAnnotation(Row);}
    }
    return false;
}
void ReadingNavigation::pollAnnotations(const QJsonObject& State) {
    if(!annotationsStarted_){annotationsStarted_=true;syncAnnotations();}
    if(annotationNeedsSync_ && annotationPending_<0 && annotationMode_.isEmpty())syncAnnotations();
    const auto Result=State["annotationAction"].toObject();if(annotationPending_<0 || Result["serial"].toInteger(-1)!=annotationPending_ || Result["busy"].toBool())return;
    annotationPending_=-1;
    if(annotationMode_=="capture" || annotationMode_=="link") {
        const bool Linking=annotationMode_=="link";annotationMode_.clear();if(Result.contains("error")){refreshAnnotations();annotationStatus_->setText(Result["error"].toString());return;}
        const auto Selection=Result["selection"].toObject();auto Row=annotationDraft_;for(const auto& Key:QStringList{"locator","quote","prefix","suffix"})Row[Key]=Selection[Key];
        for(const auto& Value:annotations_)if(Value.toObject()["locator"]==Row["locator"] && !Value.toObject()["unresolved"].toBool() && (!Linking || Value.toObject()["id"]!=Row["id"])) {refreshAnnotations();annotationStatus_->setText(tr("이 범위에는 다른 메모가 있습니다. 기존 메모를 수정하거나 다른 본문 범위를 선택하세요."));return;}
        Row["unresolved"]=false;Row.remove("reason");
        if(!putAnnotation(Row)){refreshAnnotations();annotationStatus_->setText(tr("선택 범위가 없거나 인용·위치·독서 기록의 용량 제한을 초과했습니다."));}else annotationStatus_->setText(tr("형광펜·메모를 추가했습니다."));return;
    }
    if(annotationMode_=="transfer" && !transferPending_.isEmpty()) {
        auto Row=transferPending_.takeAt(0).toObject();const auto Locator=Result["locator"].toString();Row["locator"]=Locator;Row["unresolved"]=Locator.isEmpty();Row["reason"]=Result.contains("error")?Result["error"].toString().left(256):Locator.isEmpty()?tr("일치하는 문장이 없거나 여러 곳에 있어 자동 연결하지 않았습니다."):QString{};
        if(!ReadingAnnotations::Valid(Row)){Row["locator"]="";Row["unresolved"]=true;Row["reason"]=tr("위치 형식 또는 길이를 지원하지 않습니다.");}transferOutput_.append(Row);nextAnnotationTransfer();
    }
}
void ReadingNavigation::beginAnnotationTransfer(const QString& Source,const QJsonArray& Rows) {
    if(annotations_.size()+Rows.size()>32){annotationStatus_->setText(tr("책 한 권의 형광펜·메모는 32개까지 보관합니다."));return;}
    transferOutput_={};transferPending_={};transferSource_=Source;
    for(const auto& Value:Rows){auto Row=Value.toObject();Row["id"]=QUuid::createUuid().toString(QUuid::Id128);
        if(Source==book_ && !Row["unresolved"].toBool())transferOutput_.append(Row);
        else {const auto Original=Row["locator"].toString();if(!Original.isEmpty() || !Row.contains("sourceBook"))Row["sourceBook"]=Source;if(!Original.isEmpty())Row["sourceLocator"]=Original;Row["locator"]="";Row["unresolved"]=true;transferPending_.append(Row);}}
    annotationMode_="transfer";nextAnnotationTransfer();
}
void ReadingNavigation::nextAnnotationTransfer() {
    if(!transferPending_.isEmpty()){const auto Row=transferPending_.first().toObject();send("annotation-resolve",QJsonObject{{"quote",Row["quote"]},{"prefix",Row["prefix"]},{"suffix",Row["suffix"]}});annotationPending_=serial_;refreshAnnotations();annotationStatus_->setText(tr("다른 판본에서 문장과 앞뒤 문맥을 확인 중… %1개 남음").arg(transferPending_.size()));return;}
    QSet<QString> Used;for(const auto& Value:annotations_)if(!Value.toObject()["unresolved"].toBool())Used.insert(Value.toObject()["locator"].toString());
    for(int I=0;I<transferOutput_.size();++I){auto Row=transferOutput_[I].toObject();if(!Row["unresolved"].toBool()) {const auto Locator=Row["locator"].toString();if(Used.contains(Locator)){Row["sourceBook"]=transferSource_;Row["sourceLocator"]=Locator;Row["locator"]="";Row["unresolved"]=true;Row["reason"]=tr("같은 범위에 다른 메모가 있어 자동으로 연결하지 않았습니다.");transferOutput_[I]=Row;}else Used.insert(Locator);}}
    auto Candidate=annotations_;for(const auto& Value:transferOutput_)Candidate.append(Value);if(!annotationFits(Candidate)){transferOutput_={};annotationMode_.clear();refreshAnnotations();annotationStatus_->setText(tr("독서 기록 용량 제한을 초과해 가져오지 않았습니다. 기존 메모와 가져온 파일은 유지됩니다."));return;}
    int Missing=0;for(const auto& Value:transferOutput_){Missing+=Value.toObject()["unresolved"].toBool()?1:0;putAnnotation(Value.toObject());}const int Total=transferOutput_.size();transferOutput_={};annotationMode_.clear();syncAnnotations();refreshAnnotations();annotationStatus_->setText(tr("메모 %1개 가져옴 · 자동 연결 %2개 · 미연결 %3개. 미연결 메모는 임의 위치에 표시하지 않습니다.").arg(Total).arg(Total-Missing).arg(Missing));
}
void ReadingNavigation::annotationFile(bool Import) {
    if(transient_ || annotationBusy_ || annotationPending_>=0 || !transferPending_.isEmpty())return;
    auto* Dialog=new QFileDialog(this,Import?tr("형광펜·메모 가져오기"):tr("형광펜·메모 내보내기"));Dialog->setAttribute(Qt::WA_DeleteOnClose);Dialog->setNameFilter(tr("EPUB 형광펜·메모 (*.json)"));Dialog->setDefaultSuffix("json");Dialog->setAcceptMode(Import?QFileDialog::AcceptOpen:QFileDialog::AcceptSave);Dialog->setFileMode(Import?QFileDialog::ExistingFile:QFileDialog::AnyFile);
    connect(Dialog,&QFileDialog::fileSelected,this,[this,Import](const QString& Path){if(Path.isEmpty() || transient_)return;annotationBusy_=true;refreshAnnotations();
        auto* Worker=new QFutureWatcher<ReadingAnnotationTransfer>(this);connect(Worker,&QFutureWatcher<ReadingAnnotationTransfer>::finished,this,[this,Worker,Import]{const auto Result=Worker->result();Worker->deleteLater();annotationBusy_=false;refreshAnnotations();if(!Result)annotationStatus_->setText(Result.error);else if(Import)beginAnnotationTransfer(Result.book,Result.annotations);else annotationStatus_->setText(tr("형광펜·메모를 내보냈습니다."));});
        Worker->setFuture(QtConcurrent::run([Path,Import,Book=book_,Title=bookTitle_,Rows=annotations_]{if(Import)return ReadingAnnotations::Import(Path);return ReadingAnnotationTransfer{{},{},{},ReadingAnnotations::Export(Path,Book,Title.left(512),Rows)};}));
    });Dialog->open();
}
}
