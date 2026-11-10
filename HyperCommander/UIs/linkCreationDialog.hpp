#pragma once
#include "dialogShell.hpp"
#include "UniqueLibs/linkCreation.hpp"

namespace hc::ui {
class LinkCreationDialog final : public DialogShell {
public:
    LinkCreationDialog(links::Request Request, int DirectoryCount, const QString& RelativeBase, QWidget* Parent = nullptr);
    ~LinkCreationDialog() override;
    links::Request Request() const;
private:
    void changed();
    void probe();
    links::Request request_;
    QString base_;
    fm::ui::PathEdit* path_ = nullptr;
    QComboBox *mode_ = nullptr, *conflict_ = nullptr;
    QCheckBox* parents_ = nullptr;
    QRadioButton *hard_ = nullptr, *symbolic_ = nullptr, *junction_ = nullptr, *absolute_ = nullptr, *relative_ = nullptr;
    QWidget* reference_ = nullptr;
    QLabel *description_ = nullptr, *preview_ = nullptr, *summary_ = nullptr;
    QTimer* timer_ = nullptr;
    QFutureWatcher<QJsonObject>* watcher_ = nullptr;
    quint64 generation_ = 0;
    bool targetIsName_ = false;
    std::stop_source stop_;
};
}
