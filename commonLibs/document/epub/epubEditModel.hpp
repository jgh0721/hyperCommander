#pragma once
#include "epubPackage.hpp"
#include <QMap>
#include <QList>
namespace hc::document::epub {
struct EditOutput {QByteArray bytes_;QString error_,location_;explicit operator bool()const{return error_.isEmpty() && !bytes_.isEmpty();}};
class EditModel final {
public:
    bool Load(QByteArray Bytes,const Cancellation& Cancel={});
    QJsonObject Publication()const{return publication_;}
    QString Error()const{return error_;}
    QByteArray Read(const QString& Path)const;
    QByteArray OriginalBytes()const{return original_;}
    QMap<QString,QByteArray> Changes()const{return replacements_;}
    bool IsEditable(const QString& Path)const{return media_.contains(Path);}
    QString Replace(const QString& Path,const QByteArray& Bytes);
    bool CanUndo()const{return position_>0;}
    bool CanRedo()const{return position_<history_.size();}
    void Undo();void Redo();
    QByteArray Fingerprint(const QMap<QString,QByteArray>& Draft={})const;
    EditOutput Export(const QMap<QString,QByteArray>& Draft={},const Cancellation& Cancel={})const;
private:
    void assign(const QString& Path,const QByteArray& Bytes);
    QByteArray original_;
    QJsonObject publication_;QString error_;QMap<QString,QString> media_;
    QMap<QString,QByteArray> replacements_;
    struct Change{QString path_;QByteArray before_,after_;};QList<Change> history_;qsizetype position_=0;
};
}
