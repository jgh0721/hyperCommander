#pragma once
#include <QAbstractItemModel>
#include <QJsonObject>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <functional>
namespace hc::document {
// Book structure for the EPUB editor: reading order, styles, images, fonts, package and other resources.
class EpubResourceModel final:public QAbstractItemModel {
    Q_OBJECT
public:
    enum Role {PathRole=Qt::UserRole+1,MediaRole,TitleRole,OrderRole,GroupRole,EditableRole,ModifiedRole,ProblemsRole,SpineRole};
    struct Entry {QString path_,media_,title_;int order_=-1;bool editable_=false;};
    struct Group {QString id_,label_;QList<Entry> entries_;};
    explicit EpubResourceModel(QObject* Parent=nullptr):QAbstractItemModel(Parent){}
    void Reset(const QJsonObject& Publication,const std::function<bool(const QString&)>& Editable);
    void SetDecorations(std::function<bool(const QString&)> Modified,std::function<int(const QString&)> Problems){modified_=std::move(Modified);problems_=std::move(Problems);}
    void Refresh();
    QModelIndex IndexOf(const QString& Path)const;
    int ResourceCount()const;
    QString Title(const QString& Path)const;
    QModelIndex index(int Row,int Column,const QModelIndex& Parent={})const override;
    QModelIndex parent(const QModelIndex& Child)const override;
    int rowCount(const QModelIndex& Parent={})const override;
    int columnCount(const QModelIndex& Parent={})const override{Q_UNUSED(Parent);return 1;}
    QVariant data(const QModelIndex& Index,int Role=Qt::DisplayRole)const override;
    Qt::ItemFlags flags(const QModelIndex& Index)const override;
private:
    QList<Group> groups_;std::function<bool(const QString&)> modified_;std::function<int(const QString&)> problems_;
};
class EpubResourceFilter final:public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit EpubResourceFilter(QObject* Parent=nullptr):QSortFilterProxyModel(Parent){setRecursiveFilteringEnabled(true);setFilterCaseSensitivity(Qt::CaseInsensitive);}
    void SetText(const QString& Text){beginFilterChange();text_=Text.trimmed();endFilterChange(QSortFilterProxyModel::Direction::Rows);}
    void SetChangedOnly(bool Value){beginFilterChange();changedOnly_=Value;endFilterChange(QSortFilterProxyModel::Direction::Rows);}
    bool ChangedOnly()const{return changedOnly_;}
protected:
    bool filterAcceptsRow(int Row,const QModelIndex& Parent)const override;
private:
    QString text_;bool changedOnly_=false;
};
// Reading-order entries take two lines: the chapter title, then the file name.
class EpubResourceDelegate final:public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* Painter,const QStyleOptionViewItem& Option,const QModelIndex& Index)const override;
    QSize sizeHint(const QStyleOptionViewItem& Option,const QModelIndex& Index)const override;
};
}
