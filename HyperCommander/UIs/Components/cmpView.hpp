#pragma once

#include <QtWidgets>

#include "cmnTypeDefs.hpp"
#include "Modules/mdlFileSystem.hpp"

// Panel 에 표시되는 뷰 형태의 기초 클래스
// 또는 Lister 에 표시되는 WLX 등을 호스트한다.

class CFsModelT;
struct ColumnView;

class CViewT : public QWidget
{
    Q_OBJECT
public:
    enum TyEnViewMode { VM_NONE, VM_GRID, VM_QUICK, VM_LISTER };
    CViewT( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

    TyEnViewMode                    GetViewMode() const { return viewMode_; }
    virtual void                    ApplyColorScheme( const TyColorScheme& ColorScheme );
    virtual bool                    IsEditing() const { return false; };
    virtual void                    AdjustSizeToContents() const {};
    virtual void                    EnsureKeyboardFocusOnView() const {};
    virtual void                    CloseView() {};

signals:
    void                            sig_NotifyViewActivated();

protected:
    bool                            eventFilter( QObject* watched, QEvent* Event ) override;

    TyEnViewMode                    viewMode_ = VM_NONE;
};

namespace Qtitan
{
    class Grid;
    class GridViewBase;
    class GridSelection;
    class GridBandedTableView;
    class ContextMenuEventArgs;
    class CellClickEventArgs;
    class RowClickEventArgs;
    class EditorEventArgs;
} // Qtitan

class CGridView : public CViewT
{
    Q_OBJECT
public:
    CGridView( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

    void                            SetModel( CFsModelT* Source, QAbstractProxyModel* Proxy, ColumnView* Columns );
    void                            ApplyColorScheme( const TyColorScheme& ColorScheme ) override;
    bool                            IsEditing() const override;
    void                            AdjustSizeToContents() const override;
    void                            EnsureKeyboardFocusOnView() const override;

    nsHC::TySpFileSource            SelectRowOnCurrentTab( const QModelIndex& SrcIndex, bool IsMoveDown, bool& IsSelected );
    void                            ContextMenuOnCurrentTab( const QModelIndex& SrcIndex );

    Qtitan::GridViewBase*           BaseView() const;
    CFsModelT*                      BaseModel() const;

protected slots:
    void                            oo_grid_contextMenu( Qtitan::ContextMenuEventArgs* Args );
    void                            oo_grid_cellClicked( Qtitan::CellClickEventArgs* Args );
    void                            oo_grid_rowDblClicked( Qtitan::RowClickEventArgs* Args );
    void                            oo_grid_rowFocusChanged( int OldRowIndex, int NewRowIndex );
    void                            oo_grid_editorStarted( Qtitan::EditorEventArgs* Args );
    void                            oo_grid_editorPosting( Qtitan::EditorEventArgs* Args );
    void                            oo_grid_selectionChanged( Qtitan::GridSelection* NewSelection, Qtitan::GridSelection* OldSelection );

protected:
    bool                            eventFilter( QObject* watched, QEvent* Event ) override;
    void                            resizeEvent( QResizeEvent* event ) override;

private:
    QModelIndex                     retrieveCursorIndex() const;

    QVBoxLayout*                    vbox_ = nullptr;
    Qtitan::Grid*                   grid_ = nullptr;
    Qtitan::GridBandedTableView*    view_ = nullptr;

    CFsModelT*                      model_ = nullptr;
    QAbstractProxyModel*            proxy_ = nullptr;
    
    // DataGrid 의 클릭 상태 추적을 위한 타이머
    QTimer                          viewClickTimer_;
    int                             lastFocusedRowIndex_ = -1;
    int                             fileNameColumnIndex = -1;       // 추후 사용.
};

class CCardView : public CViewT
{
    Q_OBJECT
public:

private:
    QVBoxLayout*                    vbox_ = nullptr;
    Qtitan::Grid*                   grid_ = nullptr;

};

class CViewBase : public CViewT
{
    Q_OBJECT
public:
    CViewBase( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() )
        : CViewT( Parent, f )
    {}

    void                            SetFilePath( const QString& FilePath ) { filePath_ = FilePath; }
    QString                         GetFilePath() const { return filePath_; }

protected:

    QString                         filePath_;

private:
};

class CInternalViewer : public CViewBase
{
    Q_OBJECT
public:
    CInternalViewer( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

};

class CExternalViewer : public CViewBase
{
    Q_OBJECT
public:
    CExternalViewer( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::FramelessWindowHint  );
    ~CExternalViewer();

    void                            SetWLX( TySpWLX Viewer );
    bool                            Initialize();

    ///////////////////////////////////////////////////////////////////////////
    ///

    HWND                            ListLoad( HWND ParentWin, const QString& FilePath, int ShowFlags );

private:
    void                            setDefaultParams();

    static LRESULT CALLBACK         cbWndProc( _In_ HWND hWnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam );
    static LRESULT CALLBACK         cbWndProcChild( _In_ HWND hWnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam );

protected:

    TySpWLX                         viewer_;
    ATOM                            atom;
    QWidget*                        wParent = nullptr;
    HWND                            hParent = nullptr;

    QWidget*                        wChild = nullptr;
    HWND                            hChild = nullptr;

    QVBoxLayout*                    vbox_ = nullptr;
};

class CQuickView : public CViewT
{
    Q_OBJECT
public:
    CQuickView( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

    void                            SetFilePath( const QString& FilePath );

protected:
    void                            showEvent( QShowEvent* event ) override;

private:

    bool                            isInitOnce_ = false;
    QString                         filePath_;
    CInternalViewer*                internalViewer_ = nullptr;
    CExternalViewer*                externalViewer_ = nullptr;

    QVBoxLayout*                    vbox_ = nullptr;
};
