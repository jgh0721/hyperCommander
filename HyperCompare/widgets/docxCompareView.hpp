#pragma once
#include "extensionView.hpp"
#include <QJsonArray>
class QTableView;
class QPlainTextEdit;
namespace hc::compare
{
    class ArchiveCompareView;
    class DocxCompareView final : public ExtensionView
    {
    public:
        DocxCompareView(const QJsonObject& Comparison, const QStringList& Paths, QWidget* Parent);
        void SetListFont(const QFont& Font) override;
        void SetTextFont(const QFont& Font) override;
    private:
        QTableView* table_ = nullptr;
        QPlainTextEdit* left_ = nullptr;
        QPlainTextEdit* right_ = nullptr;
        ArchiveCompareView* parts_ = nullptr;
    };
}
