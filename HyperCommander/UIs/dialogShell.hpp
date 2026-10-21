#pragma once
#include <QDialog>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <optional>
#include <QJsonObject>
#include <QFutureWatcher>
#include <stop_token>
class QTimer;
class QRadioButton;
class QScrollArea;
class QTabWidget;
class QPushButton;

class QVBoxLayout;
class QLabel;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QSpinBox;
namespace fm::ui { class Banner; class Button; class DialogHeader; class PathEdit; class FolderPlanView; }

namespace hc::ui {
class DialogMessage;
// Resolves against an explicit folder; never uses the process working directory.
QString ResolveNativeDestination(const QString& Input, const QString& BaseDirectory);

// Product-owned frame. Only presentation primitives come from Fm::widgets.
class DialogShell : public QDialog
{
public:
    explicit DialogShell(const QString& Title, QWidget* Parent = nullptr);
    QVBoxLayout* BodyLayout() const { return bodyLayout_; }
    fm::ui::Button* AcceptButton() const { return accept_; }
    fm::ui::Button* CancelButton() const { return cancel_; }
    void SetTitle(const QString& Text);
    void SetSummary(const QString& Text);
    void SetMessage(const QString& Text, bool Error = false);
    void SetAcceptEnabled(bool Enabled);
    void SetInitialInput(QLineEdit* Input);
    void EnableCompactLayout(int Width = 720);
    void FitToContents();

protected:
    void accept() override;
    void showEvent(QShowEvent* Event) override;
    bool eventFilter(QObject* Watched, QEvent* Event) override;
    void ResetBody();

private:
    fm::ui::DialogHeader* header_ = nullptr;
    DialogMessage* message_ = nullptr;
    QVBoxLayout* bodyLayout_ = nullptr;
    fm::ui::Button* accept_ = nullptr;
    fm::ui::Button* cancel_ = nullptr;
    QPointer<QLineEdit> initialInput_;
    QScrollArea* bodyScroll_ = nullptr;
    QWidget* body_ = nullptr;
    QWidget* headerContainer_ = nullptr;
    QWidget* footer_ = nullptr;
    bool compact_ = false;
};

// Context remains in the command adapter: this value cannot execute file operations.
struct FileDialogRequest
{
    QString title_;
    QString summary_;
    QString label_;
    QString initialValue_;
    bool localFolder_ = false;
};

class FileInputDialog final : public DialogShell
{
public:
    explicit FileInputDialog(const FileDialogRequest& Request, QWidget* Parent = nullptr);
    QString Input() const;
    static std::optional<QString> Prompt(const FileDialogRequest& Request, QWidget* Parent);

private:
    void validateInput();
    fm::ui::PathEdit* input_ = nullptr;
};
class ArchiveExtractDialog final : public DialogShell {
public:
    ArchiveExtractDialog(const QStringList& Names, const QString& Destination, bool InsideArchive, QWidget* Parent = nullptr, const QString& BaseDirectory = {});
    QString Destination() const;
    QString Conflict() const;
    bool SeparateFolders() const;
    bool Queued() const;
private:
    void validate();
    QString baseDirectory_;
    QLineEdit* destination_;
    QComboBox* conflict_;
    QCheckBox *separate_, *queued_;
};
// Product request values are captured before entering the modal event loop.
struct FileOperationDialogRequest
{
    QString command_;
    QString source_;
    QString initialValue_;
    QStringList names_;
    bool nativeDestination_ = false;
    bool manifestSelection_ = false;
    bool nativeSource_ = false;
    bool copyEngine_ = true;
    QString providerKey_;
    QString relativeBase_;
};
class FileOperationDialog final : public DialogShell
{
public:
    explicit FileOperationDialog(const FileOperationDialogRequest& Request, QWidget* Parent = nullptr);
    QString Input() const;
    ~FileOperationDialog() override;
    QString Conflict() const;
    QString ResolvedTarget() const;
    bool TargetIsName() const;
    bool Queued() const { return queued_; }
    bool PreserveMetadata() const;
    bool VerifyCopy() const;
    bool CopyAcl() const;
    bool CopyStreams() const;
    QString IncludePattern() const;
    quint64 SpeedLimit() const;
    bool CreateParents() const;
    bool OpenCreatedFolder() const;
    bool Recycle() const;
    bool AllowReadOnly() const;
    static QStringList RecentTargets(const QString& Provider);
    static bool RememberTarget(const QString& Provider, const QString& Target);
private:
    void validateInput(bool RefreshProbe = true);
    void probeTarget();
    void summarizeSource();
    void arrangeCompactOperation();
    void updateOptionSummary();
    void toggleAdvancedOptions();
    QCheckBox* metadata_ = nullptr;
    QCheckBox* verify_ = nullptr;
    QCheckBox* acl_ = nullptr;
    QCheckBox* streams_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QSpinBox* speed_ = nullptr;
    bool queued_ = false;
    FileOperationDialogRequest request_;
    QComboBox* targetMode_ = nullptr;
    QCheckBox* parents_ = nullptr;
    QCheckBox* openCreated_ = nullptr;
    QCheckBox* readonly_ = nullptr;
    QRadioButton* recycle_ = nullptr;
    QLabel* sourceSummary_ = nullptr;
    QLabel* targetSummary_ = nullptr;
    QTimer* probeTimer_ = nullptr;
    QFutureWatcher<QJsonObject>* targetWatcher_ = nullptr;
    std::stop_source stop_;
    quint64 generation_ = 0;
    QString inspectedTarget_;
    QString targetKind_;
    bool sourceDirectory_ = false;
    fm::ui::PathEdit* input_ = nullptr;
    fm::ui::FolderPlanView* plan_ = nullptr;
    QLabel* preview_ = nullptr;
    QComboBox* conflict_ = nullptr;
    QCheckBox* extension_ = nullptr;
    QWidget* advanced_ = nullptr;
    QTabWidget* advancedTabs_ = nullptr;
    QPushButton* advancedToggle_ = nullptr;
    QLabel* optionSummary_ = nullptr;
};
} // namespace hc::ui
