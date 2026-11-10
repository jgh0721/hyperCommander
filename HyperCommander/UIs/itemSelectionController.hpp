#pragma once

#include <QObject>
#include <QStringList>
#include <memory>

class CGridView;
namespace hc::ui {
class ItemSelectionController final : public QObject {
public:
    static ItemSelectionController* For(CGridView* View);
    explicit ItemSelectionController(CGridView* View);
    ~ItemSelectionController() override;
    void Execute(const QString& Command);
    // 선택을 다른 경로로 바꾸기 전 선택을 "이전 선택 복원"(hc.selection.restore) 대상으로 남긴다.
    // 문맥 메뉴가 선택 밖의 행을 오른쪽 클릭했을 때 쓴다(설계 261110 6절).
    void RememberPrevious(const QStringList& Keys);
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace hc::ui
