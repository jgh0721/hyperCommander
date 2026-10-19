#pragma once
#include "creationSettings.hpp"
#include <QDialog>

class QComboBox; class QLineEdit; class QCheckBox; class QLabel; class QPushButton;
namespace hc::archive {
class CreationDialog final : public QDialog {
public:
    CreationDialog(settings::Repository& Settings, storage::Locator SourceRoot, QString Directory, uint64_t SelectionCount,
        QWidget* Parent = nullptr, const std::string& InitialFormat = {});
    ~CreationDialog() override;
    plugins::ArchiveCreateRequest Request() const;
    CreatorRuntime Creator() const;
    storage::Result<storage::Unit> SavePreferences();
private:
    void formatChanged();
    void creatorChanged();
    void validate();
    void configure();
    void separateChanged();
    void remember();
    QString outputPath() const;
    settings::Repository& settingsRepository_;
    storage::Locator sourceRoot_;
    QString directory_, combinedPath_, folderPath_;
    std::vector<CreatorRuntime> catalog_;
    std::vector<bool> checked_;
    QJsonObject preferences_;
    std::string format_;
    int creator_ = -1;
    std::optional<plugins::ArchiveCreateSettings> value_;
    bool separate_ = false, queued_ = false, settingsUnsupported_ = false;
    QComboBox *formats_, *creators_;
    QLineEdit* output_;
    QCheckBox *paths_, *recursive_, *individual_, *omitRoot_;
    QLabel *outputLabel_, *summary_, *error_, *notice_;
    QPushButton *configure_, *create_, *queue_;
    std::shared_ptr<std::stop_source> stop_ = std::make_shared<std::stop_source>();
};
}
