#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

class QComboBoxItemWidget : public QWidget
{
    Q_OBJECT
public:
    QComboBoxItemWidget( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

    void                                SetItem( QListWidgetItem* Item );
    QListWidgetItem*                    GetItem() const { return widItem;}

    // IsLeft == false => Right
    QPushButton*                        AddButton( bool IsLeft, const QIcon& Icon = QIcon(), const QString& ToolTip = "");
    QPushButton*                        GetButton( bool IsLeft ) const;
    QLabel*                             AddLabel();
    QLabel*                             GetLabel() const;
    void                                SetText( const QString& Text );
    QString                             GetText() const;

    QVariant                            GetUserData( int Role ) const;
    void                                SetUserData( int Role, const QVariant& Data );

signals:
    void                                buttonClicked( bool checked = false );
    void                                contentClicked( bool checked = false );

private:

    QPushButton*                        btnLeft     = nullptr;
    QPushButton*                        btnContent  = nullptr;
    QLabel*                             lblContent = nullptr;
    QPushButton*                        btnRight    = nullptr;

    QString                             sText;
    QListWidgetItem*                    widItem     = nullptr;
};

class QComboBoxForWidget : public QComboBox
{
    Q_OBJECT
public:
    explicit QComboBoxForWidget( QWidget* parent = nullptr );

    QComboBoxItemWidget*                CreateItem();
    void                                AddItem( QListWidgetItem* Item, QComboBoxItemWidget* Widget );
    void                                AppendItem( QListWidgetItem* Item, QComboBoxItemWidget* Widget );
    QComboBoxItemWidget*                GetItem( int Index ) const;
    void                                RemoveItem( QListWidgetItem* Item );

    int                                 GetCount() const;

private:

    QListWidget*                        lstView = nullptr;
};