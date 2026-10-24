#pragma once
#include "commonLibs/document/epub/epubEditModel.hpp"
#include "commonLibs/storage/storage.hpp"
#include <QWidget>
#include <QFutureWatcher>
#include <atomic>
class QComboBox;class QPlainTextEdit;class QLabel;
namespace hc::document {
class EpubSourceEditor final:public QWidget {
    Q_OBJECT
public:
    EpubSourceEditor(QByteArray Bytes,const QString& Path,QWidget* Parent=nullptr);
    ~EpubSourceEditor()override;
    QJsonObject Snapshot()const;
    QJsonObject Execute(const QJsonObject& Request);
    QByteArray Token()const;
    storage::Result<QByteArray> Export(const QString& Destination)const;
    bool CanUndo()const;bool CanRedo()const;void Undo();void Redo();
    bool IsReady()const{return ready_;}
    bool IsValidated()const{return ready_ && !validation_.isRunning() && validatedToken_==Token() && !validatedBytes_.isEmpty();}
    void Commit(const QString& Path,const QByteArray& Bytes);
    void RequestSave(){validate(true);}
signals:void Changed();void StateChanged();void SaveRequested();
private:
    void load(QByteArray Bytes);void validate(bool SaveAfter);void select();bool flush();void importImage();
    QMap<QString,QByteArray> draft()const;
    epub::EditModel model_;QFutureWatcher<epub::EditModel> worker_;
    QFutureWatcher<epub::EditOutput> validation_;QByteArray validationToken_,validatedToken_,validatedBytes_;bool saveAfter_=false;
    std::shared_ptr<std::atomic_bool> cancel_;
    QComboBox* resources_=nullptr;QPlainTextEdit* text_=nullptr;QLabel *image_=nullptr,*status_=nullptr;
    QString current_,error_;QStringList protectedPaths_;QList<QByteArray> identities_;QByteArray baseToken_,reference_;
    bool ready_=false,updating_=false;
};
}
