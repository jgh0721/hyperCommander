#include "stdafx.h"
#include "readingNavigation.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTabWidget>
#include <QTreeWidget>
#include <QSignalBlocker>
#include <QUuid>
#include <QVBoxLayout>
namespace hc::epub::poc {
namespace {
bool cfi(const QString& Value) {return Value.size()<=8192 && QRegularExpression("^epubcfi\\([^\\x{0}-\\x{1f}\\x{7f}-\\x{9f}\\x{2028}\\x{2029}]+\\)$").match(Value).hasMatch();}
QJsonObject read(const QString& Path) {
    if(!QFileInfo::exists(Path))return {{"schemaVersion",1},{"locator",""},{"bookmarks",QJsonArray{}}};
    QFile File(Path);if(QFileInfo(Path).isSymLink() || !File.open(QIODevice::ReadOnly) || File.size()>256*1024)throw QString("읽기 기록을 읽을 수 없습니다.");
    QJsonParseError Error;const auto Document=QJsonDocument::fromJson(File.read(256*1024+1),&Error);const auto Root=Document.object();
    if(Error.error!=QJsonParseError::NoError || Root["schemaVersion"]!=QJsonValue(1) || !Root["locator"].isString()
        || (!Root["locator"].toString().isEmpty() && !cfi(Root["locator"].toString())) || !Root["bookmarks"].isArray() || Root["bookmarks"].toArray().size()>128)throw QString("읽기 기록이 손상되었습니다. 기존 파일을 유지합니다.");
    QSet<QString> Ids;for(const auto& Value:Root["bookmarks"].toArray()) {const auto Mark=Value.toObject();const auto Id=Mark["id"].toString();
        if(!QRegularExpression("^[a-f0-9]{32}$").match(Id).hasMatch() || Ids.contains(Id) || !cfi(Mark["locator"].toString()) || !Mark["label"].isString() || Mark["label"].toString().size()>160)throw QString("책갈피가 손상되었습니다.");Ids.insert(Id);}
    return Root;
}
}
ReadingNavigation::ReadingNavigation(const QString& Settings,const QString& Book,Dispatch Send,QWidget* Parent):QWidget(Parent),send_(std::move(Send)) {
    if(!Settings.isEmpty()) {
        const auto Directory=QDir(QFileInfo(Settings).absolutePath()).filePath("epub-state");
        if(QFileInfo(Directory).isSymLink() || !QDir().mkpath(Directory)) {error_=tr("읽기 기록 폴더를 만들 수 없습니다.");readFailed_=true;}
        else {path_=QDir(Directory).filePath(Book+".json");try {const auto Root=read(path_);restore_=saved_=Root["locator"].toString();bookmarks_=Root["bookmarks"].toArray();}catch(const QString& Error){error_=Error;readFailed_=true;}}
    }
    auto* Layout=new QVBoxLayout(this);Layout->setContentsMargins(0,0,0,0);auto* Tabs=new QTabWidget(this);Layout->addWidget(Tabs);
    toc_=new QTreeWidget(Tabs);toc_->setObjectName("epubToc");toc_->setAccessibleName(tr("목차"));toc_->setHeaderHidden(true);
    toc_->setStyleSheet("QTreeWidget:focus { border: 2px solid palette(highlight); }");
    toc_->setMinimumHeight(180);Tabs->addTab(toc_,tr("목차"));
    const auto OpenToc=[this](QTreeWidgetItem* Item){const auto Index=Item->data(0,Qt::UserRole).toInt();if(navigation_[Index].toObject()["path"].toString().isEmpty())Item->setExpanded(!Item->isExpanded());else Execute("toc",Index);};
    connect(toc_,&QTreeWidget::itemClicked,this,OpenToc);connect(toc_,&QTreeWidget::itemActivated,this,OpenToc);
    auto* Search=new QWidget(Tabs);auto* SearchLayout=new QVBoxLayout(Search);query_=new QLineEdit(Search);query_->setMaxLength(128);query_->setPlaceholderText(tr("책 전체에서 검색"));query_->setObjectName("epubSearch");query_->setAccessibleName(tr("책 전체 검색어"));query_->setAccessibleDescription(tr("Ctrl+F로 이동하고 Enter로 검색합니다."));
    auto* Find=new QPushButton(tr("검색"),Search);results_=new QListWidget(Search);results_->setObjectName("epubSearchResults");results_->setAccessibleName(tr("본문 검색 결과"));results_->setMaximumHeight(140);
    SearchLayout->addWidget(query_);SearchLayout->addWidget(Find);SearchLayout->addWidget(results_);Tabs->addTab(Search,tr("검색"));
    auto* Marks=new QWidget(Tabs);auto* MarkLayout=new QVBoxLayout(Marks);marks_=new QListWidget(Marks);marks_->setObjectName("epubBookmarks");marks_->setAccessibleName(tr("책갈피 목록"));marks_->setMaximumHeight(140);
    auto* Add=new QPushButton(tr("현재 위치 책갈피"),Marks);auto* Remove=new QPushButton(tr("선택 책갈피 삭제"),Marks);MarkLayout->addWidget(marks_);MarkLayout->addWidget(Add);MarkLayout->addWidget(Remove);Tabs->addTab(Marks,tr("책갈피"));
    status_=new QLabel(this);status_->setTextFormat(Qt::PlainText);status_->setWordWrap(true);Layout->addWidget(status_);
    connect(Find,&QPushButton::clicked,this,[this]{Execute("search",query_->text());});connect(query_,&QLineEdit::returnPressed,this,[this]{Execute("search",query_->text());});
    connect(results_,&QListWidget::itemActivated,this,[this](QListWidgetItem* Item){Execute("locate",Item->data(Qt::UserRole).toString());});
    connect(marks_,&QListWidget::itemActivated,this,[this](QListWidgetItem* Item){Execute("locate",Item->data(Qt::UserRole+1).toString());});
    connect(Add,&QPushButton::clicked,this,[this]{Execute("bookmark-add");});connect(Remove,&QPushButton::clicked,this,[this]{if(auto* Item=marks_->currentItem())Execute("bookmark-remove",Item->data(Qt::UserRole).toString());});
    saveTimer_.setSingleShot(true);saveTimer_.setInterval(700);connect(&saveTimer_,&QTimer::timeout,this,[this]{save();});refresh();
}
ReadingNavigation::~ReadingNavigation(){saveTimer_.stop();save();}
void ReadingNavigation::send(const QString& Action,const QJsonValue& Value){send_({{"action",Action},{"value",Value},{"serial",++serial_}});}
bool ReadingNavigation::Execute(const QString& Action,const QJsonValue& Value) {
    if(Action=="focus-search") {if(auto* Tabs=findChild<QTabWidget*>())Tabs->setCurrentIndex(1);query_->setFocus(Qt::ShortcutFocusReason);query_->selectAll();return true;}
    if(Action=="focus-toc") {if(auto* Tabs=findChild<QTabWidget*>())Tabs->setCurrentIndex(0);toc_->setFocus(Qt::ShortcutFocusReason);return true;}
    if(Action=="focus-bookmarks") {if(auto* Tabs=findChild<QTabWidget*>())Tabs->setCurrentIndex(2);marks_->setFocus(Qt::ShortcutFocusReason);return true;}
    if(!started_ || restoring_)return false;
    if(QStringList{"next","previous","back","next-chapter","previous-chapter"}.contains(Action)){send(Action,Value);return true;}
    if(Action=="chapter"){if(!Value.isDouble() || Value.toDouble()!=Value.toInt(-1) || Value.toInt(-1)<0 || Value.toInt()>=spineCount_)return false;send(Action,Value);return true;}
    if(Action=="toc") {if(!Value.isDouble() || Value.toInt()<0 || Value.toInt()>=navigation_.size() || navigation_[Value.toInt()].toObject()["path"].toString().isEmpty())return false;send(Action,Value);return true;}
    if(Action=="search") {if(!Value.isString() || Value.toString().trimmed().isEmpty() || Value.toString().size()>128)return false;results_->clear();status_->setText(tr("책 전체에서 검색 중…"));send(Action,Value);return true;}
    if(Action=="bookmark-add") {
        if(!cfi(locator_) || bookmarks_.size()>=128)return false;
        const auto Mark=QJsonObject{{"id",QUuid::createUuid().toString(QUuid::Id128)},{"locator",locator_},{"label",Value.toString().isEmpty()?tr("책갈피 %1").arg(bookmarks_.size()+1):Value.toString().left(160)}};
        bookmarks_.append(Mark);additions_.append(Mark);saveTimer_.start();refresh();return true;
    }
    if(Action=="bookmark-remove") {for(qsizetype I=0;I<bookmarks_.size();++I)if(bookmarks_[I].toObject()["id"]==Value){removals_.append(Value.toString());bookmarks_.removeAt(I);saveTimer_.start();refresh();return true;}return false;}
    if((Action=="locate" || Action=="preview-location") && !cfi(Value.toString()))return false;
    if(QStringList{"locate","preview-location","image","footnote","close-popup"}.contains(Action)){send(Action,Value);return true;}
    return false;
}
void ReadingNavigation::Poll(const QJsonObject& State) {
    if(State["status"]!="ready")return;
    const auto Current=State["tocIndex"].toInt(-1);
    if(Current>=0 && Current!=currentToc_) {currentToc_=Current;QTreeWidgetItemIterator It(toc_);for(;*It;++It){
        const bool Active=(*It)->data(0,Qt::UserRole).toInt()==Current;auto Font=(*It)->font(0);Font.setBold(Active);(*It)->setFont(0,Font);
        if(Active){if(!toc_->hasFocus()){QSignalBlocker Block(toc_);toc_->setCurrentItem(*It);}for(auto* Parent=(*It)->parent();Parent;Parent=Parent->parent())Parent->setExpanded(true);}}}
    if(!started_) {started_=true;if(resume_ && remember_ && !restore_.isEmpty()){send("locate",restore_);restoreSerial_=serial_;restoring_=true;return;}}
    const auto Result=State["readerAction"].toObject();const auto Serial=Result["serial"].toInteger();
    if(restoring_) {if(Serial!=restoreSerial_ || Result["busy"].toBool())return;restoring_=false;if(Result.contains("error"))error_=tr("이전 읽기 위치를 복원하지 못했습니다.");}
    const auto Locator=State["locator"].toString();if(cfi(Locator) && Locator!=locator_){locator_=Locator;if(remember_)saveTimer_.start();}
    if(Serial>seen_ && !Result["busy"].toBool()) {
        seen_=Serial;
        if(Result.contains("error"))status_->setText(Result["error"].toString());
        else if(Result.contains("hits")) {results_->clear();for(const auto& Hit:Result["hits"].toArray()){const auto H=Hit.toObject();auto* Item=new QListWidgetItem(H["excerpt"].toString(),results_);Item->setData(Qt::UserRole,H["cfi"]);}
            status_->setText(tr("검색 결과 %1개%2").arg(results_->count()).arg(Result["limited"].toBool()?tr(" · 검색 한도에 도달했습니다"):QString{}));}
    }
}
void ReadingNavigation::SetPublication(const QJsonObject& Publication) {
    spineCount_=Publication["spine"].toArray().size();
    navigation_=Publication["navigation"].toArray();currentToc_=-1;toc_->clear();QList<QTreeWidgetItem*> Parents;
    for(int Index=0;Index<navigation_.size();++Index) {
        const auto Entry=navigation_[Index].toObject();const int Depth=qBound(0,Entry["depth"].toInt(),static_cast<int>(Parents.size()));
        while(Parents.size()>Depth)Parents.removeLast();
        auto* Item=new QTreeWidgetItem(QStringList{Entry["label"].toString().isEmpty()?Entry["path"].toString():Entry["label"].toString()});
        Item->setData(0,Qt::UserRole,Index);Item->setToolTip(0,"<qt>"+Item->text(0).toHtmlEscaped()+"</qt>");
        if(Parents.isEmpty())toc_->addTopLevelItem(Item);else Parents.last()->addChild(Item);Parents.append(Item);
    }
    toc_->expandToDepth(0);
}
void ReadingNavigation::ConfigureProfile(const QJsonObject& Profile) {
    resume_=Profile["resume"].toBool(true);remember_=!Profile["privateReading"].toBool();
    const auto Epoch=Profile["historyEpoch"].toInteger();
    if(historyEpoch_>=0 && historyEpoch_!=Epoch){restore_.clear();saved_=locator_;saveTimer_.stop();}
    historyEpoch_=Epoch;
}
QJsonObject ReadingNavigation::Snapshot()const {return {{"bookmarks",bookmarks_},{"readingStateError",error_},{"readingStateRestoring",restoring_},{"readingStateLoaded",started_}};}
void ReadingNavigation::refresh(){marks_->clear();for(const auto& Value:bookmarks_){const auto M=Value.toObject();auto* Item=new QListWidgetItem(M["label"].toString(),marks_);Item->setData(Qt::UserRole,M["id"]);Item->setData(Qt::UserRole+1,M["locator"]);}if(!error_.isEmpty())status_->setText(error_);}
bool ReadingNavigation::Flush() {
    saveTimer_.stop();if(path_.isEmpty())return true;save();
    return !readFailed_ && additions_.isEmpty() && removals_.isEmpty() && (!remember_ || restoring_ || locator_==saved_);
}
void ReadingNavigation::save() {
    if(path_.isEmpty() || readFailed_ || restoring_ || ((!remember_ || locator_==saved_) && additions_.isEmpty() && removals_.isEmpty()))return;
    QLockFile Lock(path_+".lock");if(!Lock.tryLock(0)){saveTimer_.start(1200);return;}
    try {auto Root=read(path_);auto Marks=Root["bookmarks"].toArray();for(qsizetype I=Marks.size();I>0;--I)if(removals_.contains(Marks[I-1].toObject()["id"].toString()))Marks.removeAt(I-1);
        for(const auto& Value:additions_) {if(removals_.contains(Value.toObject()["id"].toString()))continue;if(Marks.size()>=128)throw tr("책갈피는 128개까지 저장합니다.");Marks.append(Value);}
        Root["bookmarks"]=Marks;if(remember_ && cfi(locator_))Root["locator"]=locator_;
        const auto Bytes=QJsonDocument(Root).toJson();if(Bytes.size()>256*1024)throw tr("읽기 기록 크기 제한을 초과했습니다.");
        QSaveFile File(path_);File.setDirectWriteFallback(false);if(!File.open(QIODevice::WriteOnly)||File.write(Bytes)!=Bytes.size()||!File.commit())throw tr("읽기 기록을 저장하지 못했습니다.");
        saved_=locator_;bookmarks_=Marks;additions_={};removals_.clear();refresh();
    }catch(const QString& Error){error_=Error;status_->setText(Error);}
}
}
