#pragma once

#include "cmnTypeDefs_Opts.hpp"

#include "ui_dlgOpts.h"

const QPair< int, QString > PAGE_DISPLAY                    = { 0, QObject::tr("화면") };
const QPair< int, QString > PAGE_DISPLAY_FONTCOLOR          = { 1, QObject::tr("  글꼴 및 색상") };
const QPair< int, QString > PAGE_FILESET                    = { 2, QObject::tr("파일집합 정의") };
const QPair< int, QString > PAGE_SHORTCUT                   = { 2, QObject::tr("단축키") };
const QPair< int, QString > PAGE_CUSTOM_COLUMNS             = { 3, QObject::tr("사용자정의 컬럼") };

class QMainOpts : public QDialog
{
    Q_OBJECT
public:
    QMainOpts( QWidget* parent = Q_NULLPTR, Qt::WindowFlags flags = Qt::FramelessWindowHint );

    Q_INVOKABLE void                    LoadSettings();
    Q_INVOKABLE void                    SaveSettings();
    Q_INVOKABLE void                    SaveSettings_ColorScheme();
    Q_INVOKABLE void                    RefreshColorScheme( const QString& SchemeName );
    
protected slots:

    ///////////////////////////////////////////////////////////////////////////////
    /// TitleBar

    void                                on_btnClose_clicked( bool checked = false );

    ///////////////////////////////////////////////////////////////////////////////
    /// Left

    void                                on_lstOptCate_currentItemChanged( QListWidgetItem* Current, QListWidgetItem* Previous );

    ///////////////////////////////////////////////////////////////////////////////
    /// Content

    /// Page = pdDisplay_ColorScheme
    void                                on_cbxColorScheme_currentIndexChanged( int index );
    void                                on_btnAddColorScheme_clicked( bool checked = false );

    void                                on_btnLstFGColor_clicked( bool checked = false );
    void                                on_btnLstBGColor_clicked( bool checked = false );
    void                                on_btnLstCursorColor_clicked( bool checked = false );
    void                                on_btnLstSelectColor_clicked( bool checked = false );

    ///////////////////////////////////////////////////////////////////////////////
    /// Footer

    void                                on_btnOK_clicked( bool checked = false );
    void                                on_btnCancel_clicked( bool checked = false );
    void                                on_btnApply_clicked( bool checked = false );

private:
    void                                initialize();

    void                                setButtonColor( QPushButton* Button, const QColor& Color );
    void                                resetColorScheme( const QString& Name );

    Ui::dlgOpts                         ui;
};