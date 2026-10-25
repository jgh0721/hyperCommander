#include "stdafx.h"
#include "readingSettings.hpp"
#include "readingProfiles.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>
namespace hc::epub::poc {
void ReadingSettings::addProfiles(QTabWidget* Tabs) {
    auto* Panel=new QWidget(Tabs);Panel->setObjectName("epubProfilesTab");auto* Layout=new QVBoxLayout(Panel);auto* Form=new QFormLayout;Layout->addLayout(Form);Tabs->addTab(Panel,tr("프로필"));
    namedProfiles_=new QComboBox(Panel);namedProfiles_->setObjectName("epubNamedProfiles");namedProfiles_->setAccessibleName(tr("저장한 읽기 프로필"));Form->addRow(tr("저장한 프로필"),namedProfiles_);
    profileName_=new QLineEdit(Panel);profileName_->setObjectName("epubProfileName");profileName_->setMaxLength(80);Form->addRow(tr("저장할 이름"),profileName_);
    profileOverwrite_=new QCheckBox(tr("같은 이름이 있으면 덮어쓰기"),Panel);Form->addRow(profileOverwrite_);
    auto* Use=new QPushButton(tr("선택 프로필 미리보기"),Panel);Use->setObjectName("epubProfileUse");Layout->addWidget(Use);
    auto* Save=new QPushButton(tr("현재 서식을 이름으로 저장"),Panel);Save->setObjectName("epubProfileSave");Layout->addWidget(Save);
    auto* Remove=new QPushButton(tr("선택 프로필 삭제"),Panel);Remove->setObjectName("epubProfileRemove");Layout->addWidget(Remove);
    auto* Import=new QPushButton(tr("프로필 가져오기…"),Panel);Import->setObjectName("epubProfileImport");Layout->addWidget(Import);
    auto* Export=new QPushButton(tr("현재 서식 내보내기…"),Panel);Export->setObjectName("epubProfileExport");Layout->addWidget(Export);
    auto* Reload=new QPushButton(tr("프로필 목록 갱신"),Panel);Reload->setObjectName("epubProfileReload");Layout->addWidget(Reload);
    auto* Note=new QLabel(tr("미리보기 후 적용하면 현재 범위에 저장합니다. 프로필에는 글꼴·간격·배경·CSS와 관리되는 배경 이미지를 담습니다. 읽던 위치·책갈피·메모·개인정보 설정은 포함하지 않습니다."),Panel);Note->setWordWrap(true);Layout->addWidget(Note);Layout->addStretch();
    connect(Tabs,&QTabWidget::currentChanged,this,[this,Tabs,Panel]{if(Tabs->currentWidget()==Panel && !profilesLoaded_ && !profilesBusy_)profilesOperation("list",{});});
    connect(Use,&QPushButton::clicked,this,[this]{if(!IsBusy() && namedProfiles_->currentIndex()>=0)useNamedProfile(namedProfiles_->currentData().toJsonObject());});
    connect(Save,&QPushButton::clicked,this,[this]{profilesOperation("save",profileName_->text(),profileOverwrite_->isChecked());});
    connect(Remove,&QPushButton::clicked,this,[this]{if(namedProfiles_->currentIndex()>=0)profilesOperation("remove",namedProfiles_->currentText());});
    connect(Reload,&QPushButton::clicked,this,[this]{profilesOperation("list",{});});
    connect(Import,&QPushButton::clicked,this,[this]{profileTransfer(true);});connect(Export,&QPushButton::clicked,this,[this]{profileTransfer(false);});
    connect(&profilesWorker_,&QFutureWatcher<ReadingProfileResult>::finished,this,[this]{
        const auto Result=profilesWorker_.result();profilesBusy_=false;const auto Operation=profileOperation_;
        if(!Result){refresh();status_->setText(Result.error);return;}
        if(Operation!="export") {
            profilesLoaded_=true;const auto Current=Result.name.isEmpty()?namedProfiles_->currentText():Result.name;const QSignalBlocker Block(namedProfiles_);namedProfiles_->clear();
            for(const auto& Value:Result.profiles){const auto Row=Value.toObject();namedProfiles_->addItem(Row["name"].toString(),QVariant::fromValue(Row["profile"].toObject()));}
            const auto Index=namedProfiles_->findText(Current);if(Index>=0)namedProfiles_->setCurrentIndex(Index);
        }
        profileOverwrite_->setChecked(false);refresh();
        if(Operation=="import"){profileName_->setText(Result.name);useNamedProfile(Result.profile);status_->setText(tr("프로필을 가져왔습니다. 미리보기 후 적용하세요."));}
        else status_->setText(Operation=="export"?tr("현재 서식을 JSON으로 내보냈습니다."):Operation=="save"?tr("이름 있는 프로필을 저장했습니다."):Operation=="remove"?tr("선택 프로필을 삭제했습니다."):tr("프로필 목록을 불러왔습니다."));
    });
}
void ReadingSettings::useNamedProfile(const QJsonObject& Profile) {
    if(IsBusy() || !ReadingStore::Valid(Profile))return;
    for(auto I=Profile.begin();I!=Profile.end();++I)if(I.key()!="resume" && I.key()!="privateReading" && I.key()!="historyEpoch")draft_[I.key()]=I.value();
    validatedCss_.clear();cssError_.clear();refresh();emit ProfileChanged(this->Profile());
}
void ReadingSettings::profilesOperation(const QString& Operation,const QString& Argument,bool Overwrite) {
    if(profilesBusy_ || (Operation!="list" && (busy_ || loadFailed_)))return;
    if(path_.isEmpty()){status_->setText(tr("설정 경로가 없는 모드에서는 프로필을 보관하지 않습니다."));return;}
    if((Operation=="save" || Operation=="export") && !CssValid()){status_->setText(tr("CSS 검사를 통과한 서식만 보관하거나 내보낼 수 있습니다."));return;}
    profilesBusy_=true;profileOperation_=Operation;const auto Value=Profile();const auto Name=profileName_->text().trimmed().isEmpty()?tr("읽기 프로필"):profileName_->text();refresh();
    profilesWorker_.setFuture(QtConcurrent::run([Path=path_,Operation,Argument,Overwrite,Value,Name]{
        if(Operation=="list")return ReadingProfiles::List(Path);
        if(Operation=="save")return ReadingProfiles::Save(Path,Argument,Value,Overwrite);
        if(Operation=="remove")return ReadingProfiles::Remove(Path,Argument);
        if(Operation=="export")return ReadingProfiles::Export(Path,Argument,Name,Value);
        if(Operation=="import")return ReadingProfiles::Import(Path,Argument,Overwrite);
        return ReadingProfileResult{{},{},{},QStringLiteral("지원하지 않는 프로필 작업입니다.")};
    }));
}
void ReadingSettings::profileTransfer(bool Import) {
    if(IsBusy() || path_.isEmpty())return;
    auto* Dialog=new QFileDialog(this,Import?tr("EPUB 읽기 프로필 가져오기"):tr("EPUB 읽기 프로필 내보내기"));Dialog->setAttribute(Qt::WA_DeleteOnClose);Dialog->setNameFilter(tr("EPUB 읽기 프로필 (*.json)"));Dialog->setDefaultSuffix("json");
    Dialog->setAcceptMode(Import?QFileDialog::AcceptOpen:QFileDialog::AcceptSave);Dialog->setFileMode(Import?QFileDialog::ExistingFile:QFileDialog::AnyFile);
    const bool Overwrite=profileOverwrite_->isChecked();connect(Dialog,&QFileDialog::fileSelected,this,[this,Import,Overwrite](const QString& Path){if(!Path.isEmpty())profilesOperation(Import?"import":"export",Path,Overwrite);});Dialog->open();
}
}
