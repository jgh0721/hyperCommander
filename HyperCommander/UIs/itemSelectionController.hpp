#pragma once

#include <QObject>
#include <memory>

class CGridView;
namespace hc::ui {
class ItemSelectionController final : public QObject {
public:
    static ItemSelectionController* For(CGridView* View);
    explicit ItemSelectionController(CGridView* View);
    ~ItemSelectionController() override;
    void Execute(const QString& Command);
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace hc::ui
