#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAbstractListModel>
#include <QWidget>
#include <QTimer>
#include <functional>
#include <atomic>
#include <memory>

class QStackedWidget;
namespace fm::ui { class TextField; class PathEdit; class Button; class Label; }
namespace fm::filelist { class PathValueEditor; }
namespace hc::ui {
struct PathResolution {
    QString expanded;
    enum class Status { Pending, Exists, Missing, Unresolved } status = Status::Pending;
    QString warning;
};
using PathResolver = std::function<PathResolution(const QString&)>;
class PathEntryModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit PathEntryModel(QObject* Parent = nullptr);
    ~PathEntryModel() override;
    int rowCount(const QModelIndex& Parent = {}) const override;
    QVariant data(const QModelIndex& Index, int Role) const override;
    Qt::ItemFlags flags(const QModelIndex& Index) const override;
    bool setData(const QModelIndex& Index, const QVariant& Value, int Role = Qt::EditRole) override;
    bool moveRows(const QModelIndex& Source, int Row, int Count, const QModelIndex& Destination, int Target) override;
    bool SetValue(const QString& Value);
    QString Value() const;
    void SetResolver(PathResolver Resolver);
    bool Insert(int Row, const QString& Raw = {});
    bool Remove(int Row);
signals:
    void ValueChanged();
private:
    struct Entry { QString raw; PathResolution resolution; };
    struct Worker;
    void resolve();
    void resolveChunk(int Start, quint64 Generation, const std::shared_ptr<std::atomic_bool>& Cancel);
    QList<Entry> entries_;
    PathResolver resolver_;
    QTimer debounce_;
    std::shared_ptr<Worker> worker_;
    std::shared_ptr<std::atomic_bool> cancel_;
    quint64 generation_ = 0;
};
// A target-specific resolver is injected (current user, another user, system or
// a future environment preview). The widget never guesses that environment.
class PathListEditor final : public QWidget
{
    Q_OBJECT
public:
    explicit PathListEditor(QWidget* Parent = nullptr);
    bool SetValue(const QString& Value);
    QString Value() const;
    bool IsModified() const;
    void SetResolver(PathResolver Resolver);
    PathEntryModel* Model() const { return model_; }
signals:
    void ModifiedChanged(bool Modified);
private:
    void syncEditor();
    PathEntryModel* model_;
    fm::filelist::PathValueEditor* list_;
    fm::ui::PathEdit* editor_;
    fm::ui::Button* remove_;
    fm::ui::Label* notice_;
    QStackedWidget* pages_;
    fm::ui::TextField* raw_;
    bool textMode_ = false;
    QString original_;
};
}
