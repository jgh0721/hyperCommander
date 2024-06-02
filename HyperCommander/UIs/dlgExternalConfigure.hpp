#pragma once

#include <QtGui>
#include <QtWidgets>

#include "ui_dlgExternalConfigure.h"

constexpr int COL_ICON      = 0;
constexpr int COL_NAME      = 1;
constexpr int COL_PROGRAM   = 2;
constexpr int COL_CMDLINE   = 3;
constexpr int COL_DETECT    = 4;
constexpr int COL_EXTS      = 5;

/// QTableWidget 의 RowIndex 는 실제 설정파일에 저장된 것과 순서가 일치해야한다. 
/// RowIndex + 1 == ProgramN 과 같음

class CExternalConfigure : public QDialog
{
    Q_OBJECT
public:
    CExternalConfigure( QWidget* parent = nullptr, Qt::WindowFlags f = Qt::Dialog );

    Q_INVOKABLE void                    Refresh();

    QSet< int >                         SelectedRow() const;

protected slots:

    void                                on_btnAdd_clicked( bool checked = false );
    void                                on_btnModify_clicked( bool checked = false );
    void                                on_btnRemove_clicked( bool checked = false );

private:

    
    Ui::dlgExternalOpts                 ui;
};