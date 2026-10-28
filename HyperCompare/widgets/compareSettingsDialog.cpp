#include "stdafx.h"
#include "compareSettingsDialog.hpp"
#include "compareCommands.hpp"
#include "comparePalette.hpp"
#include "UniqueLibs/qtDialogFrame.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDialog>
#include <QFontInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QTimer>

namespace hc::compare
{
    namespace
    {
        QString keyText(const QList<QKeySequence>& Keys)
        { return Keys.isEmpty() ? QObject::tr("미지정") : Keys.first().toString(QKeySequence::NativeText) + (Keys.size() > 1 ? QObject::tr(" 외 %1개").arg(Keys.size() - 1) : QString{}); }
        class KeyCapture final : public QLineEdit
        {
        public:
            using QLineEdit::QLineEdit;
            std::function<void()> changed_;
        protected:
            bool event(QEvent* Event) override
            { if (Event->type() == QEvent::ShortcutOverride) { Event->accept(); return true; } return QLineEdit::event(Event); }
            void keyPressEvent(QKeyEvent* Event) override
            {
                if (Event->key() == Qt::Key_Tab || Event->key() == Qt::Key_Backtab) { QLineEdit::keyPressEvent(Event); return; }
                if (Event->key() == Qt::Key_Escape) { clear(); if (changed_) changed_(); Event->accept(); return; }
                if (Event->key() == Qt::Key_Control || Event->key() == Qt::Key_Shift || Event->key() == Qt::Key_Alt || Event->key() == Qt::Key_Meta) return;
                const QKeySequence key(QKeyCombination(Event->modifiers() & ~Qt::KeypadModifier, Qt::Key(Event->key())));
                setText(key.toString(QKeySequence::NativeText)); if (changed_) changed_(); Event->accept();
            }
        };
        class ColorPreview final : public QWidget
        {
        public:
            CompareSettings settings_;
            bool dark_ = false, merge_ = false;
            using QWidget::QWidget;
            QSize sizeHint() const override { return {390, 350}; }
        protected:
            void paintEvent(QPaintEvent*) override
            {
                QPainter painter(this); const auto color = [&](const char* Role) { return CompareColor(Role, dark_, settings_); };
                painter.fillRect(rect(), color("background"));
                const int row = std::max(26, fontMetrics().height() + 8), top = row * 2, lane = 28, bar = 20;
                const int pane = (width() - lane - bar) / 2;
                painter.setPen(color("foreground")); painter.drawText(QRect(8, 0, pane - 12, top), Qt::AlignVCenter, merge_ ? "LEFT" : tr("왼쪽"));
                painter.drawText(QRect(pane + lane + 8, 0, pane - 12, top), Qt::AlignVCenter, merge_ ? "RESULT" : tr("오른쪽"));
                const QStringList left{"struct Options {", "  retries = 2;", "  timeout = 30;", "", "  legacy = true;", "", "  return result;", "};"};
                const QStringList right{"struct Options {", "  retries = 3;", "  timeout = 60;", "", "", "  cache = true;", "  return result;", "};"};
                for (int side = 0; side < 2; ++side) for (int i = 0; i < left.size(); ++i)
                {
                    QString role;
                    if (i == 1 || i == 2) role = "modified";
                    if (i == 4) role = merge_ ? "conflict" : side ? "gap" : "removed";
                    if (i == 5) role = merge_ ? "resolved" : side ? "added" : "gap";
                    const QRect cell(side * (pane + lane), top + i * row, pane, row);
                    painter.fillRect(cell, role.isEmpty() ? color("background") : CompareColor(role + "Bg", dark_, settings_));
                    painter.setPen(role.isEmpty() || role == "gap" ? color("foreground") : CompareColor(role + "Fg", dark_, settings_));
                    const auto symbol = role == "added" ? "+ " : role == "removed" ? "− " : role == "conflict" ? "! " : role == "resolved" ? "✓ " : "  ";
                    const auto text = QString::number(i + 1) + " " + symbol + (side ? right[i] : left[i]);
                    if (role.isEmpty())
                    {
                        // Unchanged rows show the syntax colors; changed rows keep their glyph color.
                        const auto prefix = text.left(text.indexOf(' ') + 1 + 2); int x = cell.left() + 6 + fontMetrics().horizontalAdvance(prefix);
                        static const QRegularExpression tokens("\\d+|\\w+|\\s+|[^\\w\\s]+");
                        for (auto it = tokens.globalMatch(text.mid(prefix.size())); it.hasNext();)
                        {
                            const auto token = it.next().captured();
                            const auto ink = QStringList{"struct", "return", "true"}.contains(token) ? CompareColor("syntaxKeyword", dark_, settings_) :
                                token.front().isDigit() ? CompareColor("syntaxNumber", dark_, settings_) : color("foreground");
                            painter.setPen(ink); const QRect box(x, cell.top(), fontMetrics().horizontalAdvance(token), row);
                            if (box.right() < cell.right()) painter.drawText(box, Qt::AlignVCenter, token); x = box.right() + 1;
                        }
                        painter.setPen(color("foreground"));
                        painter.drawText(cell.adjusted(6, 0, -4, 0), Qt::AlignVCenter, prefix);
                    }
                    else painter.drawText(cell.adjusted(6, 0, -4, 0), Qt::AlignVCenter, fontMetrics().elidedText(text, Qt::ElideRight, pane - 10));
                    if (i == 1 || i == 2)
                    {
                        const auto at = text.indexOf('=') + 2; const auto number = text.mid(at).chopped(1);
                        QRect highlight(cell.left() + 6 + fontMetrics().horizontalAdvance(text.left(at)), cell.top() + 2, fontMetrics().horizontalAdvance(number), row - 4);
                        if (highlight.right() < cell.right()) {
                            auto ink = color("inline"); ink.setAlpha(CompareAlpha("inlineAlpha", dark_, settings_)); painter.fillRect(highlight, ink);
                            painter.drawText(highlight, Qt::AlignVCenter, number);
                        }
                    }
                }
                auto connector = color("connector"); connector.setAlpha(CompareAlpha("connectorAlpha", dark_, settings_));
                painter.fillRect(QRect(pane, top + row, lane, row * 2), connector); painter.setPen(color("connectorOutline")); painter.drawRect(QRect(pane, top + row, lane, row * 2));
                connector.setAlpha(CompareAlpha("resolvedAlpha", dark_, settings_)); painter.fillRect(QRect(pane, top + row * 5, lane, row), connector);
                painter.fillRect(QRect(width() - bar, top, bar, row * 8), color("barTrack"));
                const char* marks[]{"barModified", "barRemoved", "barAdded", "barGap"};
                for (int i = 0; i < 4; ++i) painter.fillRect(QRect(width() - bar + 5, top + row + i * row * 2, bar - 10, 5), color(marks[i]));
                painter.setPen(color("barViewport")); painter.drawRect(QRect(width() - bar + 1, top + row, bar - 3, row * 3));
            }
        };
        QWidget* scrollPage(QTabWidget* Tabs, const QString& Title, QWidget*& Contents)
        {
            auto* scroll = new QScrollArea(Tabs); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
            Contents = new QWidget(scroll); scroll->setWidget(Contents); Tabs->addTab(scroll, Title); return scroll;
        }
    }
    CompareSettingsDialog::CompareSettingsDialog(CompareSettings Settings, QWidget* Parent) : QDialog(Parent), draft_(std::move(Settings)), saved_(draft_)
    {
        setObjectName("compareSettingsDialog"); setWindowTitle(tr("HyperCompare 설정"));
        auto* layout = new QVBoxLayout(this); tabs_ = new QTabWidget(this); tabs_->setObjectName("compareSettingsTabs"); layout->addWidget(tabs_, 1);
        QWidget* appearance; scrollPage(tabs_, tr("화면과 글꼴"), appearance); auto* form = new QFormLayout(appearance);
        auto* theme = new QComboBox(appearance); theme->setObjectName("settingsTheme");
        theme->addItem(tr("본체 따르기"), "product"); theme->addItem(tr("밝게"), "light"); theme->addItem(tr("어둡게"), "dark");
        theme->setCurrentIndex(std::max(0, theme->findData(draft_.theme_))); form->addRow(tr("테마(&T)"), theme);
        connect(theme, &QComboBox::currentIndexChanged, this, [this, theme] { draft_.theme_ = theme->currentData().toString(); changed(); });
        const QString labels[]{tr("화면 글꼴"), tr("목록 글꼴"), tr("본문 글꼴")};
        for (int i = 0; i < 3; ++i)
        {
            auto* button = new QPushButton(appearance); button->setObjectName(QString("settingsFont%1").arg(i)); button->setAccessibleName(labels[i]);
            const auto describe = [this, button, i] { button->setText(QString("%1 · %2 pt").arg(draft_.fonts_[i].family()).arg(QFontInfo(draft_.fonts_[i]).pointSizeF())); };
            describe(); form->addRow(labels[i], button); connect(button, &QPushButton::clicked, this, [this, i, describe] {
                bool ok; const auto font = QFontDialog::getFont(&ok, draft_.fonts_[i], this); if (ok) { draft_.fonts_[i] = font; describe(); changed(); }
            });
        }
        auto* explanation = new QLabel(tr("본체 테마를 따르면서 비교 색상은 별도로 설정할 수 있습니다."), appearance); explanation->setWordWrap(true); form->addRow(explanation);
        QWidget* comparison; scrollPage(tabs_, tr("비교 표시"), comparison); auto* options = new QVBoxLayout(comparison);
        const QString viewLabels[]{tr("전체 패널 표시 줄 정렬"), tr("왼쪽·오른쪽 표시 줄 정렬"), tr("왼쪽·오른쪽 스크롤 동기화"), tr("줄 내부 차이 강조"), tr("차이 개요 막대")};
        for (int i = 0; i < 5; ++i) { auto* check = new QCheckBox(viewLabels[i], comparison); check->setObjectName(QString("settingsView%1").arg(i)); check->setChecked(draft_.view_[i]); options->addWidget(check); connect(check, &QCheckBox::toggled, this, [this, i](bool Value) { draft_.view_[i] = Value; changed(); }); }
        options->addWidget(new QLabel(tr("표시 옵션은 현재 비교와 이후에 여는 비교에 적용합니다."), comparison)); options->addStretch();
        QWidget* shortcuts; scrollPage(tabs_, tr("단축키"), shortcuts); auto* keysLayout = new QVBoxLayout(shortcuts); auto* filter = new QHBoxLayout(); keysLayout->addLayout(filter);
        search_ = new QLineEdit(shortcuts); search_->setObjectName("shortcutSearch"); search_->setPlaceholderText(tr("명령 또는 단축키 검색")); search_->setAccessibleName(tr("명령 또는 단축키 검색")); filter->addWidget(search_, 1);
        category_ = new QComboBox(shortcuts); category_->addItem(tr("모든 명령"), ""); category_->setAccessibleName(tr("명령 분류"));
        for (const auto& command : CompareCommands()) if (category_->findText(command.category_) < 0) category_->addItem(command.category_, command.category_); filter->addWidget(category_);
        modified_ = new QCheckBox(tr("변경한 항목만"), shortcuts); filter->addWidget(modified_);
        auto* keyRow = new QHBoxLayout(); keysLayout->addLayout(keyRow, 1);
        keys_ = new QTableWidget(shortcuts); keys_->setObjectName("shortcutTable"); keys_->setColumnCount(3); keys_->setHorizontalHeaderLabels({tr("명령"), tr("현재 단축키"), tr("기본값")});
        keys_->setSelectionBehavior(QAbstractItemView::SelectRows); keys_->setSelectionMode(QAbstractItemView::SingleSelection); keys_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        keys_->verticalHeader()->hide(); keys_->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed); keys_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        keys_->setColumnWidth(1, 145); keys_->setColumnWidth(2, 145); keys_->setMinimumHeight(300); keyRow->addWidget(keys_, 3);
        auto* detail = new QGroupBox(tr("선택한 명령"), shortcuts); auto* edit = new QVBoxLayout(detail); detail->setMinimumWidth(280); keyRow->addWidget(detail, 2);
        commandLabel_ = new QLabel(detail); edit->addWidget(commandLabel_); commandInfo_ = new QLabel(detail); commandInfo_->setWordWrap(true); edit->addWidget(commandInfo_);
        auto* key = new KeyCapture(detail); capture_ = key; capture_->setObjectName("shortcutCapture"); capture_->setReadOnly(true); capture_->setPlaceholderText(tr("클릭한 뒤 키를 누르세요")); capture_->setAccessibleName(tr("새 단축키")); edit->addWidget(new QLabel(tr("새 단축키"), detail)); edit->addWidget(capture_); key->changed_ = [this] { checkKey(); };
        auto* help = new QLabel(tr("한 번의 키 조합을 입력합니다. Esc는 입력을 취소합니다."), detail); help->setWordWrap(true); edit->addWidget(help);
        conflict_ = new QLabel(detail); conflict_->setObjectName("shortcutConflict"); conflict_->setWordWrap(true); edit->addWidget(conflict_);
        assign_ = new QPushButton(tr("할당"), detail); assign_->setObjectName("shortcutAssign"); edit->addWidget(assign_);
        auto* clear = new QPushButton(tr("할당 해제"), detail); clear->setObjectName("shortcutClear"); edit->addWidget(clear);
        auto* reset = new QPushButton(tr("이 명령 기본값"), detail); reset->setObjectName("shortcutReset"); edit->addWidget(reset); edit->addStretch();
        auto* resetAll = new QPushButton(tr("전체 기본값"), shortcuts); resetAll->setObjectName("shortcutsResetAll"); keysLayout->addWidget(resetAll, 0, Qt::AlignRight);
        connect(search_, &QLineEdit::textChanged, this, [this] { fillKeys(); }); connect(category_, &QComboBox::currentIndexChanged, this, [this] { fillKeys(); }); connect(modified_, &QCheckBox::toggled, this, [this] { fillKeys(); });
        connect(keys_, &QTableWidget::currentCellChanged, this, [this] { if (!filling_) selectCommand(); });
        connect(assign_, &QPushButton::clicked, this, [this] { if (assign_->isEnabled()) { draft_.shortcuts_[selected_] = QKeySequence::fromString(capture_->text(), QKeySequence::NativeText).toString(QKeySequence::PortableText); fillKeys(); changed(); } });
        connect(clear, &QPushButton::clicked, this, [this] { if (!selected_.isEmpty()) { draft_.shortcuts_[selected_] = ""; fillKeys(); changed(); } });
        connect(reset, &QPushButton::clicked, this, [this] { auto proposed = draft_.shortcuts_; proposed.remove(selected_); const auto error = ValidateCompareShortcuts(proposed); if (!error.isEmpty()) { conflict_->setText(error); return; } draft_.shortcuts_ = proposed; fillKeys(); changed(); });
        connect(resetAll, &QPushButton::clicked, this, [this] { for (const auto& command : CompareCommands()) draft_.shortcuts_.remove(command.id_); fillKeys(); changed(); });
        QWidget* colorPage; scrollPage(tabs_, tr("테마 색상"), colorPage); auto* colorLayout = new QVBoxLayout(colorPage); auto* paletteRow = new QHBoxLayout(); colorLayout->addLayout(paletteRow);
        paletteRow->addWidget(new QLabel(tr("편집할 색상"), colorPage)); colorMode_ = new QComboBox(colorPage); colorMode_->setObjectName("colorMode"); colorMode_->setAccessibleName(tr("편집할 색상")); colorMode_->addItems({tr("밝은 테마"), tr("어두운 테마")}); colorMode_->setCurrentIndex(CompareIsDark() ? 1 : 0); paletteRow->addWidget(colorMode_);
        colorGroup_ = new QComboBox(colorPage); colorGroup_->setObjectName("colorGroup"); colorGroup_->setAccessibleName(tr("색상 분류")); colorGroup_->addItems({tr("본문·줄 강조"), tr("비교막대"), tr("병합·연결 영역"), tr("구문 강조"), tr("고급")}); paletteRow->addWidget(colorGroup_); paletteRow->addStretch();
        auto* resetPalette = new QPushButton(tr("이 테마 기본값"), colorPage); resetPalette->setObjectName("colorsReset"); paletteRow->addWidget(resetPalette);
        link_ = new QCheckBox(tr("비교 색상과 연결"), colorPage); link_->setObjectName("linkOverviewColors"); colorLayout->addWidget(link_);
        auto* colorRow = new QHBoxLayout(); colorLayout->addLayout(colorRow, 1); colors_ = new QTableWidget(colorPage); colors_->setObjectName("colorTable"); colors_->setColumnCount(4); colors_->setHorizontalHeaderLabels({tr("항목"), tr("색상"), tr("HEX / 불투명도"), tr("기본값")});
        colors_->setEditTriggers(QAbstractItemView::NoEditTriggers); colors_->verticalHeader()->hide(); colors_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents); colors_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch); colors_->setMinimumSize(440, 330); colorRow->addWidget(colors_, 3);
        auto* previewBox = new QGroupBox(tr("미리보기"), colorPage); auto* previewLayout = new QVBoxLayout(previewBox); auto* sample = new QComboBox(previewBox); sample->addItems({tr("텍스트 비교"), tr("3방향 병합 · LEFT / RESULT")}); sample->setAccessibleName(tr("미리보기 종류")); previewLayout->addWidget(sample);
        preview_ = new ColorPreview(previewBox); preview_->setObjectName("compareColorPreview"); preview_->setMinimumSize(320, 330); preview_->setAccessibleName(tr("비교 색상 미리보기")); previewLayout->addWidget(preview_, 1); colorRow->addWidget(previewBox, 2);
        auto* note = new QLabel(tr("색상 변경은 미리보기에 즉시 반영됩니다. 밝은 테마와 어두운 테마는 각각 저장합니다."), colorPage); note->setWordWrap(true); colorLayout->addWidget(note);
        contrast_ = new QLabel(colorPage); contrast_->setObjectName("colorContrast"); contrast_->setWordWrap(true); colorLayout->addWidget(contrast_);
        connect(sample, &QComboBox::currentIndexChanged, this, [this](int Index) { static_cast<ColorPreview*>(preview_)->merge_ = Index == 1; refreshPreview(); });
        connect(colorMode_, &QComboBox::currentIndexChanged, this, [this] { fillColors(); }); connect(colorGroup_, &QComboBox::currentIndexChanged, this, [this] { fillColors(); });
        connect(link_, &QCheckBox::toggled, this, [this](bool Value) {
            if (filling_) return; const int mode = colorMode_->currentIndex();
            if (!Value) for (const auto* role : {"barModified", "barAdded", "barRemoved", "barGap"}) if (!draft_.colors_[mode].contains(role)) draft_.colors_[mode][role] = CompareColor(role, mode == 1, draft_).name();
            draft_.linked_[mode] = Value; fillColors(); changed();
        });
        connect(resetPalette, &QPushButton::clicked, this, [this] { const auto mode = colorMode_->currentIndex(); draft_.colors_[mode] = {}; draft_.linked_[mode] = true; fillColors(); changed(); });
        status_ = new QLabel(this); status_->setObjectName("settingsStatus"); status_->setWordWrap(true); layout->addWidget(status_);
        buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this); layout->addWidget(buttons_);
        buttons_->button(QDialogButtonBox::Ok)->setText(tr("확인")); buttons_->button(QDialogButtonBox::Cancel)->setText(tr("취소")); buttons_->button(QDialogButtonBox::Apply)->setText(tr("적용"));
        connect(buttons_->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { apply(); });
        connect(buttons_, &QDialogButtonBox::accepted, this, [this] { if (apply()) accept(); }); connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
        hc::ui::ApplyDialogFrame(this, buttons_, {{}, tr("화면·단축키·비교 색상을 설정합니다."), fm::ui::glyph::App});
        fillKeys(); fillColors(); changed(); if (!draft_.warnings_.isEmpty()) status_->setText(draft_.warnings_.join('\n'));
        tabs_->setCurrentIndex(std::clamp(QSettings{}.value("settings/lastTab", 0).toInt(), 0, 3));
        connect(tabs_, &QTabWidget::currentChanged, this, [](int Index) { QSettings{}.setValue("settings/lastTab", Index); });
        const auto available = screen()->availableGeometry().size() - QSize(40, 60); resize(QSize(1120, 780).boundedTo(available));
    }
    void CompareSettingsDialog::changed()
    {
        bool valid = true;
        for (int row = 0; row < colors_->rowCount(); ++row) if (auto* field = qobject_cast<QLineEdit*>(colors_->cellWidget(row, 2))) valid &= !field->property("invalidColor").toBool();
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(valid);
        buttons_->button(QDialogButtonBox::Apply)->setEnabled(valid && !(draft_ == saved_));
        status_->setText(!valid ? tr("올바른 색상 값을 입력하십시오.") : draft_ == saved_ ? tr("변경 사항 없음") : tr("적용하지 않은 변경 사항이 있습니다."));
    }
    bool CompareSettingsDialog::apply()
    {
        for (int row = 0; row < colors_->rowCount(); ++row) if (auto* value = colors_->cellWidget(row, 2); value && value->property("invalidColor").toBool()) { status_->setText(tr("올바른 색상 값을 입력하십시오.")); return false; }
        QString error; if (!draft_.Save(error)) { status_->setText(error); return false; }
        SetActiveCompareSettings(draft_); if (applied_) applied_(draft_); saved_ = draft_; changed(); return true;
    }
    void CompareSettingsDialog::fillKeys()
    {
        filling_ = true; keys_->setRowCount(0); const auto query = search_->text().trimmed(); int selectedRow = -1;
        for (const auto& command : CompareCommands())
        {
            const auto effective = CompareKeys(command.id_, draft_.shortcuts_); const auto current = keyText(effective);
            if (!category_->currentData().toString().isEmpty() && category_->currentData().toString() != command.category_) continue;
            if (modified_->isChecked() && effective == command.defaults_) continue;
            if (!query.isEmpty() && !(command.label_ + command.id_ + current).contains(query, Qt::CaseInsensitive)) continue;
            const int row = keys_->rowCount(); keys_->insertRow(row); auto* label = new QTableWidgetItem(command.label_); label->setData(Qt::UserRole, command.id_);
            if (effective != command.defaults_) label->setText(label->text() + tr(" · 변경")); keys_->setItem(row, 0, label);
            keys_->setItem(row, 1, new QTableWidgetItem(current)); keys_->setItem(row, 2, new QTableWidgetItem(keyText(command.defaults_)));
            for (int column = 1; column <= 2; ++column) {
                QStringList all; for (const auto& key : column == 1 ? effective : command.defaults_) all.append(key.toString(QKeySequence::NativeText));
                keys_->item(row, column)->setToolTip(all.join(", "));
            }
            label->setToolTip(command.label_);
            if (selected_ == command.id_) selectedRow = row;
        }
        if (keys_->rowCount()) keys_->setCurrentCell(selectedRow < 0 ? 0 : selectedRow, 0);
        filling_ = false; selectCommand();
    }
    void CompareSettingsDialog::selectCommand()
    {
        const auto* item = keys_->item(keys_->currentRow(), 0); selected_ = item ? item->data(Qt::UserRole).toString() : QString{};
        commandLabel_->setText(item ? item->text() : tr("일치하는 명령이 없습니다.")); commandInfo_->clear(); capture_->clear(); capture_->setEnabled(item != nullptr);
        for (const auto& command : CompareCommands()) if (command.id_ == selected_) { commandInfo_->setText(tr("적용 범위 · %1").arg(command.scope_)); commandInfo_->setToolTip(command.id_); capture_->setPlaceholderText(keyText(CompareKeys(selected_, draft_.shortcuts_))); }
        checkKey();
    }
    void CompareSettingsDialog::checkKey()
    {
        const auto key = QKeySequence::fromString(capture_->text(), QKeySequence::NativeText); QString error = CompareKeyError(key);
        auto proposed = draft_.shortcuts_; proposed[selected_] = key.toString(QKeySequence::PortableText);
        if (error.isEmpty()) error = ValidateCompareShortcuts(proposed);
        assign_->setEnabled(!selected_.isEmpty() && !key.isEmpty() && error.isEmpty());
        conflict_->setText(!error.isEmpty() ? error : key.isEmpty() ? tr("새 키를 누르면 충돌 여부를 확인합니다.") : tr("사용할 수 있습니다. 할당을 누르십시오."));
    }
    void CompareSettingsDialog::fillColors()
    {
        filling_ = true; const int mode = colorMode_->currentIndex(); const bool dark = mode == 1;
        link_->setVisible(colorGroup_->currentIndex() == 1); link_->setChecked(draft_.linked_[mode]);
        for (int row = 0; row < colors_->rowCount(); ++row) for (int column = 1; column < 4; ++column)
            if (auto* widget = colors_->cellWidget(row, column)) widget->setObjectName({});
        colors_->setRowCount(0);
        for (const auto& role : CompareColorRoles())
        {
            if (role.group_ != colorGroup_->currentText()) continue;
            const int row = colors_->rowCount(); colors_->insertRow(row); colors_->setItem(row, 0, new QTableWidgetItem(role.label_)); colors_->item(row, 0)->setData(Qt::UserRole, role.id_);
            const bool linked = draft_.linked_[mode] && QStringList{"barModified", "barAdded", "barRemoved", "barGap"}.contains(role.id_);
            auto* reset = new QPushButton(tr("복원"), colors_); reset->setAccessibleName(role.label_ + tr(" 기본값")); reset->setEnabled(!linked); colors_->setCellWidget(row, 3, reset);
            connect(reset, &QPushButton::clicked, this, [this, mode, role] { draft_.colors_[mode].remove(role.id_); QTimer::singleShot(0, this, [this] { fillColors(); }); changed(); });
            if (role.alpha_)
            {
                auto* alpha = new QSpinBox(colors_); alpha->setRange(0, 255); alpha->setValue(CompareAlpha(role.id_, dark, draft_)); alpha->setAccessibleName(role.label_); alpha->setObjectName("color_" + role.id_); colors_->setCellWidget(row, 2, alpha);
                connect(alpha, &QSpinBox::valueChanged, this, [this, mode, role](int Value) { draft_.colors_[mode][role.id_] = Value; refreshPreview(); changed(); }); continue;
            }
            auto* button = new QPushButton(colors_); button->setAccessibleName(role.label_ + tr(" 색상 선택")); button->setObjectName("swatch_" + role.id_); button->setEnabled(!linked); colors_->setCellWidget(row, 1, button);
            auto* hex = new QLineEdit(CompareColor(role.id_, dark, draft_).name().toUpper(), colors_); hex->setObjectName("color_" + role.id_); hex->setAccessibleName(role.label_ + " HEX"); hex->setMaximumWidth(112); hex->setEnabled(!linked); colors_->setCellWidget(row, 2, hex);
            const auto swatch = [button](QColor Color) {
                QPixmap pixmap(38, 18); pixmap.fill(Color); QIcon icon(pixmap);
                // Linked colors are read-only, but their actual hue must remain visible.
                icon.addPixmap(pixmap, QIcon::Disabled); button->setIcon(icon); button->setIconSize(pixmap.size());
            };
            swatch(CompareColor(role.id_, dark, draft_));
            connect(hex, &QLineEdit::textChanged, this, [this, mode, role, hex, swatch](const QString& Text) {
                const QColor value(Text); const bool valid = value.isValid() && Text.size() == 7 && Text.startsWith('#'); hex->setProperty("invalidColor", !valid);
                if (!valid) { status_->setText(tr("%1: #RRGGBB 형식으로 입력하십시오.").arg(role.label_)); buttons_->button(QDialogButtonBox::Ok)->setEnabled(false); buttons_->button(QDialogButtonBox::Apply)->setEnabled(false); return; }
                draft_.colors_[mode][role.id_] = value.name(); swatch(value); refreshPreview(); changed();
            });
            connect(button, &QPushButton::clicked, this, [this, mode, dark, role, hex] { const auto value = QColorDialog::getColor(CompareColor(role.id_, dark, draft_), this, role.label_); if (value.isValid()) hex->setText(value.name().toUpper()); });
        }
        filling_ = false; colors_->resizeRowsToContents(); refreshPreview(); changed();
    }
    void CompareSettingsDialog::refreshPreview()
    {
        auto* preview = static_cast<ColorPreview*>(preview_); preview->settings_ = draft_; preview->dark_ = colorMode_->currentIndex() == 1; preview->update();
        QStringList warnings; const bool dark = preview->dark_;
        for (const auto* role : {"modified", "added", "removed", "conflict", "resolved"}) if (CompareContrast(CompareColor(QString(role) + "Fg", dark, draft_), CompareColor(QString(role) + "Bg", dark, draft_)) < 4.5)
            for (const auto& item : CompareColorRoles()) if (item.id_ == QString(role) + "Fg") warnings.append(item.label_);
        // Syntax glyphs sit on the surface and on every changed-line background. Dimmed
        // inactive code is held to the large-text minimum (3:1) instead of 4.5:1.
        for (const auto& item : CompareColorRoles())
        {
            if (!item.id_.startsWith("syntax")) continue;
            const auto ink = CompareColor(item.id_, dark, draft_); const double minimum = item.id_ == "syntaxInactive" ? 3.0 : 4.5;
            for (const auto* back : {"background", "modifiedBg", "addedBg", "removedBg", "conflictBg"})
                if (CompareContrast(ink, CompareColor(back, dark, draft_)) < minimum) { warnings.append(item.group_ + " · " + item.label_); break; }
        }
        contrast_->setText(warnings.isEmpty() ? tr("글자 대비 양호 · 색상과 함께 기호 및 상태 문구를 표시합니다.") : tr("글자 대비가 낮습니다: %1").arg(warnings.join(", ")));
    }
}
