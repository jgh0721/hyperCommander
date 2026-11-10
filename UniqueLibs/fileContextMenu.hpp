#pragma once
// HC 파일 문맥 메뉴(설계 261110). 대상 스냅샷으로 항목을 만들고, 지연 판정 결과로 제안 영역만 바꾼다.
// 항목은 대리 QAction이다. 브리지 QAction의 활성 상태를 바꾸지 않고, 고른 항목은 exec가 끝난 뒤
// 명령 ID와 매개변수로 돌려준다. 실행은 호출자(CommandUiBridge)가 단축키와 같은 경로로 한다.

#include "UniqueLibs/fileContextProbes.hpp"
#include "commonLibs/columns/columnContracts.hpp"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

class QEventLoop;
class QLabel;
class QHBoxLayout;
namespace hc::columns { class QueryEngine; }

namespace hc::ui
{
    struct MenuChoice { QString commandId_; QVariantMap parameters_; };
    // 항목 하나의 표시 상태. available_가 거짓이면 이 빌드에 없는 명령이라 항목을 만들지 않는다.
    struct CommandState { bool available_ = true, enabled_ = true, checked_ = false, checkable_ = false; QString reason_, key_; };

    class FileContextMenu final : public QMenu
    {
    public:
        using StateFor = std::function<CommandState(const QString& Id, const QVariantMap& Parameters)>;
        // 메뉴가 읽은 필드 값을 패널 모델 캐시(색상 규칙 · 열)와 나눈다.
        using Share = std::function<void(const QString& Path, const QString& Field, const hc::columns::CellValue& Value)>;

        FileContextMenu(filecontext::ContextTarget Target, filecontext::ProbeInputs Inputs, StateFor State, QWidget* Parent);
        ~FileContextMenu() override;
        void SetShare(Share Values) { share_ = std::move(Values); }
        // contextMenu.json 의 items(설계 10절 P2). when 조건에 맞는 항목만 넣는다.
        void SetUserItems(const QJsonArray& Items) { userItems_ = Items; }
        // Anchor는 전역 논리 좌표. Wait이면 로컬 고정 디스크의 지연 판정을 최대 40 ms 기다린 뒤 연다.
        std::optional<MenuChoice> Exec(const QPoint& Anchor, bool Wait);
        const filecontext::ProbeResult& Result() const { return result_; }
        const filecontext::ContextTarget& Target() const { return target_; }

    private:
        void build();
        void buildItemMenu();
        void buildBackgroundMenu();
        void buildUserItems(QMenu* Into, const QJsonArray& Items, int Depth);
        void refreshHeader();
        void refreshSuggestions();
        void startProbes();
        void collect(const QSet<QString>& Paths);
        bool apply(int Item, const QString& Field, const hc::columns::CellValue& Value);
        void expireProbes();
        void keepAnchor(const QRect& Before);
        int pendingCount() const;
        QAction* addCommand(QMenu* Into, const QString& Id, const QString& Label, QChar Mnemonic, const QIcon& Icon,
            const QVariantMap& Parameters = {}, const QString& Variant = {}, QAction* Before = nullptr, const QString& ToolTip = {});
        QIcon icon(filecontext::Icon Kind, filecontext::Tone Tone) const;

        filecontext::ContextTarget target_;
        filecontext::ProbeInputs inputs_;
        filecontext::ProbeResult result_;
        StateFor state_;
        Share share_;
        QJsonArray userItems_;
        std::vector<MenuChoice> choices_;
        std::vector<QAction*> suggestionActions_;
        QAction* defaultAction_ = nullptr;
        QAction* section_ = nullptr;
        QAction* placeholder_ = nullptr;
        QAction* suggestionEnd_ = nullptr;
        QLabel* title_ = nullptr;
        QLabel* detail_ = nullptr;
        QWidget* chips_ = nullptr;
        QHBoxLayout* chipRow_ = nullptr;
        std::unique_ptr<hc::columns::QueryEngine> engine_;
        QMultiHash<QString, std::pair<int, QString>> waiting_;   // 조회 경로 → (대상 위치, 필드)
        QPointer<QEventLoop> waitLoop_;
        QTimer expire_;
        QPoint anchor_;
        bool expired_ = false;
    };
}
