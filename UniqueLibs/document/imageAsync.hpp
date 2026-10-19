#pragma once
#include <QObject>
#include <QTimer>
#include <QPointer>
namespace hc::document
{
    inline void DeleteCompletedImageWork(QObject* Work)
    {
        // The isolated viewer pumps events around pipe reads rather than running
        // app.exec(). Reclaim only this completed job on the next GUI turn;
        // flushing all DeferredDelete events would invalidate unrelated editors.
        // The owner cancels this callback before DLL unload. Delivering it to
        // the watcher itself would delete the receiver inside its own event.
        if(auto* Owner=Work->parent())QTimer::singleShot(0,Owner,[Completed=QPointer<QObject>(Work)]{delete Completed.data();});
        else Work->deleteLater();
    }
}
