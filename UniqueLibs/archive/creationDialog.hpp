#pragma once
#include "creationSettings.hpp"
#include <QDialog>

class QComboBox; class QLineEdit; class QCheckBox; class QLabel; class QPushButton; class QTableWidget; class QScrollArea;
namespace hc::archive {
class CreationDialog final : public QDialog {
public:
    CreationDialog(settings::Repository& Settings, storage::Locator SourceRoot, QString Directory, uint64_t SelectionCount,
        QWidget* Parent = nullptr, const std::string& InitialFormat = {}, std::vector<storage::EntrySnapshot> SelectionEntries = {});
    ~CreationDialog() override;
    plugins::ArchiveCreateRequest Request() const;
    CreatorRuntime Creator() const;
    bool Background() const { return queued_; }
    storage::Result<storage::Unit> SavePreferences();
protected:
    bool eventFilter(QObject* Object,QEvent* Event) override;
    bool event(QEvent* Event) override;
    void keyPressEvent(QKeyEvent* Event) override;
    void showEvent(QShowEvent* Event) override;
private:
    void scheduleFit();
    void fitToContents();
    void formatChanged();
    void creatorChanged();
    void validate();
    void configure();
    void separateChanged();
    void remember();
    QString outputPath() const;
    QString suggestedName() const;
    void refreshPreview();
    settings::Repository& settingsRepository_;
    storage::Locator sourceRoot_;
    QString directory_, combinedPath_, folderPath_;
    std::vector<CreatorRuntime> catalog_;
    std::vector<bool> checked_;
    QJsonObject preferences_;
    std::vector<storage::EntrySnapshot> selection_;
    uint64_t selectionCount_ = 0;
    std::string format_;
    int creator_ = -1;
    std::optional<plugins::ArchiveCreateSettings> value_;
    bool separate_ = false, queued_ = false, settingsUnsupported_ = false, fitPending_ = false;
    QScrollArea* bodyScroll_ = nullptr;
    QComboBox *formats_, *creators_;
    QLineEdit *output_, *filename_;
    QTableWidget* files_;
    QLabel *filenameLabel_, *preview_;
    QPushButton* filesToggle_;
    QCheckBox *paths_, *recursive_, *individual_, *omitRoot_;
    QLabel *outputLabel_, *summary_, *error_, *notice_;
    QPushButton *configure_, *create_, *queue_;
    std::shared_ptr<std::stop_source> stop_ = std::make_shared<std::stop_source>();
};
}
