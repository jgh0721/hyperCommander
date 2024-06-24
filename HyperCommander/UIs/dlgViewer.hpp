#pragma once

#include "cmnTypeDefs.hpp"
#include "UniqueLibs/solTCPluginMgr.hpp"

#include "ui_dlgViewer.h"

class QDlgViewer : public QDialog
{
    Q_OBJECT
public:
    QDlgViewer( QWidget* parent = nullptr, Qt::WindowFlags f = Qt::FramelessWindowHint );

    Q_INVOKABLE void                    SetFileName( const QString& FilePath );
    Q_INVOKABLE void                    SetInternalViewer( TySpWLX Lister );

protected slots:

    void                                on_btnClose_clicked( bool checked = false );

protected:
    void                                keyPressEvent( QKeyEvent* Ev ) override;

private:

    Q_INVOKABLE void                    initialize();

    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

    static LRESULT CALLBACK cbWndProc( _In_ HWND hWnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam );
    static LRESULT CALLBACK cbWndProcChild( _In_ HWND hWnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam );

    quint32                             currentIndex = 0;
    QString                             currentFilePath;

    Ui::dlgViewer                       ui;
};