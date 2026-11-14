#pragma once

/// 고유 라이브러리
#include "sqliteSchema.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QHash>
#include <QWidget>

class QLabel;
class QHBoxLayout;
class QStackedWidget;
class QTabBar;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
namespace fm::ui { class FindBox; class Tag; }

namespace hc::sqlite
{
    // 작업 창 왼쪽의 스키마 패널(시안 ‘SQLite 작업 창’, 설계 4.1). 탭 ‘스키마 · 기록’, 머리글(파일 이름 · 저널 모드 ·
    // 암호화 칩 · 단추), 경로 · 크기, 필터, 트리(main → 테이블 · 뷰 · 인덱스 · 트리거 → 열, temp, ATTACH).
    class SchemaPanel final : public QWidget
    {
        Q_OBJECT
    public:
        struct Header
        {
            QString fileName_, path_, journalMode_;
            qint64 fileSize_ = -1;
            bool encrypted_ = false, readOnly_ = false;
        };
        explicit SchemaPanel(QWidget* Parent = nullptr);
        void SetHeader(const Header& Value);
        void SetSchema(const SchemaInfo& Schema);
        void SetRowCount(const QString& Database, const QString& Table, qint64 Count);
        void SetLocked(bool Locked);
        // 기록 탭: 최근 실행한 문(P2 에서 채운다).
        QWidget* HistoryPage() const;
        void SetHistoryWidget(QWidget* Widget);
        void ShowHistory();
        QTreeWidget* Tree() const { return tree_; }
        void FocusFilter();
        bool ShowSystemObjects() const { return showSystem_; }
    signals:
        // 테이블 · 뷰를 두 번 누르거나 Enter.
        void OpenObject(const QString& Database, const QString& Name);
        void RefreshRequested();
        void AttachRequested();
        void DetachRequested(const QString& Database);
        void CountRequested();
        void ShowDdlRequested(const QString& Database, const QString& Name);
        // 작업 창 명령(sqlite.editTable · renameObject · dropObject · newTable · dbInfo). 스키마 트리의 지금 항목에 적용한다.
        void CommandRequested(const QString& Id);
        // 가져오기 · 내보내기(시안 스키마 메뉴).
        void ImportRequested(const QString& Database, const QString& Name);
        void ExportRequested(const QString& Database, const QString& Name);
        // 스크립트 만들기 ▸ SELECT · INSERT · UPDATE · DELETE · CREATE(새 쿼리 탭).
        void ScriptRequested(const QString& Database, const QString& Name, const QString& Kind);
    protected:
        bool eventFilter(QObject* Watched, QEvent* Event) override;
        void changeEvent(QEvent* Event) override;
        void resizeEvent(QResizeEvent* Event) override;
    private:
        void rebuild();
        void applyFilter();
        void applyTheme();
        void showContextMenu(const QPoint& Position);
        void openItem(QTreeWidgetItem* Item);
        void elideSub();
        QString subText_;

        SchemaInfo schema_;
        Header header_;
        bool showSystem_ = false, locked_ = false;
        QHash<QString, qint64> counts_;
        QTabBar* tabs_ = nullptr;
        QStackedWidget* pages_ = nullptr;
        QLabel* icon_ = nullptr;
        QLabel* title_ = nullptr;
        QLabel* sub_ = nullptr;
        QHBoxLayout* chips_ = nullptr;
        QList<fm::ui::Tag*> chipTags_;
        QList<QToolButton*> tools_;
        fm::ui::FindBox* filter_ = nullptr;
        QTreeWidget* tree_ = nullptr;
        QWidget* lockedPage_ = nullptr;
        QWidget* history_ = nullptr;
    };
}
