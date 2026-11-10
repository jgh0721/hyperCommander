#pragma once
#include <QtWidgets>
#include "commonLibs/columns/flatProfile.hpp"
#include <deque>

namespace fm::ui { class SegmentedControl; }

// 평면 폴더 세트 편집기: 표시 방식(단계형) · 열과 정렬 · 적용 조건.
// 설정 초안만 바꾼다. 저장은 설정 대화상자가 Library() · Selection()으로 한다.
class QtFlatProfileEditor final : public QWidget
{
    Q_OBJECT
public:
    explicit QtFlatProfileEditor(QWidget* Parent = nullptr);
    void Load(const QJsonObject& Library, const QJsonObject& Selection, const QJsonObject& NormalLibrary);
    void SetNormalLibrary(const QJsonObject& Library);
    void SetCurrentLocation(const QString& Path);
    bool Select(const QUuid& Id);
    const hc::columns::FlatProfile& Profile() const { return draft_; }
    void SetTitle(const QString& Title);
    // 새 세트(From의 복제, 없으면 기본 제공 세트 기준)를 만들고 그 식별자를 돌려준다.
    QUuid Add(const QUuid& From = {});
    // 현재 세트를 지우고 다음에 고를 세트를 돌려준다. 기본 제공 세트는 지우지 않는다.
    QUuid Remove();
    QJsonObject Library() const;
    QJsonObject Selection() const;
    QVector<hc::columns::FlatProfile> Profiles() const;
    bool Dirty() const { return dirty_; }
    void MarkSaved() { dirty_ = false; }
    QString Validate() const;
    // 세트 목록의 부제: 기본 제공 · 평면 기본 · 자동: … · 수동.
    QString Summary(const QUuid& Id) const;
signals:
    void Changed();
private:
    struct Row { QWidget* widget_ = nullptr; QLabel* fixed_ = nullptr; QWidget* control_ = nullptr; QString value_; };
    Row& addRow(QGridLayout* Grid, const QString& Label, QWidget* Control, int Level, const char* Name);
    fm::ui::SegmentedControl* segment(const QStringList& Items, const char* Name);
    void store();
    void syncCascade();
    void updatePreview();
    void loadColumns();
    void storeColumns();
    void loadRules();
    void storeRules();
    void appendRule(const QJsonObject& Rule);
    void refreshVerdict();
    void highlight(Row& Target);
    hc::columns::FlatProfile draft_;
    QJsonObject library_, selection_, normalLibrary_;
    QString currentLocation_;
    bool loading_ = false, dirty_ = false;
    QTabWidget* pages_ = nullptr;
    QLabel *preview_ = nullptr, *verdict_ = nullptr, *error_ = nullptr;
    fm::ui::SegmentedControl *view_, *structure_, *thumbStructure_, *location_, *prefixLook_, *prefixSort_, *columnPlace_, *secondRight_,
        *groupTitle_, *groupOrder_, *groupField_, *groupMode_, *groupDirection_, *expand_, *treeTotal_, *treeExpand_, *treeLines_,
        *lines_, *tileSize_, *tileChip_, *columnsFrom_;
    QCheckBox *titleCount_, *titleSize_, *titleLatest_, *defaultSet_, *selectionEnabled_;
    QComboBox *baseSet_, *sortColumn_, *sortOrder_, *defaultProfile_;
    QTableWidget* rules_ = nullptr;
    Row *rowView_, *rowStructure_, *rowThumbStructure_, *rowLocation_, *rowPrefixLook_, *rowPrefixSort_, *rowColumnPlace_, *rowSecondRight_,
        *rowGroupTitle_, *rowGroupOrder_, *rowGroupField_, *rowGroupMode_, *rowGroupDirection_, *rowTitle_, *rowExpand_, *rowTreeTotal_,
        *rowTreeExpand_, *rowTreeLines_, *rowLines_, *rowTileSize_, *rowTileChip_;
    std::deque<Row> rows_;
};
