#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include "UniqueLibs/qtMessageDialog.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTabWidget>
#include <QTreeWidget>
#include <QUrl>
#include <Scintilla.h>
namespace hc::document {
namespace {
ScintillaQtDirectBackend* direct(QTextView* View){return static_cast<ScintillaQtDirectBackend*>(View->Backend());}
QString escapeAttribute(QString Text){return Text.replace('&',"&amp;").replace('<',"&lt;").replace('>',"&gt;").replace('"',"&quot;");}
QString escapeText(QString Text){return Text.replace('&',"&amp;").replace('<',"&lt;").replace('>',"&gt;");}
// EPUB-internal paths are '/'-separated and relative to the container root.
QString relativePath(const QString& From,const QString& To) {
    auto Base=From.split('/');Base.removeLast();const auto Target=To.split('/');qsizetype Common=0;while(Common<Base.size() && Common<Target.size()-1 && Base[Common]==Target[Common])++Common;
    QStringList Parts;for(auto I=Common;I<Base.size();++I)Parts.append("..");for(auto I=Common;I<Target.size();++I)Parts.append(Target[I]);return Parts.join('/');
}
QString resolvePath(const QString& Base,const QString& Href) {
    const QUrl Url(Href,QUrl::TolerantMode);if(Href.isEmpty() || Href.startsWith('#') || !Url.scheme().isEmpty() || Href.startsWith('/'))return {};
    return QDir::cleanPath(QFileInfo(Base).path()+"/"+QUrl::fromPercentEncoding(Href.section('#',0,0).toUtf8()));
}
// The start tag around Position, or an empty range.
QPair<int,int> tagAround(const QString& Text,int Position) {
    const int Open=Text.lastIndexOf('<',qMax(0,Position-1));if(Open<0 || Text.lastIndexOf('>',qMax(0,Position-1))>Open)return {-1,-1};
    const int Close=Text.indexOf('>',Open);if(Close<0 || Close+1<Position)return {-1,-1};return {Open,Close+1};
}
}
QJsonObject EpubSourceEditor::quickFixAt(int Position)const {
    if(!sourceShown() || mediaOf(current_)!="application/xhtml+xml")return {};const auto Text=sourceText();const auto Tag=tagAround(Text,Position);if(Tag.first<0)return {};
    const auto Body=Text.mid(Tag.first,Tag.second-Tag.first);
    if(!QRegularExpression("^<img\\b",QRegularExpression::CaseInsensitiveOption).match(Body).hasMatch() || QRegularExpression("\\salt\\s*=",QRegularExpression::CaseInsensitiveOption).match(Body).hasMatch())return {};
    return {{"kind","img-alt"},{"label",tr("alt 속성 추가")},{"insert",Tag.first+4}};
}
void EpubSourceEditor::quickFix() {
    const auto Fix=quickFixAt(sourcePosition());if(Fix.isEmpty() || sourceReadOnly_)return;bool Ok=false;
    const auto Alt=hc::ui::InputDialog::getText(this,tr("빠른 수정: alt 속성 추가"),tr("대체 텍스트 · 장식 이미지는 빈 값"),QLineEdit::Normal,{},&Ok);if(!Ok)return;
    const auto Insert=QString(" alt=\"%1\"").arg(escapeAttribute(Alt.left(2048))).toUtf8();
    direct(text_)->Send(SCI_INSERTTEXT,quintptr(fromUtf16(Fix["insert"].toInt())),reinterpret_cast<qintptr>(Insert.constData()));report(tr("img 요소에 alt 속성을 추가했습니다. 다시 검사해 결과를 확인하세요."));
}
QString EpubSourceEditor::linkAt(int Position)const {
    if(!sourceShown())return {};const auto Text=sourceText();const auto Media=mediaOf(current_);QString Value;
    if(Media=="text/css") {
        const auto Line=Text.lastIndexOf('\n',qMax(0,Position-1))+1;auto Matches=QRegularExpression(R"(url\(\s*(['"]?)([^'")]+)\1\s*\))").globalMatch(Text.mid(Line,qMax(0,Text.indexOf('\n',Position)<0?int(Text.size())-Line:Text.indexOf('\n',Position)-Line)));
        while(Matches.hasNext()){const auto Match=Matches.next();if(Line+Match.capturedStart()<=Position && Position<=Line+Match.capturedEnd()){Value=Match.captured(2);break;}}
    } else {
        const auto Tag=tagAround(Text,Position);if(Tag.first<0)return {};auto Matches=QRegularExpression(R"(\b(?:href|src|xlink:href)\s*=\s*(["'])(.*?)\1)").globalMatch(Text.mid(Tag.first,Tag.second-Tag.first));
        QString First;while(Matches.hasNext()){const auto Match=Matches.next();if(First.isEmpty())First=Match.captured(2);if(Tag.first+Match.capturedStart()<=Position && Position<=Tag.first+Match.capturedEnd()){Value=Match.captured(2);break;}}if(Value.isEmpty())Value=First;
    }
    const auto Path=resolvePath(current_,Value.trimmed());if(Path.isEmpty())return {};
    for(const auto& Resource:model_.Publication()["resources"].toArray())if(Resource.toObject()["path"]==Path)return Path;return {};
}
bool EpubSourceEditor::openLink() {
    const auto Path=linkAt(sourcePosition());if(Path.isEmpty())return false;if(!model_.IsEditable(Path)){report(tr("%1은(는) 편집기에서 열 수 없는 리소스입니다.").arg(Path));return false;}
    // A fragment link lands on its id in the target document.
    const auto Text=sourceText();const auto Tag=tagAround(Text,sourcePosition());QString Fragment;const auto Match=QRegularExpression(R"(#([^"'\s>]+))").match(Text.mid(qMax(0,Tag.first),qMax(0,Tag.second-Tag.first)));if(Match.hasMatch())Fragment=Match.captured(1);
    if(!openResource(Path))return false;
    if(!Fragment.isEmpty() && sourceShown()){const auto Target=sourceText().indexOf(QRegularExpression(QString(R"(\bid\s*=\s*["']%1["'])").arg(QRegularExpression::escape(Fragment))));if(Target>=0)setSourceSelection(Target,Target);}
    focusSource();return true;
}
void EpubSourceEditor::wrapSelection(const QString& Tag) {
    if(!sourceShown() || sourceReadOnly_)return;const auto Selected=text_->Backend()->selectedText();if(Selected.isEmpty())return;
    if(Selected.contains('<') || Selected.contains('>')){report(tr("선택 영역이 태그 경계를 넘습니다. 텍스트만 선택해 주세요."));return;}
    QString Open,Close;bool Ok=true;
    if(Tag.isEmpty()) {
        const auto Name=hc::ui::InputDialog::getText(this,tr("사용자 지정 태그로 감싸기"),tr("XML 요소 이름 · 예: cite, abbr"),QLineEdit::Normal,{},&Ok).trimmed();if(!Ok)return;
        if(!QRegularExpression(R"(^[A-Za-z_][\w.\-]*(?::[A-Za-z_][\w.\-]*)?$)").match(Name).hasMatch()){report(tr("올바른 XML 요소 이름을 입력해 주세요."));return;}Open="<"+Name+">";Close="</"+Name+">";
    } else if(Tag=="span"){const auto Class=hc::ui::InputDialog::getText(this,tr("span으로 감싸기"),tr("class 값 · 빈 값은 class 없음"),QLineEdit::Normal,{},&Ok).trimmed();if(!Ok)return;Open=Class.isEmpty()?QStringLiteral("<span>"):QString("<span class=\"%1\">").arg(escapeAttribute(Class));Close="</span>";}
    else if(Tag=="a"){const auto Href=hc::ui::InputDialog::getText(this,tr("링크로 감싸기"),tr("href · 다른 리소스는 상대 경로, 같은 문서는 #id"),QLineEdit::Normal,{},&Ok).trimmed();if(!Ok || Href.isEmpty())return;Open=QString("<a href=\"%1\">").arg(escapeAttribute(Href));Close="</a>";}
    else if(Tag=="ruby"){const auto Reading=hc::ui::InputDialog::getText(this,tr("읽기 표기 추가"),tr("rt에 넣을 읽기"),QLineEdit::Normal,{},&Ok).trimmed();if(!Ok || Reading.isEmpty())return;Open="<ruby>";Close="<rt>"+escapeText(Reading)+"</rt></ruby>";}
    else {Open="<"+Tag+">";Close="</"+Tag+">";}
    const auto Bytes=(Open+Selected+Close).toUtf8();direct(text_)->Send(SCI_BEGINUNDOACTION);direct(text_)->Send(SCI_REPLACESEL,0,reinterpret_cast<qintptr>(Bytes.constData()));direct(text_)->Send(SCI_ENDUNDOACTION);
}
void EpubSourceEditor::findInResources(const QString& Requested) {
    auto Needle=Requested.trimmed();bool Ok=true;
    if(Needle.isEmpty())Needle=hc::ui::InputDialog::getText(this,tr("모든 리소스에서 찾기"),tr("찾을 문자열"),QLineEdit::Normal,{},&Ok).trimmed();if(!Ok || Needle.isEmpty())return;
    searchResults_->clear();int Count=0;
    for(const auto& Value:model_.Publication()["resources"].toArray()) {
        const auto Path=Value.toObject()["path"].toString();const auto Media=Value.toObject()["mediaType"].toString();if(!model_.IsEditable(Path) || Media.startsWith("image/") && Media!="image/svg+xml")continue;
        const auto Text=Path==current_ && sourceShown()?sourceText():QString::fromUtf8(model_.Read(Path));const auto Lines=Text.split('\n');
        for(int Line=0;Line<Lines.size() && Count<2000;++Line){int Column=Lines[Line].indexOf(Needle,0,Qt::CaseInsensitive);if(Column<0)continue;
            auto* Item=new QTreeWidgetItem(searchResults_,{Path,QString::number(Line+1),Lines[Line].trimmed().left(200)});Item->setData(0,Qt::UserRole,Path);Item->setData(1,Qt::UserRole,Line+1);Item->setData(2,Qt::UserRole,Column+1);++Count;}
    }
    problems_->setTabText(2,tr("검색 결과 %1").arg(Count));setPanelVisible("problems",true);problems_->setCurrentWidget(searchResults_);
    report(Count>=2000?tr("'%1' 결과가 많아 처음 2,000개만 표시합니다.").arg(Needle):tr("'%1' 결과 %2개").arg(Needle).arg(Count));
}
void EpubSourceEditor::findSelectionInSource(const QString& Text) {
    if(Text.trimmed().isEmpty() || !sourceShown())return;
    // Rendered text loses markup and entities; search for its longest plain line from the caret.
    QString Needle;for(const auto& Part:Text.split(QRegularExpression("\\s*\\n\\s*"),Qt::SkipEmptyParts))if(Part.size()>Needle.size())Needle=Part;Needle=Needle.left(200);
    text_->findText(Needle,false,false,false,true);focusSource();
}
void EpubSourceEditor::copyPath(const QString& Path,bool Relative){if(!Path.isEmpty())QApplication::clipboard()->setText(Relative && !current_.isEmpty()?relativePath(current_,Path):Path);}
void EpubSourceEditor::insertImageReference(const QString& Path) {
    if(!sourceShown() || sourceReadOnly_ || mediaOf(current_)!="application/xhtml+xml" || !mediaOf(Path).startsWith("image/"))return;bool Ok=false;
    const auto Alt=hc::ui::InputDialog::getText(this,tr("이미지 넣기"),tr("대체 텍스트 · 장식 이미지는 빈 값"),QLineEdit::Normal,{},&Ok);if(!Ok)return;
    insertSource(QString("<img src=\"%1\" alt=\"%2\"/>").arg(escapeAttribute(relativePath(current_,Path)),escapeAttribute(Alt.left(2048))));focusSource();
}
void EpubSourceEditor::exportResource(const QString& Path) {
    if(Path.isEmpty() || !flush())return;const auto Data=model_.Read(Path);if(Data.isEmpty()){report(tr("%1을(를) 읽지 못했습니다.").arg(Path));return;}
    const auto Destination=QFileDialog::getSaveFileName(this,tr("리소스를 파일로 내보내기"),QFileInfo(Path).fileName());if(Destination.isEmpty())return;
    QSaveFile File(Destination);if(!File.open(QIODevice::WriteOnly) || File.write(Data)!=Data.size() || !File.commit()){report(tr("파일을 저장하지 못했습니다: %1").arg(Destination));return;}
    report(tr("%1을(를) 내보냈습니다.").arg(QDir::toNativeSeparators(Destination)));
}
void EpubSourceEditor::duplicateResource(const QString& Path) {
    if(Path.isEmpty())return;const QFileInfo Info(Path);bool Ok=false;
    const auto Suggested=Info.path()+"/"+Info.completeBaseName()+"-copy"+(Info.suffix().isEmpty()?QString{}:"."+Info.suffix());
    const auto Target=hc::ui::InputDialog::getText(this,tr("리소스 복제"),tr("같은 폴더의 새 이름 · 복제한 장은 원본 바로 뒤 읽기 순서에 넣습니다."),QLineEdit::Normal,Suggested,&Ok).trimmed();
    if(Ok && !Target.isEmpty() && Target!=Path)structure({{"operation","duplicate"},{"resource",Path},{"destination",Target}});
}
void EpubSourceEditor::importResources() {
    const auto File=QFileDialog::getOpenFileName(this,tr("스타일 · 글꼴 가져오기"),{},tr("스타일 · 글꼴 (*.css *.ttf *.otf *.woff *.woff2)"));if(File.isEmpty())return;
    const auto Suffix=QFileInfo(File).suffix().toLower();const auto Media=Suffix=="css"?QStringLiteral("text/css"):"font/"+Suffix;
    const auto Root=QFileInfo(model_.Publication()["rootfile"].toString()).path();bool Ok=false;
    const auto Target=hc::ui::InputDialog::getText(this,tr("스타일 · 글꼴 가져오기"),tr("EPUB 내부 경로"),QLineEdit::Normal,QDir::cleanPath(Root+(Suffix=="css"?"/Styles/":"/Fonts/")+QFileInfo(File).fileName()),&Ok).trimmed();
    if(Ok && !Target.isEmpty())structure({{"operation","resource-add"},{"resource",Target},{"file",File},{"mediaType",Media}});
}
}
