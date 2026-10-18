#include "text/TextSaveDialog.h"

#include "utils/EncodingDetector.h"

#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLayout>
#include <QSignalBlocker>
#include <QWidget>

namespace {

QString bomModeLabel(TextSaveDialog::BomMode mode)
{
    switch (mode) {
        case TextSaveDialog::BomMode::Include:
            return QObject::tr("BOM 포함");
        case TextSaveDialog::BomMode::Exclude:
            return QObject::tr("BOM 없음");
        case TextSaveDialog::BomMode::Auto:
        default:
            return QObject::tr("자동");
    }
}

} // namespace

TextSaveDialog::TextSaveDialog(QWidget* parent)
    : QFileDialog(parent)
{
    setAcceptMode(QFileDialog::AcceptSave);
    setFileMode(QFileDialog::AnyFile);
    setOption(QFileDialog::DontUseNativeDialog, true);
    setDefaultSuffix(QStringLiteral("txt"));
    setWindowTitle(tr("다른 이름으로 저장"));
    buildAccessoryUi();
}

void TextSaveDialog::setEncodingOptions(const QStringList& encodings,
                                        const QString& currentEncoding,
                                        bool currentHasBom,
                                        BomMode initialBomMode)
{
    if (!m_encodingCombo || !m_bomCombo)
        return;

    const QStringList uniqueEncodings = encodings.isEmpty()
        ? QStringList{QStringLiteral("UTF-8")}
        : encodings;

    m_encodingCombo->clear();
    m_encodingCombo->addItems(uniqueEncodings);

    const int encodingIndex = uniqueEncodings.indexOf(currentEncoding);
    if (encodingIndex >= 0)
        m_encodingCombo->setCurrentIndex(encodingIndex);
    else
        m_encodingCombo->setCurrentText(currentEncoding.isEmpty() ? QStringLiteral("UTF-8") : currentEncoding);

    m_bomCombo->clear();
    m_bomCombo->addItem(bomModeLabel(BomMode::Auto), static_cast<int>(BomMode::Auto));
    m_bomCombo->addItem(bomModeLabel(BomMode::Include), static_cast<int>(BomMode::Include));
    m_bomCombo->addItem(bomModeLabel(BomMode::Exclude), static_cast<int>(BomMode::Exclude));
    m_bomCombo->setItemData(0, currentHasBom, Qt::UserRole);
    const int bomIndex = m_bomCombo->findData(static_cast<int>(initialBomMode));
    m_bomCombo->setCurrentIndex(bomIndex >= 0 ? bomIndex : 0);

    refreshBomOptions();
}

void TextSaveDialog::setInitialFilePath(const QString& filePath)
{
    if (filePath.isEmpty())
        return;

    selectFile(filePath);
    setDirectory(QFileInfo(filePath).absolutePath());
}

void TextSaveDialog::setDialogFilters(const QStringList& nameFilters, const QString& selectedFilter)
{
    if (!nameFilters.isEmpty())
        setNameFilters(nameFilters);
    if (!selectedFilter.isEmpty())
        selectNameFilter(selectedFilter);
}

TextSaveDialog::SaveOptions TextSaveDialog::selectedOptions() const
{
    SaveOptions options;
    const QStringList files = selectedFiles();
    if (!files.isEmpty())
        options.filePath = files.constFirst();
    if (m_encodingCombo)
        options.encoding = m_encodingCombo->currentText().trimmed();
    if (m_bomCombo) {
        options.bomMode = static_cast<BomMode>(m_bomCombo->currentData().toInt());
    }
    return options;
}

bool TextSaveDialog::selectedBomIncluded() const
{
    const SaveOptions options = selectedOptions();
    switch (options.bomMode) {
        case BomMode::Include:
            return true;
        case BomMode::Exclude:
            return false;
        case BomMode::Auto:
        default:
            return EncodingDetector::supportsBom(options.encoding)
                && m_bomCombo
                && m_bomCombo->currentIndex() == 0
                && m_bomCombo->itemData(0, Qt::UserRole).toBool();
    }
}

void TextSaveDialog::buildAccessoryUi()
{
    auto* accessory = new QWidget(this);
    auto* formLayout = new QFormLayout(accessory);
    formLayout->setContentsMargins(9, 0, 9, 9);
    formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_encodingCombo = new QComboBox(accessory);
    m_encodingCombo->setEditable(false);
    formLayout->addRow(tr("인코딩:"), m_encodingCombo);

    m_bomCombo = new QComboBox(accessory);
    formLayout->addRow(tr("BOM:"), m_bomCombo);

    connect(m_encodingCombo, &QComboBox::currentTextChanged, this, [this] {
        refreshBomOptions();
    });

    if (auto* gridLayout = qobject_cast<QGridLayout*>(layout())) {
        gridLayout->addWidget(accessory, gridLayout->rowCount(), 0, 1, gridLayout->columnCount());
    } else if (layout()) {
        layout()->addWidget(accessory);
    }
}

void TextSaveDialog::refreshBomOptions()
{
    if (!m_encodingCombo || !m_bomCombo)
        return;

    const bool supportsBom = EncodingDetector::supportsBom(m_encodingCombo->currentText());
    const bool autoChecked = m_bomCombo->currentData().toInt() == static_cast<int>(BomMode::Auto);
    const bool includeChecked = m_bomCombo->currentData().toInt() == static_cast<int>(BomMode::Include);
    const bool preserveAutoValue = m_bomCombo->itemData(0, Qt::UserRole).toBool();

    const QSignalBlocker blocker(m_bomCombo);
    m_bomCombo->setItemData(0, preserveAutoValue, Qt::UserRole);
    m_bomCombo->setItemText(0,
                            supportsBom
                                ? tr("자동 (%1)").arg(preserveAutoValue ? tr("BOM 포함") : tr("BOM 없음"))
                                : tr("자동 (BOM 없음)"));
    m_bomCombo->setItemText(1, bomModeLabel(BomMode::Include));
    m_bomCombo->setItemText(2, bomModeLabel(BomMode::Exclude));
    m_bomCombo->setItemData(0,
                            supportsBom
                                ? tr("기존 파일의 BOM 여부를 유지합니다.")
                                : tr("현재 인코딩은 BOM을 지원하지 않습니다."),
                            Qt::ToolTipRole);
    m_bomCombo->setItemData(1,
                            supportsBom ? QVariant() : tr("현재 인코딩은 BOM을 지원하지 않습니다."),
                            Qt::ToolTipRole);
    m_bomCombo->setItemData(2, QVariant(), Qt::ToolTipRole);

    if (!supportsBom && includeChecked)
        m_bomCombo->setCurrentIndex(2);
    else if (autoChecked)
        m_bomCombo->setCurrentIndex(0);
}


