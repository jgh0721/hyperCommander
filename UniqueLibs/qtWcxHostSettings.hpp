#pragma once
#include <QWidget>
#include <QByteArray>
#include <vector>

class QSettings;
class QTableWidget;
class QPushButton;
class QWcxHostSettings final : public QWidget
{
public:
    explicit QWcxHostSettings(QWidget* Parent = nullptr);
    void Load(QSettings& Settings);
    void Save(QSettings& Settings);
private:
    struct Row { int index_; QString path_, manifest_; QByteArray reviewed_; };
    void review();
    void revoke();
    void updateState(int Row);
    QTableWidget* table_;
    QPushButton* review_;
    std::vector<Row> rows_;
    unsigned generation_ = 0;
};
