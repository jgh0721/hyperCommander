#pragma once
#include "documentSearchEngine.hpp"
#include <QWidget>
#include <QThread>
#include <QPointer>
#include <functional>
#include <optional>
#include <QStringList>
class QLineEdit; class QCheckBox; class QLabel; class QTableView; class QPushButton; class QComboBox;
namespace hc::document::search {
class ResultModel;
class Panel final : public QWidget {
    Q_OBJECT
public:
    explicit Panel(QWidget* Parent = nullptr);
    ~Panel() override;
    void Open(bool Replace, QString DefaultRoot, bool Book, QJsonArray Buffers);
    void SetSources(std::function<QJsonArray()> Provider) { sources_ = std::move(Provider); }
    void SetBookSources(std::function<std::function<QJsonArray()>()> Provider) { bookSources_ = std::move(Provider); }
    bool Busy() const { return applying_; }
    static bool AllStopped() { return liveThreads_.load() == 0; }
    void SetReadOnly(bool Value);
    void SourcesChanged() { if (isVisible() && !applying_) schedule(); }
    void SetQuery(const QString& Text);
    QJsonObject Context() const;
    void RestoreContext(const QJsonObject& Context);
    void FinishBuffer(quint64 Ticket, const QString& Error);
    // Docked inside an EPUB editor's problems panel: the host tab shows the title, so the panel hides
    // its own title, placement and close controls.
    void SetEmbedded(bool Embedded);
    // '파일에서 찾기 · “검색어” n건' for the hosting tab.
    QString Summary() const;
signals:
    void SummaryChanged(const QString& Summary);
    void Navigate(const QJsonObject& Row);
    void PlacementRequested(const QString& Location);
    void CloseRequested();
    void EditorRequested(const QJsonObject& Context);
    void ReplaceRequested(const QString& Scope, const QJsonArray& Ids);
    void ApplyBuffer(quint64 Ticket, const QJsonObject& Source, const QString& Expected, const QString& Replacement);
private:
    void schedule();
    void start();
    void invalidatePreview();
    void replace(const QString& Scope);
    void showResultMenu(const QPoint& Position);
    QString rowText(const QJsonObject& Row) const;
    void copyAll();
    void clearResults();
    QLineEdit *query_, *replacement_, *root_, *include_, *exclude_;
    QCheckBox *regex_, *case_, *recursive_, *replaceMode_;
    QComboBox* target_;
    QLabel *status_, *replacementLabel_, *title_ = nullptr;
    QWidget *dock_ = nullptr, *close_ = nullptr;
    qint64 rows_ = 0;
    // 모든 결과 복사 reads the spooled pages one by one.
    struct CopyJob { quint64 generation_ = 0, version_ = 0; int next_ = 0, total_ = 0; QStringList lines_; };
    std::optional<CopyJob> copy_;
    QTableView* table_;
    ResultModel* model_;
    QPushButton *selected_, *file_, *all_, *undo_;
    QThread* thread_;
    Engine* engine_;
    QTimer debounce_;
    QJsonArray buffers_;
    QString defaultRoot_;
    std::function<QJsonArray()> sources_;
    std::function<std::function<QJsonArray()>()> bookSources_;
    quint64 generation_ = 0, previewVersion_ = 0;
    bool book_ = false, starting_ = false, applying_ = false, readOnly_ = false, complete_ = false;
    inline static std::atomic_int liveThreads_{0};
};
}
