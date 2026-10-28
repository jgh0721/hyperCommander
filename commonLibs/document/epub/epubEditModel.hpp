#pragma once
#include "epubPackage.hpp"
#include <QMap>
#include <QList>
#include <memory>
namespace hc::document::epub {
class EditModel;
struct EditOutput {QByteArray bytes_;QString error_,location_;std::shared_ptr<const EditModel> prepared_;explicit operator bool()const{return error_.isEmpty() && !bytes_.isEmpty();}};
class EditModel final {
public:
    bool Load(QByteArray Bytes,const Cancellation& Cancel={});
    QJsonObject Publication()const{return publication_;}
    QString Error()const{return error_;}
    QByteArray Read(const QString& Path)const;
    QByteArray OriginalBytes()const{return original_;}
    QByteArray StructuralBytes()const{return publication_["sourceSha256"].toString()==originalHash_?QByteArray{}:edition_;}
    QString RestoreStructuralBytes(const QByteArray& Bytes);
    QString Structure(const QJsonObject& Request,const Cancellation& Cancel={});
    QJsonObject Metadata()const;
    QJsonObject InspectImage(const QString& Path,const Cancellation& Cancel={})const;
    QMap<QString,QByteArray> Changes()const{return replacements_;}
    bool IsEditable(const QString& Path)const{return media_.contains(Path);}
    QString Replace(const QString& Path,const QByteArray& Bytes);
    bool CanUndo()const{return position_>0;}
    bool CanRedo()const{return position_<history_.size();}
    void Undo();void Redo();
    QByteArray Fingerprint(const QMap<QString,QByteArray>& Draft={})const;
    EditOutput Export(const QMap<QString,QByteArray>& Draft={},const Cancellation& Cancel={})const;
private:
    void accept(QByteArray Bytes,const QJsonObject& Publication);
    QString modifyProperties(const QJsonObject& Request,const Cancellation& Cancel);
    void assign(const QString& Path,const QByteArray& Bytes);
    struct State {QByteArray edition_;QJsonObject publication_;QMap<QString,QString> media_;QMap<QString,QByteArray> changes_;};
    State state()const;void restore(const State& Value);void record(State Before);
    QByteArray original_,edition_;QString originalHash_;
    QJsonObject publication_;QString error_;QMap<QString,QString> media_;
    QMap<QString,QByteArray> replacements_;
    mutable QMap<QString,QByteArray> fingerprintChanges_;
    mutable QString fingerprintEdition_;mutable QByteArray fingerprint_;
    struct Change{State before_,after_;};QList<Change> history_;qsizetype position_=0;
};
}
