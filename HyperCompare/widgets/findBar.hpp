#pragma once
#include <QToolBar>
#include <QPointer>
#include <QVector>
#include <QElapsedTimer>
#include <optional>
#include <cstdint>

class ScintillaEditBase;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QLabel;
class QWidget;
class QTimer;
class QDockWidget;
class QTreeWidget;

namespace hc::compare
{
    inline constexpr int searchMatchIndicator = 21;
    class FindBar final : public QToolBar
    {
        Q_OBJECT
    public:
        explicit FindBar(QWidget* Parent);
        void SetEditors(const QVector<ScintillaEditBase*>& Editors);
        void SetBusy(bool Busy);
        void Open();
        void Close();
        void Find(bool Backwards);
        void FindAll();
        void SetListFont(const QFont& Font);
    protected:
        void changeEvent(QEvent* Event) override;
        bool eventFilter(QObject* Object, QEvent* Event) override;
    private:
        ScintillaEditBase* editor() const;
        void clearFeedback();
        void resetSearch();
        void clearMatches();
        void finishAll(bool Limited);
        void styleMatches();
        void step();
        void stepOnce();
        struct Search
        {
            QPointer<ScintillaEditBase> editor_;
            QByteArray text_;
            std::intptr_t cursor_ = 0, length_ = 0, anchor_ = 0, caret_ = 0, overlap_ = 0;
            int flags_ = 0;
            bool backwards_ = false, wrapped_ = false, all_ = false;
            int count_ = 0;
            QElapsedTimer elapsed_;
        };
        std::optional<Search> search_;
        QTimer* timer_ = nullptr;
        QVector<QPointer<ScintillaEditBase>> editors_;
        QVector<QMetaObject::Connection> connections_;
        QWidget* controls_ = nullptr;
        QComboBox* scope_ = nullptr;
        QLineEdit* query_ = nullptr;
        QCheckBox* case_ = nullptr;
        QCheckBox* word_ = nullptr;
        QLabel* feedback_ = nullptr;
        QPointer<QDockWidget> resultsDock_;
        QPointer<QTreeWidget> results_;
        QPointer<ScintillaEditBase> resultEditor_;
        bool busy_ = false;
    };
}
