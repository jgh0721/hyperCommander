#pragma once

#include <QFileDialog>

class QComboBox;

class TextSaveDialog : public QFileDialog
{
    Q_OBJECT

public:
    enum class BomMode {
        Auto,
        Include,
        Exclude
    };

    struct SaveOptions {
        QString filePath;
        QString encoding;
        BomMode bomMode = BomMode::Auto;
    };

    explicit TextSaveDialog(QWidget* parent = nullptr);

    void setEncodingOptions(const QStringList& encodings,
                            const QString& currentEncoding,
                            bool currentHasBom,
                            BomMode initialBomMode = BomMode::Auto);
    void setInitialFilePath(const QString& filePath);
    void setDialogFilters(const QStringList& nameFilters, const QString& selectedFilter = {});

    SaveOptions selectedOptions() const;
    bool selectedBomIncluded() const;

private:
    void buildAccessoryUi();
    void refreshBomOptions();

    QComboBox* m_encodingCombo = nullptr;
    QComboBox* m_bomCombo = nullptr;
};

