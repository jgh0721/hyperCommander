#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QJsonObject>
#include <QWidget>
#include <QStringList>
#include "readingAssets.hpp"
class QPlainTextEdit;
class QCheckBox;
class QTabWidget;

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QPushButton;

namespace hc::epub::poc
{
    struct ReadingSnapshot
    {
        QJsonObject global;
        QJsonObject book;
        QString error;
    };

    // Developer store is intentionally separate from product preferences and EPUB bytes.
    class ReadingStore
    {
    public:
        static bool Valid(const QJsonObject& Profile);
        static QJsonObject Defaults();
        static QJsonObject Resolve(const QJsonObject& Global, const QJsonObject& Book);
        static ReadingSnapshot Load(const QString& Path, const QString& Book);
        static ReadingSnapshot Save(const QString& Path, const QString& Book, bool Global,
            const QJsonObject& Base, const QJsonObject& Draft);
    };

    class ReadingSettings final : public QWidget
    {
        Q_OBJECT
    public:
        ReadingSettings(const QString& Path, const QString& Book, const QString& Theme, QWidget* Parent = nullptr);
        QJsonObject Profile() const;
        void SetBookFonts(const QStringList& Families);
        void SetCssValidation(const QString& Css, const QString& Error);
        bool CssValid() const;
        bool Execute(const QString& Action, const QJsonObject& Patch, bool Global);
        QString CssError() const { return cssError_; }
        bool IsBusy() const { return busy_; }
        bool IsDirty() const { return draft_ != base_; }
    signals:
        void ProfileChanged(const QJsonObject& Profile);
    private:
        void addAppearance(QTabWidget* Tabs);
        void importBackground();
        void addNumber(QFormLayout* Form, const QString& Key, const QString& Label, double Min, double Max, double Step);
        void addField(QFormLayout* Form, const QString& Key, const QString& Label, QWidget* Editor);
        void edit(const QString& Key, const QJsonValue& Value);
        void refresh();
        void rebuildFonts();
        void acceptSnapshot(const ReadingSnapshot& Snapshot);
        void save();
        void reload();
        QString path_, book_, validatedCss_, cssError_;
        QComboBox* background_=nullptr;
        QCheckBox *cssEnabled_=nullptr,*cssForce_=nullptr;
        QPlainTextEdit* css_=nullptr;
        QPushButton *backgroundColor_=nullptr,*textColor_=nullptr,*backgroundImport_=nullptr;
        QFutureWatcher<ReadingAsset> assetWorker_;
        QStringList bookFonts_, systemFonts_;
        ReadingSnapshot snapshot_;
        QJsonObject base_, draft_;
        QHash<QString, QDoubleSpinBox*> numbers_;
        QHash<QString, QPushButton*> resets_;
        QComboBox *scope_ = nullptr, *mode_ = nullptr, *theme_ = nullptr, *font_ = nullptr, *alignment_ = nullptr;
        QComboBox *layout_=nullptr,*columns_=nullptr;
        QPushButton *apply_ = nullptr, *cancel_ = nullptr, *clear_ = nullptr, *reload_ = nullptr;
        QLabel* status_ = nullptr;
        QWidget* editors_ = nullptr;
        QFutureWatcher<ReadingSnapshot> worker_;
        bool busy_ = false, updating_ = false, loadFailed_ = false;
    };
}
