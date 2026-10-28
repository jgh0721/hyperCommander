#pragma once
#include <QWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <functional>
#include <QHash>
class QLineEdit;class QListWidget;class QLabel;class QTreeWidget;class QTabWidget;class QComboBox;class QPlainTextEdit;
namespace hc::epub::poc {
class ReadingNavigation final : public QWidget {
public:
    using Dispatch=std::function<void(const QJsonObject&)>;
    ReadingNavigation(const QString& Settings,const QString& Book,Dispatch Send,QWidget* Parent=nullptr);
    ~ReadingNavigation() override;
    bool Execute(const QString& Action,const QJsonValue& Value={});
    void Poll(const QJsonObject& State);
    QJsonObject Snapshot() const;
    bool Flush();
    void SetPublication(const QJsonObject& Publication);
    void ConfigureProfile(const QJsonObject& Profile);
private:
    void addAnnotations(QTabWidget* Tabs);void refreshAnnotations();void syncAnnotations();
    bool annotationAction(const QString& Action,const QJsonValue& Value);bool putAnnotation(QJsonObject Row);bool annotationFits(const QJsonArray& Rows) const;
    void pollAnnotations(const QJsonObject& State);void annotationFile(bool Import);
    void beginAnnotationTransfer(const QString& Source,const QJsonArray& Rows);void nextAnnotationTransfer();
    void reloadAnnotations();
    void save();void refresh();void send(const QString& Action,const QJsonValue& Value);
    QString path_,error_,locator_,saved_,restore_,book_,bookTitle_,annotationMode_,transferSource_;
    QJsonArray annotations_,annotationAdditions_,transferPending_,transferOutput_;QStringList annotationRemovals_;
    QHash<QString,QJsonObject> annotationBases_;QJsonObject annotationDraft_;
    qint64 annotationPending_=-1;bool transient_=false,annotationBusy_=false,annotationsStarted_=false,annotationsSynced_=false,annotationNeedsSync_=false;
    QWidget* annotationPanel_=nullptr;QListWidget* annotationList_=nullptr;QComboBox* annotationColor_=nullptr;QPlainTextEdit* annotationNote_=nullptr;QLabel *annotationQuote_=nullptr,*annotationStatus_=nullptr;
    QJsonArray bookmarks_,additions_;QStringList removals_;
    Dispatch send_;QTimer saveTimer_;
    QLineEdit* query_=nullptr;QListWidget *results_=nullptr,*marks_=nullptr;QLabel* status_=nullptr;
    qint64 serial_=0,seen_=0,restoreSerial_=0,searchSeen_=0;bool started_=false,restoring_=false,readFailed_=false;
    QTreeWidget* toc_=nullptr;
    QJsonArray navigation_;
    bool resume_=true,remember_=true;
    qint64 historyEpoch_=-1;
    int currentToc_=-1,spineCount_=0;
};
}
