#include "stdafx.h"
#include "epubResourceTree.hpp"
#include <QApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QPainter>
#include <QSet>
#include <algorithm>
namespace hc::document {
namespace {
QString groupOf(const QString& Path,const QString& Media,const QString& Rootfile,bool Spine) {
    if(Spine)return "spine";if(Path==Rootfile || Media=="application/x-dtbncx+xml" || Media=="application/xhtml+xml")return "package";
    if(Media=="text/css")return "style";if(Media.startsWith("image/"))return "image";
    if(Media.startsWith("font/") || Media.contains("font") || Media=="application/vnd.ms-opentype")return "font";return "other";
}
}
void EpubResourceModel::Reset(const QJsonObject& Publication,const std::function<bool(const QString&)>& Editable) {
    beginResetModel();groups_={{"spine",tr("읽기 순서")},{"style",tr("스타일")},{"image",tr("이미지")},{"font",tr("글꼴")},{"package",tr("패키지")},{"other",tr("기타")}};
    const auto Rootfile=Publication["rootfile"].toString();QMap<QString,QString> Titles;
    // The first table-of-contents entry of a file names its chapter; fragments point inside it.
    for(const auto& Value:Publication["navigation"].toArray()){const auto Row=Value.toObject();const auto Path=Row["path"].toString().section('#',0,0);const auto Label=Row["label"].toString().trimmed();if(!Path.isEmpty() && !Label.isEmpty() && !Titles.contains(Path))Titles[Path]=Label;}
    QMap<QString,int> Order;const auto Spine=Publication["spine"].toArray();for(int I=0;I<Spine.size();++I){const auto Path=Spine[I].toObject()["path"].toString();if(!Order.contains(Path))Order[Path]=I;}
    QSet<QString> Seen;QList<Entry> Spined(Order.size());QList<bool> Filled(Order.size(),false);
    auto Resources=Publication["resources"].toArray();bool HasRoot=false;for(const auto& Value:Resources)HasRoot|=Value.toObject()["path"]==Rootfile;
    if(!HasRoot && !Rootfile.isEmpty())Resources.prepend(QJsonObject{{"path",Rootfile},{"mediaType","application/oebps-package+xml"}});
    for(const auto& Value:Resources) {
        const auto Row=Value.toObject();const auto Path=Row["path"].toString();if(Path.isEmpty() || Seen.contains(Path))continue;Seen.insert(Path);
        Entry Item{Path,Row["mediaType"].toString(),Titles.value(Path),Order.value(Path,-1),Editable(Path)};
        if(Item.order_>=0 && Item.order_<Spined.size()){Spined[Item.order_]=Item;Filled[Item.order_]=true;continue;}
        const auto Group=groupOf(Path,Item.media_,Rootfile,false);for(auto& Target:groups_)if(Target.id_==Group){Target.entries_.append(Item);break;}
    }
    for(int I=0;I<Spined.size();++I)if(Filled[I])groups_[0].entries_.append(Spined[I]);
    for(auto& Group:groups_)if(Group.id_!="spine")std::sort(Group.entries_.begin(),Group.entries_.end(),[&](const Entry& A,const Entry& B){if((A.path_==Rootfile)!=(B.path_==Rootfile))return A.path_==Rootfile;return A.path_.compare(B.path_,Qt::CaseInsensitive)<0;});
    groups_.removeIf([](const Group& Value){return Value.entries_.isEmpty() && Value.id_!="spine";});
    endResetModel();
}
void EpubResourceModel::Refresh(){if(rowCount()>0){emit dataChanged(index(0,0),index(rowCount()-1,0));for(int I=0;I<rowCount();++I)if(rowCount(index(I,0))>0)emit dataChanged(index(0,0,index(I,0)),index(rowCount(index(I,0))-1,0,index(I,0)));}}
QModelIndex EpubResourceModel::IndexOf(const QString& Path)const{for(int G=0;G<groups_.size();++G)for(int I=0;I<groups_[G].entries_.size();++I)if(groups_[G].entries_[I].path_==Path)return createIndex(I,0,quintptr(G+1));return {};}
int EpubResourceModel::ResourceCount()const{int Count=0;for(const auto& Group:groups_)Count+=Group.entries_.size();return Count;}
QString EpubResourceModel::Title(const QString& Path)const{const auto Index=IndexOf(Path);return Index.isValid()?Index.data(TitleRole).toString():QString{};}
QModelIndex EpubResourceModel::index(int Row,int Column,const QModelIndex& Parent)const {
    if(Column!=0 || Row<0)return {};
    if(!Parent.isValid())return Row<groups_.size()?createIndex(Row,0,quintptr(0)):QModelIndex{};
    if(Parent.internalId()!=0 || Parent.row()>=groups_.size() || Row>=groups_[Parent.row()].entries_.size())return {};
    return createIndex(Row,0,quintptr(Parent.row()+1));
}
QModelIndex EpubResourceModel::parent(const QModelIndex& Child)const{if(!Child.isValid() || Child.internalId()==0)return {};return createIndex(int(Child.internalId()-1),0,quintptr(0));}
int EpubResourceModel::rowCount(const QModelIndex& Parent)const{if(!Parent.isValid())return groups_.size();if(Parent.internalId()!=0 || Parent.row()>=groups_.size())return 0;return groups_[Parent.row()].entries_.size();}
QVariant EpubResourceModel::data(const QModelIndex& Index,int Role)const {
    if(!Index.isValid())return {};
    if(Index.internalId()==0) {
        const auto& Group=groups_[Index.row()];
        if(Role==Qt::DisplayRole)return Group.label_;if(Role==GroupRole)return Group.id_;if(Role==Qt::AccessibleTextRole)return tr("%1, %2개").arg(Group.label_).arg(Group.entries_.size());
        if(Role==ProblemsRole){int Count=0;for(const auto& Item:Group.entries_)Count+=problems_?problems_(Item.path_):0;return Count;}return {};
    }
    const auto& Group=groups_[int(Index.internalId()-1)];const auto& Item=Group.entries_[Index.row()];
    switch(Role) {
    case Qt::DisplayRole:return QFileInfo(Item.path_).fileName();
    case Qt::ToolTipRole:return Item.title_.isEmpty()?Item.path_:Item.title_+"\n"+Item.path_;
    case Qt::AccessibleTextRole:return Group.id_=="spine"?tr("%1. %2, %3").arg(Item.order_+1).arg(Item.title_.isEmpty()?tr("<이름없음>"):Item.title_,QFileInfo(Item.path_).fileName()):Item.path_;
    case PathRole:return Item.path_;case MediaRole:return Item.media_;case TitleRole:return Item.title_;case OrderRole:return Item.order_;case GroupRole:return Group.id_;
    case EditableRole:return Item.editable_;case SpineRole:return Group.id_=="spine";
    case ModifiedRole:return modified_ && modified_(Item.path_);case ProblemsRole:return problems_?problems_(Item.path_):0;
    default:return {};
    }
}
Qt::ItemFlags EpubResourceModel::flags(const QModelIndex& Index)const{if(!Index.isValid())return {};if(Index.internalId()==0)return Qt::ItemIsEnabled;return Qt::ItemIsEnabled|Qt::ItemIsSelectable;}
bool EpubResourceFilter::filterAcceptsRow(int Row,const QModelIndex& Parent)const {
    const auto Index=sourceModel()->index(Row,0,Parent);if(!Parent.isValid())return text_.isEmpty() && !changedOnly_;
    if(changedOnly_ && !Index.data(EpubResourceModel::ModifiedRole).toBool() && Index.data(EpubResourceModel::ProblemsRole).toInt()==0)return false;
    return text_.isEmpty() || Index.data(EpubResourceModel::PathRole).toString().contains(text_,Qt::CaseInsensitive) || Index.data(EpubResourceModel::TitleRole).toString().contains(text_,Qt::CaseInsensitive);
}
QSize EpubResourceDelegate::sizeHint(const QStyleOptionViewItem& Option,const QModelIndex& Index)const {
    const int Line=Option.fontMetrics.height();const bool Two=Index.data(EpubResourceModel::SpineRole).toBool();
    return {Option.rect.width(),Two?Line*2+8:Line+8};
}
void EpubResourceDelegate::paint(QPainter* Painter,const QStyleOptionViewItem& Option,const QModelIndex& Index)const {
    QStyleOptionViewItem Item=Option;initStyleOption(&Item,Index);const QString Display=Item.text;Item.text.clear();Item.icon={};
    auto* Style=Option.widget?Option.widget->style():QApplication::style();Style->drawControl(QStyle::CE_ItemViewItem,&Item,Painter,Option.widget);
    const bool Selected=Option.state & QStyle::State_Selected;const auto Group=Index.data(EpubResourceModel::GroupRole).toString();
    const auto Text=Option.palette.color(Selected?QPalette::HighlightedText:QPalette::Text);auto Secondary=Text;Secondary.setAlphaF(0.68);
    const bool Disabled=!Index.parent().isValid()?false:!Index.data(EpubResourceModel::EditableRole).toBool();
    auto Rect=Option.rect.adjusted(4,0,-6,0);Painter->save();Painter->setClipRect(Option.rect);
    // Trailing decorations first so the names elide against them.
    const int Problems=Index.data(EpubResourceModel::ProblemsRole).toInt();
    if(Problems>0 && Index.parent().isValid()){const auto Badge=QString("⚠ %1").arg(Problems);const int Width=Option.fontMetrics.horizontalAdvance(Badge)+10;QRect Box(Rect.right()-Width,Rect.center().y()-Option.fontMetrics.height()/2,Width,Option.fontMetrics.height());Painter->setPen(Qt::NoPen);Painter->setBrush(QColor(0xfb,0xe3,0xc2));Painter->drawRoundedRect(Box,Box.height()/2.0,Box.height()/2.0);Painter->setPen(QColor(0x7a,0x3a,0x05));Painter->drawText(Box,Qt::AlignCenter,Badge);Rect.setRight(Box.left()-6);}
    if(Index.data(EpubResourceModel::ModifiedRole).toBool()){const int Width=Option.fontMetrics.horizontalAdvance("●");Painter->setPen(QColor(0xc2,0x6b,0x0a));Painter->drawText(QRect(Rect.right()-Width,Rect.top(),Width,Rect.height()),Qt::AlignCenter,"●");Rect.setRight(Rect.right()-Width-6);}
    if(!Index.parent().isValid()) {
        auto Font=Option.font;Font.setWeight(QFont::DemiBold);Painter->setFont(Font);const auto Count=QString::number(Index.model()->rowCount(Index));
        Painter->setPen(Secondary);const int Width=Option.fontMetrics.horizontalAdvance(Count);Painter->drawText(QRect(Rect.right()-Width,Rect.top(),Width,Rect.height()),Qt::AlignVCenter|Qt::AlignRight,Count);Rect.setRight(Rect.right()-Width-6);
        Painter->setPen(Text);Painter->drawText(Rect,Qt::AlignVCenter|Qt::AlignLeft,QFontMetrics(Font).elidedText(Display,Qt::ElideRight,Rect.width()));Painter->restore();return;
    }
    Painter->setPen(Disabled?Secondary:Text);
    if(Group!="spine"){Painter->drawText(Rect,Qt::AlignVCenter|Qt::AlignLeft,Option.fontMetrics.elidedText(Display,Qt::ElideRight,Rect.width()));Painter->restore();return;}
    const auto Number=QString::number(Index.data(EpubResourceModel::OrderRole).toInt()+1);const int NumberWidth=Option.fontMetrics.horizontalAdvance("000");
    Painter->setPen(Secondary);Painter->drawText(QRect(Rect.left(),Rect.top(),NumberWidth,Rect.height()),Qt::AlignVCenter|Qt::AlignRight,Number);Rect.setLeft(Rect.left()+NumberWidth+8);
    const int Line=Option.fontMetrics.height();const QRect Top(Rect.left(),Rect.top()+4,Rect.width(),Line),Bottom(Rect.left(),Rect.top()+4+Line,Rect.width(),Line);
    const auto Title=Index.data(EpubResourceModel::TitleRole).toString();
    Painter->setPen(Title.isEmpty()?Secondary:Text);Painter->drawText(Top,Qt::AlignVCenter|Qt::AlignLeft,Option.fontMetrics.elidedText(Title.isEmpty()?tr("<이름없음>"):Title,Qt::ElideRight,Top.width()));
    auto Small=Option.font;Small.setPointSizeF(Small.pointSizeF()>0?Small.pointSizeF()*0.92:9);Painter->setFont(Small);Painter->setPen(Secondary);
    Painter->drawText(Bottom,Qt::AlignVCenter|Qt::AlignLeft,QFontMetrics(Small).elidedText(Display,Qt::ElideRight,Bottom.width()));Painter->restore();
}
}
