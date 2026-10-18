#include "core/WatermarkDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QColorDialog>

WatermarkDialog::WatermarkDialog(const QBaseView::WatermarkConfig& initial, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("워터마크 설정"));
    setMinimumWidth(420);
    setupUI();
    loadConfig(initial);
}

QBaseView::WatermarkConfig WatermarkDialog::result() const { return m_config; }

void WatermarkDialog::setupUI()
{
    auto* mainLayout = new QVBoxLayout(this);

    // ── 활성화 ──
    m_enabledCheck = new QCheckBox(tr("워터마크 활성화"), this);
    m_forcedCheck  = new QCheckBox(tr("강제 적용 (사용자 해제 불가)"), this);
    mainLayout->addWidget(m_enabledCheck);
    mainLayout->addWidget(m_forcedCheck);

    // ── 유형 ──
    auto* typeGroup = new QGroupBox(tr("유형"), this);
    auto* typeLay = new QVBoxLayout(typeGroup);
    m_textRadio  = new QRadioButton(tr("텍스트"), this);
    m_imageRadio = new QRadioButton(tr("이미지"), this);
    m_textRadio->setChecked(true);

    m_textEdit   = new QLineEdit(this);
    m_textEdit->setPlaceholderText(tr("워터마크 문자열 입력"));

    m_textColorBtn = new QPushButton(tr("색상"), this);
    auto* textLay = new QHBoxLayout;
    textLay->addWidget(m_textEdit);
    textLay->addWidget(m_textColorBtn);

    connect(m_textColorBtn, &QPushButton::clicked, this, [this]{
        QColor c = QColorDialog::getColor(m_config.color, this, tr("글자 색상"));
        if (c.isValid()) {
            m_config.color = c;
            m_textColorBtn->setStyleSheet(QString("background-color: %1").arg(c.name()));
        }
    });

    auto* imgLay = new QHBoxLayout;
    m_imagePath  = new QLineEdit(this);
    m_imagePath->setReadOnly(true);
    m_imageBrowse = new QPushButton(tr("찾아보기..."), this);
    imgLay->addWidget(m_imagePath);
    imgLay->addWidget(m_imageBrowse);

    typeLay->addWidget(m_textRadio);
    typeLay->addLayout(textLay);
    typeLay->addWidget(m_imageRadio);
    typeLay->addLayout(imgLay);
    mainLayout->addWidget(typeGroup);

    connect(m_imageBrowse, &QPushButton::clicked, this, [this]{
        QString path = QFileDialog::getOpenFileName(this, tr("워터마크 이미지"),
            {}, tr("이미지 (*.png *.jpg *.bmp)"));
        if (!path.isEmpty()) m_imagePath->setText(path);
    });

    // ── 속성 ──
    auto* propGroup = new QGroupBox(tr("속성"), this);
    auto* form = new QFormLayout(propGroup);

    m_opacitySlider = new QSlider(Qt::Horizontal, this);
    m_opacitySlider->setRange(0, 100);
    m_opacityLabel  = new QLabel("30%", this);
    auto* opLay = new QHBoxLayout;
    opLay->addWidget(m_opacitySlider);
    opLay->addWidget(m_opacityLabel);
    connect(m_opacitySlider, &QSlider::valueChanged, this, [this](int v){
        m_opacityLabel->setText(QStringLiteral("%1%").arg(v));
    });
    form->addRow(tr("투명도:"), opLay);

    m_rotationSpin = new QDoubleSpinBox(this);
    m_rotationSpin->setRange(-180.0, 180.0);
    m_rotationSpin->setSuffix("°");
    form->addRow(tr("회전:"), m_rotationSpin);

    m_posXSlider = new QSlider(Qt::Horizontal, this);
    m_posXSlider->setRange(0, 100);
    form->addRow(tr("위치 X:"), m_posXSlider);

    m_posYSlider = new QSlider(Qt::Horizontal, this);
    m_posYSlider->setRange(0, 100);
    form->addRow(tr("위치 Y:"), m_posYSlider);

    m_sizeWSlider = new QSlider(Qt::Horizontal, this);
    m_sizeWSlider->setRange(5, 100);
    form->addRow(tr("크기 W:"), m_sizeWSlider);

    m_sizeHSlider = new QSlider(Qt::Horizontal, this);
    m_sizeHSlider->setRange(5, 100);
    form->addRow(tr("크기 H:"), m_sizeHSlider);

    mainLayout->addWidget(propGroup);

    // ── 버튼 ──
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, [this]{
        m_config.enabled  = m_enabledCheck->isChecked();
        m_config.forced   = m_forcedCheck->isChecked();
        m_config.opacity  = m_opacitySlider->value() / 100.0;
        m_config.rotation = m_rotationSpin->value();
        m_config.relativePos  = {m_posXSlider->value() / 100.0, m_posYSlider->value() / 100.0};
        m_config.relativeSize = {m_sizeWSlider->value() / 100.0, m_sizeHSlider->value() / 100.0};

        if (m_textRadio->isChecked()) {
            m_config.text  = m_textEdit->text();
            m_config.imagePath.clear();
            m_config.image = {};
        } else {
            m_config.text.clear();
            m_config.imagePath = m_imagePath->text();
            m_config.image = QPixmap(m_imagePath->text());
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void WatermarkDialog::loadConfig(const QBaseView::WatermarkConfig& cfg)
{
    m_config = cfg;
    m_enabledCheck->setChecked(cfg.enabled);
    m_forcedCheck->setChecked(cfg.forced);
    m_opacitySlider->setValue(static_cast<int>(cfg.opacity * 100));
    m_rotationSpin->setValue(cfg.rotation);
    m_posXSlider->setValue(static_cast<int>(cfg.relativePos.x() * 100));
    m_posYSlider->setValue(static_cast<int>(cfg.relativePos.y() * 100));
    m_sizeWSlider->setValue(static_cast<int>(cfg.relativeSize.width() * 100));
    m_sizeHSlider->setValue(static_cast<int>(cfg.relativeSize.height() * 100));

    if (!cfg.text.isEmpty()) {
        m_textRadio->setChecked(true);
        m_textEdit->setText(cfg.text);
    } else if (!cfg.image.isNull() || !cfg.imagePath.isEmpty()) {
        m_imageRadio->setChecked(true);
        m_imagePath->setText(cfg.imagePath);
    }

    if (m_textColorBtn) {
        m_textColorBtn->setStyleSheet(QString("background-color: %1").arg(cfg.color.name()));
    }
}
