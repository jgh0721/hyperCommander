#pragma once
#include <QWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <functional>
class QLineEdit;class QListWidget;class QLabel;class QTreeWidget;
namespace hc::epub::poc {
class ReadingNavigation final : public QWidget {
public:
    using Dispatch=std::function<void(const QJsonObject&)>;
    ReadingNavigation(const QString& Settings,const QString& Book,Dispatch Send,QWidget* Parent=nullptr);
    ~ReadingNavigation() override;
    bool Execute(const QString& Action,const QJsonValue& Value={});
    void Poll(const QJsonObject& State);
    QJsonObject Snapshot() const;
    void SetPublication(const QJsonObject& Publication);
    void ConfigureProfile(const QJsonObject& Profile);
private:
    void save();void refresh();void send(const QString& Action,const QJsonValue& Value);
    QString path_,error_,locator_,saved_,restore_;
    QJsonArray bookmarks_,additions_;QStringList removals_;
    Dispatch send_;QTimer saveTimer_;
    QLineEdit* query_=nullptr;QListWidget *results_=nullptr,*marks_=nullptr;QLabel* status_=nullptr;
    qint64 serial_=0,seen_=0,restoreSerial_=0;bool started_=false,restoring_=false,readFailed_=false;
    QTreeWidget* toc_=nullptr;
    QJsonArray navigation_;
    bool resume_=true,remember_=true;
    qint64 historyEpoch_=-1;
};
}
