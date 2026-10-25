#pragma once

/// External libraries
#include <fmwidgets/BreadcrumbBar.h>

/// Platform/framework headers
#include <QPointer>
#include <QTimer>
#include <stop_token>

class QMenu;

namespace hc::ui {

// Keeps the existing full-path presentation until the pointer reaches the path.
class PanelPathBar final : public fm::ui::BreadcrumbBar {
    Q_OBJECT
public:
    explicit PanelPathBar(QWidget* Parent = nullptr);
    ~PanelPathBar() override;
    void setFullPath(const QString& Path);
    void SetBreadcrumbsEnabled(bool Enabled);
    void EditPath();

signals:
    void activated();

protected:
    void enterEvent(QEnterEvent* Event) override;
    void paintEvent(QPaintEvent* Event) override;
    void mouseMoveEvent(QMouseEvent* Event) override;
    void mousePressEvent(QMouseEvent* Event) override;
    void mouseReleaseEvent(QMouseEvent* Event) override;
    void mouseDoubleClickEvent(QMouseEvent* Event) override;
    void leaveEvent(QEvent* Event) override;
    void resizeEvent(QResizeEvent* Event) override;
    void keyPressEvent(QKeyEvent* Event) override;
    void focusOutEvent(QFocusEvent* Event) override;
    bool eventFilter(QObject* Object, QEvent* Event) override;

private:
    struct Crumb { QString label_, path_; QRect text_, arrow_; };
    void layoutCrumbs();
    int hitAt(const QPoint& Position) const;
    bool overPath(const QPoint& Position) const;
    void updateHover(const QPoint& Position);
    void openMenu(int Hit);
    void dismissMenu();
    void navigate(const QString& Path);

    QString fullPath_, pendingPath_;
    QList<Crumb> crumbs_;
    QRect overflow_;
    int firstVisible_ = 0, hover_ = -1, pressed_ = -1, keyboard_ = 0;
    bool enabled_ = true, revealed_ = false, composing_ = false;
    QTimer clickTimer_;
    QPointer<QMenu> menu_;
    std::stop_source menuStop_;
};

} // namespace hc::ui
