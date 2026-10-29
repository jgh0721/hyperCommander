#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "documentThemeAdapter.hpp"
#include "UniqueLibs/qtDialogFrame.hpp"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFontDatabase>
#include <QSignalBlocker>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QtConcurrentRun>

namespace hc::document {
namespace {
QFormLayout* propertyForm(QDialog& Dialog,const QString& Note) {
    auto* Outer=new QVBoxLayout(&Dialog);auto* Info=new QLabel(Note,&Dialog);Info->setTextFormat(Qt::PlainText);Info->setWordWrap(true);Outer->addWidget(Info);
    auto* Scroll=new QScrollArea(&Dialog);Scroll->setObjectName("epubPropertyScroll");Scroll->setWidgetResizable(true);Scroll->setMinimumSize(0,0);auto* Panel=new QWidget;Scroll->setWidget(Panel);Outer->addWidget(Scroll,1);auto* Form=new QFormLayout(Panel);Form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);Form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    Dialog.resize(QSize(640,560).boundedTo(Dialog.screen()->availableGeometry().size()-QSize(40,80)));return Form;
}
// The button box is the last row; the dialog then gets the product header and footer bands.
QDialogButtonBox* propertyButtons(QDialog& Dialog,const QString& Label,const QString& Summary,fm::ui::glyph::Glyph Glyph) {
    auto* Buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&Dialog);Buttons->button(QDialogButtonBox::Ok)->setText(Label);Dialog.layout()->addWidget(Buttons);QObject::connect(Buttons,&QDialogButtonBox::accepted,&Dialog,&QDialog::accept);QObject::connect(Buttons,&QDialogButtonBox::rejected,&Dialog,&QDialog::reject);hc::ui::ApplyDialogFrame(&Dialog,Buttons,{{},Summary,Glyph});return Buttons;
}
}
void EpubSourceEditor::editMetadata() {
    if(!ready_ || structuring_ || !flush())return;const auto Info=model_.Metadata();if(Info.contains("error")){report(tr("서지정보를 읽지 못했습니다: %1").arg(Info["error"].toString()));return;}
    const auto Values=Info["values"].toObject();QDialog Dialog(this);Dialog.setObjectName("epubMetadataDialog");Dialog.setWindowTitle(tr("EPUB 서지정보"));
    auto* Form=propertyForm(Dialog,tr("기본 서지정보의 첫 번째 값을 편집합니다. 추가 저자·언어와 확장 메타데이터, 기존 식별자는 보존합니다. 입력 한도를 넘는 기존 값은 보존하며 OPF 소스에서 편집합니다. 변경은 실행 취소할 수 있으며 다른 이름으로 저장합니다."));
    QMap<QString,QLineEdit*> Fields;const QStringList Keys{"title","creator","language","publisher"},Labels{tr("제목"),tr("저자"),tr("언어"),tr("출판사")};
    for(int I=0;I<Keys.size();++I){auto* Field=new QLineEdit(&Dialog);Field->setMaxLength(qMax(1024,int(Values[Keys[I]].toString().size())));Field->setText(Values[Keys[I]].toString());Field->setReadOnly(Values[Keys[I]].toString().size()>1024);Field->setObjectName("epubMetadata_"+Keys[I]);Fields[Keys[I]]=Field;Form->addRow(Labels[I],Field);}
    auto* Description=new QPlainTextEdit(Values["description"].toString(),&Dialog);Description->setObjectName("epubMetadata_description");DocumentThemeAdapter::Attach(Description);Description->setAccessibleName(tr("책 설명"));Description->setMinimumHeight(100);Form->addRow(tr("설명"),Description);
    auto* Identifier=new QLineEdit(Values["identifier"].toString(),&Dialog);Identifier->setReadOnly(true);Form->addRow(tr("식별자 · 유지"),Identifier);auto* Error=new QLabel(&Dialog);Error->setWordWrap(true);Error->setAccessibleName(tr("서지정보 입력 오류"));Form->addRow(Error);auto* Buttons=propertyButtons(Dialog,tr("변경 반영"),tr("제목·저자·언어·출판사·설명을 편집합니다."),fm::ui::glyph::Info);disconnect(Buttons,&QDialogButtonBox::accepted,&Dialog,&QDialog::accept);
    connect(Buttons,&QDialogButtonBox::accepted,&Dialog,[&]{if(Fields["title"]->text()!=Values["title"].toString() && Fields["title"]->text().trimmed().isEmpty()){Error->setText(tr("제목을 입력해 주세요."));Fields["title"]->setFocus();return;}if(Fields["language"]->text()!=Values["language"].toString() && !QRegularExpression("^[A-Za-z]{2,8}(?:-[A-Za-z0-9]{1,8})*$").match(Fields["language"]->text()).hasMatch()){Error->setText(tr("언어는 ko 또는 ko-KR 같은 형식으로 입력해 주세요."));Fields["language"]->setFocus();return;}if(Description->toPlainText()!=Values["description"].toString() && Description->toPlainText().size()>8192){Error->setText(tr("책 설명은 8,192자까지 입력할 수 있습니다."));Description->setFocus();return;}Dialog.accept();});
    if(Dialog.exec()!=QDialog::Accepted)return;QJsonObject Changed;for(auto I=Fields.begin();I!=Fields.end();++I)if(!I.value()->isReadOnly() && I.value()->text()!=Values[I.key()].toString())Changed[I.key()]=I.value()->text();if(Description->toPlainText()!=Values["description"].toString())Changed["description"]=Description->toPlainText();if(Changed.isEmpty())return;
    structure({{"operation","metadata"},{"values",Changed},{"expectedSha256",Info["sha256"]}});
}
void EpubSourceEditor::inspectImage(const QString& Path) {
    if(!ready_ || structuring_ || imageInspection_.isRunning() || !flush())return;
    if(!mediaOf(Path).startsWith("image/")){report(tr("사용 위치를 확인할 이미지 리소스를 선택해 주세요."));return;}
    imageInspectionToken_=Token();auto Model=model_;report(tr("이미지의 사용 위치와 속성을 확인하고 있습니다…"));
    const auto Cancel=cancel_;imageInspection_.setFuture(QtConcurrent::run([Model=std::move(Model),Path,Cancel]{return Model.InspectImage(Path,[Cancel]{return Cancel->load();});}));
}
void EpubSourceEditor::editImageProperties(const QJsonObject& Info) {
    if(Info.contains("error")){report(tr("이미지 사용 위치를 읽지 못했습니다: %1").arg(Info["error"].toString()));return;}
    const auto Rows=Info["occurrences"].toArray();if(Rows.isEmpty()){report(tr("본문과 CSS에서 이 이미지의 사용 위치를 찾지 못했습니다. 표지와 패키지 참조는 서지정보·표지 작업에서 확인할 수 있습니다."));return;}
    QDialog Dialog(this);Dialog.setObjectName("epubImagePropertiesDialog");Dialog.setWindowTitle(tr("이미지 사용 위치·속성"));auto* Form=propertyForm(Dialog,tr("같은 이미지가 여러 곳에 사용될 수 있습니다. 선택한 위치의 표시 너비·높이·대체 텍스트·캡션을 변경합니다. CSS 치수 규칙은 원문에서 확인합니다. 긴 대체 텍스트와 복잡한 캡션은 원문을 보존하며 소스에서 편집합니다. 이미지 파일 전체의 교체는 별도 명령을 사용합니다. 복잡한 캡션과 CSS 치수 규칙은 소스에서 확인합니다."));
    auto* Locations=new QComboBox(&Dialog);Locations->setObjectName("epubImageLocations");Locations->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);Locations->setMinimumContentsLength(12);for(int I=0;I<Rows.size();++I){const auto Row=Rows[I].toObject();Locations->addItem(tr("%1 · %2 · %3").arg(I+1).arg(Row["document"].toString(),Row["kind"].toString()));}Form->addRow(tr("사용 위치"),Locations);
    auto* Dimensions=new QLabel(&Dialog);Dimensions->setTextFormat(Qt::PlainText);Dimensions->setWordWrap(true);Form->addRow(tr("치수·스타일"),Dimensions);
    QMap<QString,QLineEdit*> Sizes;for(const auto& Key:QStringList{"width","height"}){auto* Field=new QLineEdit(&Dialog);Field->setObjectName(Key=="width"?"epubImageWidth":"epubImageHeight");Field->setPlaceholderText(tr("자동 · 속성 없음"));Field->setToolTip(tr("선택한 img의 너비·높이 속성입니다. 0~999999 정수 또는 빈 값을 사용합니다. CSS의 치수 규칙은 참조 원문에서 확인하세요."));Sizes[Key]=Field;Form->addRow(Key=="width"?tr("표시 너비 · px"):tr("표시 높이 · px"),Field);}
    auto* Alt=new QLineEdit(&Dialog);Alt->setObjectName("epubImageAlt");Alt->setMaxLength(2048);Form->addRow(tr("대체 텍스트 · 빈 값은 장식"),Alt);
    auto* Caption=new QPlainTextEdit(&Dialog);Caption->setObjectName("epubImageCaption");DocumentThemeAdapter::Attach(Caption);Caption->setAccessibleName(tr("선택 위치의 캡션"));Caption->setMinimumHeight(100);Form->addRow(tr("캡션"),Caption);
    auto* CssSources=new QComboBox(&Dialog);CssSources->setObjectName("epubImageCssSources");CssSources->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);CssSources->setMinimumContentsLength(12);Form->addRow(tr("참조·CSS 원문"),CssSources);
    auto* CssText=new QPlainTextEdit(&Dialog);CssText->setObjectName("epubImageCssSource");CssText->setAccessibleName(tr("이미지 치수 규칙의 원문"));CssText->setReadOnly(true);CssText->setTabChangesFocus(true);CssText->setLineWrapMode(QPlainTextEdit::NoWrap);CssText->setMinimumHeight(100);CssText->setMaximumHeight(150);CssText->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));DocumentThemeAdapter::Attach(CssText);Form->addRow(CssText);
    const auto LoadCss=[&]{CssText->setPlainText(QString::fromUtf8(model_.Read(CssSources->currentData().toString())));};connect(CssSources,&QComboBox::currentIndexChanged,&Dialog,LoadCss);
    auto* Explanation=new QLabel(&Dialog);Explanation->setWordWrap(true);Form->addRow(Explanation);auto* Buttons=propertyButtons(Dialog,tr("선택 위치에 반영"),tr("선택한 사용 위치의 크기·대체 텍스트·캡션을 변경합니다."),fm::ui::glyph::File);
    const auto Select=[&]{const auto Row=Rows[Locations->currentIndex()].toObject();for(auto I=Sizes.begin();I!=Sizes.end();++I){const auto Value=Row[I.key()].toString();I.value()->setMaxLength(qMax(6,int(Value.size())));I.value()->setText(Value);I.value()->setEnabled(Row["editable"].toBool() && (Value.isEmpty() || QRegularExpression("^[0-9]{1,6}$").match(Value).hasMatch()));}Alt->setText(Row["alt"].toString());Caption->setPlainText(Row["caption"].toString());Alt->setEnabled(Row["editable"].toBool() && Row["alt"].toString().size()<=2048);Caption->setEnabled(Row["editable"].toBool() && Row["captionEditable"].toBool());Buttons->button(QDialogButtonBox::Ok)->setEnabled(Row["editable"].toBool());Dimensions->setText(tr("원본 %1 × %2 · width=%3 · height=%4\n%5").arg(Info["width"].toString().left(64),Info["height"].toString().left(64),Row["width"].toString().left(64),Row["height"].toString().left(64),Row["style"].toString().left(2048)));{const QSignalBlocker Block(CssSources);CssSources->clear();CssSources->addItem(tr("참조 원문 · %1").arg(Row["document"].toString()),Row["document"]);const auto Linked=Row["stylesheets"].toArray();for(const auto& Value:model_.Publication()["resources"].toArray()){const auto Css=Value.toObject()["path"].toString();if(Value.toObject()["mediaType"]=="text/css" && model_.IsEditable(Css))CssSources->addItem((Linked.contains(Css)?tr("연결 CSS · "):tr("책 CSS · "))+Css,Css);}}LoadCss();Explanation->setText(!Row["editable"].toBool()?tr("이 참조는 CSS·SVG 또는 링크입니다. 해당 소스에서 편집하세요."):!Row["captionEditable"].toBool()?tr("이 위치의 복잡한 문단·캡션 구조를 보존합니다. 캡션은 XHTML 소스에서 편집하세요."):tr("현재 위치의 속성만 변경하며 다른 사용 위치는 유지합니다."));};
    disconnect(Buttons,&QDialogButtonBox::accepted,&Dialog,&QDialog::accept);connect(Buttons,&QDialogButtonBox::accepted,&Dialog,[&]{const auto Row=Rows[Locations->currentIndex()].toObject();for(auto I=Sizes.begin();I!=Sizes.end();++I)if(I.value()->isEnabled() && !I.value()->text().isEmpty() && !QRegularExpression("^[0-9]{1,6}$").match(I.value()->text()).hasMatch()){Explanation->setText(tr("너비·높이는 0~999999 정수 또는 빈 값으로 입력해 주세요. 기존의 복잡한 값은 소스에서 편집합니다."));I.value()->setFocus();return;}if(Row["captionEditable"].toBool() && Caption->toPlainText()!=Row["caption"].toString() && Caption->toPlainText().size()>4096){Explanation->setText(tr("캡션은 4,096자까지 입력할 수 있습니다. 입력 내용을 줄여 주세요."));Caption->setFocus();return;}Dialog.accept();});
    connect(Locations,&QComboBox::currentIndexChanged,&Dialog,Select);Select();if(Dialog.exec()!=QDialog::Accepted)return;
    const auto Row=Rows[Locations->currentIndex()].toObject();QJsonObject Request{{"operation","image-attributes"},{"resource",Info["resource"]},{"document",Row["document"]},{"elementPath",Row["elementPath"]},{"expectedSha256",Row["sha256"]}};bool Changed=false;
    for(auto I=Sizes.begin();I!=Sizes.end();++I)if(I.value()->isEnabled() && I.value()->text()!=Row[I.key()].toString()){Request[I.key()]=I.value()->text();Changed=true;}
    if(Alt->isEnabled() && (!Row["hasAlt"].toBool() || Row["alt"].toString()!=Alt->text())){Request["alt"]=Alt->text();Changed=true;}
    if(Row["captionEditable"].toBool() && Row["caption"].toString()!=Caption->toPlainText()){Request["caption"]=Caption->toPlainText();Changed=true;}if(Changed)structure(Request);
}
}
