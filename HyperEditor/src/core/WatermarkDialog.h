#pragma once

#include "core/QBaseView.h"

#include <QDialog>
#include <QCheckBox>
#include <QLineEdit>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QRadioButton>
#include <QPushButton>
#include <QLabel>

/// 워터마크 설정 대화상자
class WatermarkDialog : public QDialog
{
    Q_OBJECT

public:
    explicit WatermarkDialog(const QBaseView::WatermarkConfig& initial, QWidget* parent = nullptr);

    QBaseView::WatermarkConfig result() const;

private:
    void setupUI();
    void loadConfig(const QBaseView::WatermarkConfig& cfg);

    QBaseView::WatermarkConfig m_config;

    QCheckBox*      m_enabledCheck      = nullptr;
    QCheckBox*      m_forcedCheck       = nullptr;
    QRadioButton*   m_textRadio         = nullptr;
    QRadioButton*   m_imageRadio        = nullptr;
    QLineEdit*      m_textEdit          = nullptr;
    QPushButton*    m_textColorBtn      = nullptr;
    QLineEdit*      m_imagePath         = nullptr;
    QPushButton*    m_imageBrowse       = nullptr;
    QSlider*        m_opacitySlider     = nullptr;
    QLabel*         m_opacityLabel      = nullptr;
    QDoubleSpinBox* m_rotationSpin      = nullptr;
    QSlider*        m_posXSlider        = nullptr;
    QSlider*        m_posYSlider        = nullptr;
    QSlider*        m_sizeWSlider       = nullptr;
    QSlider*        m_sizeHSlider       = nullptr;
};
