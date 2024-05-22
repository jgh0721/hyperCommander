#pragma once

#include "dlgMultiRename_Model.hpp"

#include "ui_dlgMultiRename.h"

class QMultiRenameUI : public QDialog
{
    Q_OBJECT
public:
    QMultiRenameUI( QWidget* Parent = nullptr, Qt::WindowFlags Flags = Qt::Dialog );

    Q_INVOKABLE void                    SetSourceFiles( const QVector< QString >& VecFiles );

protected slots:

    void                                on_edtFileNamePattern_textChanged( const QString& Text );
    void                                on_edtFileExtPattern_textChanged( const QString& Text );
    void                                on_edtSearchString_textChanged( const QString& Text );
    void                                on_edtReplaceString_textChanged( const QString& Text );

private:

    QMultiRenameModel*                  Model = nullptr;
    Qtitan::GridBandedTableView*        View = nullptr;
    Ui::dlgMultiRename                  Ui;
};