#pragma once

#include "cmnConcurrent.hpp"


class CShortcutMgr : public QObject
{
    Q_OBJECT
public:

    void Refresh();

    void SetShortcut( const QKeySequence& KeySequence, const QString& Cmd );
    QString GetCMDFromShortcut( const QKeySequence& KeySequence );

private:

    // first = QKeySequence String
    QHash< QString, QString > mapKeyToCMD;

};

using TyStShortcutMgr = nsCmn::nsConcurrent::TSingleton< CShortcutMgr >;
