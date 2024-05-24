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
    void                                on_chkIsCaseSensitive_checkStateChanged( Qt::CheckState state );
    void                                on_chkIsOnlyOnce_checkStateChanged( Qt::CheckState state );
    void                                on_cbxSRApplyRange_currentIndexChanged( int index );

    /// Footer
    void                                on_btnRedo_clicked( bool checked = false );
    void                                on_btnRun_clicked( bool checked = false );
    void                                on_btnCancel_clicked( bool checked = false );
    void                                on_btnResultList_clicked( bool checked = false );
    void                                on_btnClose_clicked( bool checked = false );

private:

    QMultiRenameModel*                  Model = nullptr;
    Qtitan::GridBandedTableView*        View = nullptr;
    Ui::dlgMultiRename                  Ui;
};