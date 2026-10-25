#pragma once
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>
#include <functional>
namespace hc::document::epub {
struct DraftSnapshot {
    QString sourcePath_, currentResource_, locator_;
    QByteArray original_, currentDraft_,structuralBytes_;
    QMap<QString,QByteArray> changes_;
    int cursor_=0,scroll_=0,readingChapter_=-1;
    bool hasCurrentDraft_=false;
};
struct DraftInfo {
    QString path_,sourcePath_,error_;
    QByteArray sourceHash_;
    qint64 utcMs_=0;
};
struct DraftRead { DraftSnapshot snapshot_;QString error_;bool sourceChanged_=false;explicit operator bool()const{return error_.isEmpty();} };
struct DraftWrite { QString path_,error_;explicit operator bool()const{return error_.isEmpty();} };
class DraftStore final {
public:
    static DraftWrite Write(const QString& Root,const QString& Session,const DraftSnapshot& Snapshot,const std::function<bool()>& Cancelled={});
    static DraftRead Read(const QString& Root,const QString& Path);
    static QList<DraftInfo> List(const QString& Root,const QString& SourcePath={},const QByteArray& SourceHash={});
    static bool Remove(const QString& Root,const QString& Path);
};
}
