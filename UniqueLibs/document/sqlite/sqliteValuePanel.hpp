#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QPersistentModelIndex>
#include <QVariant>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
namespace fm::ui { class SegmentedControl; }

namespace hc::sqlite
{
    // 값 패널(설계 6.4, 시안 ‘테이블 탭 — 보류 편집과 값 패널’). 그리드 오른쪽에서 지금 칸의 값 전체를 보이고 고친다.
    // 보기: 텍스트 · JSON(들여 쓴 모양) · 16진 · 이미지(BLOB 이 PNG · JPEG · GIF · WebP 일 때). 고칠 수 있는 칸이면
    // ‘적용 (Ctrl+Enter)’ 이 그리드의 보류 편집으로 넣고, ‘NULL로 설정’ 은 NULL 을 넣는다. 결과 그리드 · 뷰는 읽기만.
    class ValuePanel final : public QWidget
    {
        Q_OBJECT
    public:
        explicit ValuePanel(QWidget* Parent = nullptr);
        void SetCell(const QModelIndex& Index);
        const QPersistentModelIndex& Cell() const { return cell_; }
        void SetMonoFont(const QFont& Font);
        // 편집기에 포커스를 준다(셀에서 고치기 어려운 값을 F2 로 열 때).
        void FocusEditor();
        QPlainTextEdit* Editor() const { return editor_; }
        // 지금 보기에서 고친 값(텍스트 · JSON 은 글, 16진은 바이트). 고칠 수 없으면 무효.
        QVariant EditedValue(bool* Ok = nullptr) const;
    signals:
        void ApplyRequested(const QPersistentModelIndex& Cell, const QVariant& Value);
        void CloseRequested();
    protected:
        bool eventFilter(QObject* Watched, QEvent* Event) override;
    private:
        void showMode(int Mode);
        void refresh();
        void applyTheme();

        QPersistentModelIndex cell_;
        QVariant value_;
        bool editable_ = false;
        int mode_ = 0;
        QLabel* title_ = nullptr;
        QLabel* meta_ = nullptr;
        fm::ui::SegmentedControl* modes_ = nullptr;
        QStackedWidget* pages_ = nullptr;
        QPlainTextEdit* editor_ = nullptr;
        QLabel* image_ = nullptr;
        QPushButton* null_ = nullptr;
        QPushButton* apply_ = nullptr;
    };
}
