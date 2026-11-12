#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

class QAction;
class QComboBox;
class QLabel;
class QStackedWidget;

namespace hc::document
{
    // 문서 화면이 도킹 작업 공간에 넘기는 패널 하나. 넘긴 뒤 보이기 · 숨기기는 작업 공간의 도구 창이 맡고,
    // 화면의 켜기 · 끄기 동작(toggle_)은 그 도구 창을 연다 · 닫는다. 패널은 화면이 지울 때 함께 지운다.
    struct DocumentToolPanelItem
    {
        QString kind_;              // "outline" · "bookTree" · "problems" · "output"
        QPointer<QWidget> widget_;
        QString label_;             // 출력 고르개에 보일 출처("빌드 로그" · "EPUBCheck 출력")
        QPointer<QAction> toggle_;  // 화면의 패널 켜기 · 끄기(없을 수 있음)
    };

    // 도구 창 내용 — 문서마다 다른 패널을 포커스 문서를 따라 바꿔 보인다. 포커스 문서에 패널이 없으면 빈
    // 안내를 보인다. 고르개가 있으면(출력) 열린 모든 문서의 출처를 고를 수 있고, 포커스 문서에 출처가 없을
    // 때는 고른 것을 그대로 둔다.
    class DocumentToolHost final : public QWidget
    {
        Q_OBJECT
    public:
        DocumentToolHost(const QString& EmptyText, bool Picker, QWidget* Parent = nullptr);
        ~DocumentToolHost() override;
        // Owner: 문서 id, OwnerName: 고르개에 보일 문서 이름.
        void Add(const QString& Owner, const QString& OwnerName, QWidget* Panel, const QString& Label = {});
        // 포커스 문서가 바뀌었다(빈 문자열 = 문서 없음).
        void SetActiveOwner(const QString& Owner);
        bool HasOwner(const QString& Owner) const;
        QWidget* CurrentPanel() const;
        void RenameOwner(const QString& Owner, const QString& OwnerName);
        // Panel을 앞에 보인다(문서가 그 패널을 보여 달라고 했을 때).
        void ShowPanel(QWidget* Panel);
    signals:
        void CurrentChanged();
    private:
        struct Entry
        {
            QString owner_, ownerName_;
            QPointer<QWidget> panel_;
            QString label_;
        };
        void removePanel(QObject* Panel);
        void refreshPicker();
        void showEntry(int Index);
        QList<Entry> entries_;
        QString active_;
        QStackedWidget* stack_ = nullptr;
        QLabel* empty_ = nullptr;
        QComboBox* picker_ = nullptr;
    };
}
