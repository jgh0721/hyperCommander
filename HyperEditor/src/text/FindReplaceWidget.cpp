#include "text/FindReplaceWidget.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QStyle>
#include <QApplication>
#include <QPainter>
#include <QPixmap>

namespace {

QIcon iconFromText(const QString& text, const QPalette& palette, int size = 16)
{
    QPixmap pix(size, size);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    QFont font = QApplication::font();
    font.setPixelSize(size - 4);
    font.setBold(true);
    p.setFont(font);
    p.setPen(palette.color(QPalette::Text));
    p.drawText(pix.rect(), Qt::AlignCenter, text);
    p.end();

    // checked state: draw with highlight color
    QPixmap pixChecked(size, size);
    pixChecked.fill(Qt::transparent);
    QPainter pc(&pixChecked);
    pc.setRenderHint(QPainter::Antialiasing);
    pc.setFont(font);
    pc.setPen(palette.color(QPalette::Highlight));
    pc.drawText(pixChecked.rect(), Qt::AlignCenter, text);
    pc.end();

    QIcon icon;
    icon.addPixmap(pix, QIcon::Normal, QIcon::Off);
    icon.addPixmap(pixChecked, QIcon::Normal, QIcon::On);
    return icon;
}

} // namespace

// ═══════════════════════════════════════════════════════════
// 생성자
// ═══════════════════════════════════════════════════════════

FindReplaceWidget::FindReplaceWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
    setReplaceMode(false);
    // 이벤트 필터 설치 (ESC, F3, Enter 처리)
    m_searchEdit->installEventFilter(this);
    m_replaceEdit->installEventFilter(this);
}

// ═══════════════════════════════════════════════════════════
// UI 구성
// ═══════════════════════════════════════════════════════════

void FindReplaceWidget::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 2, 4, 2);
    mainLayout->setSpacing(2);

    // ── 상단 행: 검색 ──────────────────────────────────────
    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(2);

    // > 토글 버튼 (치환 행 접기/펼치기)
    m_toggleBtn = new QToolButton(this);
    m_toggleBtn->setArrowType(Qt::RightArrow);
    m_toggleBtn->setFixedSize(20, 20);
    m_toggleBtn->setToolTip(tr("바꾸기 표시/숨기기"));
    m_toggleBtn->setCheckable(true);
    connect(m_toggleBtn, &QToolButton::toggled, this, [this](bool checked) {
        setReplaceMode(checked);
    });
    topRow->addWidget(m_toggleBtn);

    // 검색 입력 필드
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setMaximumWidth(280);
    m_searchEdit->setPlaceholderText(tr("검색"));
    m_searchEdit->setClearButtonEnabled(false);

    // 내부 액션: T1 대소문자 구분 (Leading 위치)
    regexAction_=new QAction(tr("정규식"),this);regexAction_->setCheckable(true);regexAction_->setText(".*");regexAction_->setToolTip(tr("정규식"));m_searchEdit->addAction(regexAction_,QLineEdit::TrailingPosition);connect(regexAction_,&QAction::toggled,this,[this]{emit optionsChanged();});
    m_caseAction = new QAction(this);
    m_caseAction->setCheckable(true);
    m_caseAction->setToolTip(tr("대/소문자 구분"));
    m_caseAction->setIcon(iconFromText(QStringLiteral("Aa"), palette()));
    m_searchEdit->addAction(m_caseAction, QLineEdit::TrailingPosition);
    connect(m_caseAction, &QAction::toggled, this, [this] {
        updatePlaceholderText();
        emit optionsChanged();
    });

    // 내부 액션: T2 단어 일치
    m_wordAction = new QAction(this);
    m_wordAction->setCheckable(true);
    m_wordAction->setToolTip(tr("단어 일치"));
    m_wordAction->setIcon(iconFromText(QStringLiteral("W"), palette()));
    m_searchEdit->addAction(m_wordAction, QLineEdit::TrailingPosition);
    connect(m_wordAction, &QAction::toggled, this, [this] {
        updatePlaceholderText();
        emit optionsChanged();
    });

    // 내부 액션: X (Clear) — 텍스트 입력 시에만 표시
    m_clearAction = m_searchEdit->addAction(
        style()->standardIcon(QStyle::SP_LineEditClearButton),
        QLineEdit::TrailingPosition);
    m_clearAction->setToolTip(tr("지우기"));
    m_clearAction->setVisible(false);
    connect(m_clearAction, &QAction::triggered, m_searchEdit, &QLineEdit::clear);

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_clearAction->setVisible(!text.isEmpty());
        emit findTextChanged(text);
    });

    topRow->addWidget(m_searchEdit);

    // 결과 개수 라벨
    m_countLabel = new QLabel(this);
    m_countLabel->setMinimumWidth(60);
    m_countLabel->setAlignment(Qt::AlignCenter);
    topRow->addWidget(m_countLabel);

    m_excludedCountLabel = new QLabel(this);
    m_excludedCountLabel->setMinimumWidth(60);
    m_excludedCountLabel->setAlignment(Qt::AlignCenter);
    topRow->addWidget(m_excludedCountLabel);

    // Btn1: 이전 항목
    m_prevBtn = new QToolButton(this);
    m_prevBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    m_prevBtn->setToolTip(tr("이전 찾기"));
    m_prevBtn->setAutoRaise(true);
    connect(m_prevBtn, &QToolButton::clicked, this, &FindReplaceWidget::findPrev);
    topRow->addWidget(m_prevBtn);

    // Btn2: 다음 항목
    m_nextBtn = new QToolButton(this);
    m_nextBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    m_nextBtn->setToolTip(tr("다음 찾기"));
    m_nextBtn->setAutoRaise(true);
    connect(m_nextBtn, &QToolButton::clicked, this, &FindReplaceWidget::findNext);
    topRow->addWidget(m_nextBtn);

    // Btn3: 필터 버튼 (메뉴)
    m_filterBtn = new QToolButton(this);
    m_filterBtn->setText(tr("⫶"));
    m_filterBtn->setToolTip(tr("필터"));
    m_filterBtn->setAutoRaise(true);
    m_filterBtn->setPopupMode(QToolButton::InstantPopup);
    m_filterMenu = new QMenu(this);
    m_searchInSelectionAction = m_filterMenu->addAction(tr("선택 영역에서 검색"));
    m_searchInSelectionAction->setCheckable(true);
    m_searchInSelectionAction->setEnabled(false); // 선택 없으면 비활성
    connect(m_searchInSelectionAction, &QAction::toggled, this, [this] {
        emit optionsChanged();
    });
    m_filterBtn->setMenu(m_filterMenu);
    topRow->addWidget(m_filterBtn);

    // Btn4: 메뉴 버튼
    m_menuBtn = new QToolButton(this);
    m_menuBtn->setText(tr("☰"));
    m_menuBtn->setToolTip(tr("옵션"));
    m_menuBtn->setAutoRaise(true);
    m_menuBtn->setPopupMode(QToolButton::InstantPopup);
    m_optionsMenu = new QMenu(this);
    m_autoScrollAction = m_optionsMenu->addAction(tr("입력 결과로 스크롤"));
    m_autoScrollAction->setCheckable(true);
    m_autoScrollAction->setChecked(true);
    connect(m_autoScrollAction, &QAction::toggled, this, [this] {
        emit optionsChanged();
    });
    m_menuBtn->setMenu(m_optionsMenu);
    topRow->addWidget(m_menuBtn);

    // X 닫기 버튼
    m_closeBtn = new QToolButton(this);
    m_closeBtn->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    m_closeBtn->setToolTip(tr("닫기 (ESC)"));
    m_closeBtn->setAutoRaise(true);
    connect(m_closeBtn, &QToolButton::clicked, this, [this] {
        hide();
        emit closed();
    });
    topRow->addWidget(m_closeBtn);

    mainLayout->addLayout(topRow);

    // ── 하단 행: 치환 ──────────────────────────────────────
    m_replaceRow = new QWidget(this);
    auto* bottomRow = new QHBoxLayout(m_replaceRow);
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(2);

    // 토글 버튼과 같은 너비의 스페이서
    bottomRow->addSpacing(22);

    m_replaceEdit = new QLineEdit(m_replaceRow);
    m_replaceEdit->setMaximumWidth(280);
    m_replaceEdit->setPlaceholderText(tr("바꿀 내용"));
    bottomRow->addWidget(m_replaceEdit);

    // Btn5: 바꾸기
    m_replaceBtn = new QPushButton(tr("바꾸기(&R)"), m_replaceRow);
    m_replaceBtn->setToolTip(tr("현재 강조된 항목 바꾸기"));
    connect(m_replaceBtn, &QPushButton::clicked, this, &FindReplaceWidget::replaceRequested);
    bottomRow->addWidget(m_replaceBtn);

    // Btn6: 모두 바꾸기
    m_replaceAllBtn = new QPushButton(tr("모두 바꾸기(&A)"), m_replaceRow);
    m_replaceAllBtn->setToolTip(tr("검색된 모든 항목 바꾸기"));
    connect(m_replaceAllBtn, &QPushButton::clicked, this, &FindReplaceWidget::replaceAllRequested);
    bottomRow->addWidget(m_replaceAllBtn);

    // Btn7: 제외
    m_excludeBtn = new QPushButton(tr("제외(&E)"), m_replaceRow);
    m_excludeBtn->setToolTip(tr("현재 항목을 바꾸기 대상에서 제외"));
    connect(m_excludeBtn, &QPushButton::clicked, this, &FindReplaceWidget::excludeRequested);
    bottomRow->addWidget(m_excludeBtn);

    mainLayout->addWidget(m_replaceRow);

    // 불투명 배경 + 하단 구분선
    setAutoFillBackground(true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, false);
}

// ═══════════════════════════════════════════════════════════
// 공개 API
// ═══════════════════════════════════════════════════════════

void FindReplaceWidget::setReplaceMode(bool replace)
{
    m_replaceMode = replace;
    m_replaceRow->setVisible(replace);
    m_toggleBtn->blockSignals(true);
    m_toggleBtn->setChecked(replace);
    m_toggleBtn->setArrowType(replace ? Qt::DownArrow : Qt::RightArrow);
    m_toggleBtn->blockSignals(false);

    if (QLayout* lay = layout())
        lay->invalidate();
    m_replaceRow->updateGeometry();
    updateGeometry();
    adjustSize();
    if (QWidget* p = parentWidget()) {
        if (QLayout* parentLayout = p->layout())
            parentLayout->invalidate();
        p->updateGeometry();
    }
}

bool FindReplaceWidget::isReplaceMode() const
{
    return m_replaceMode;
}

QString FindReplaceWidget::searchText() const
{
    return m_searchEdit ? m_searchEdit->text() : QString();
}

QString FindReplaceWidget::replaceText() const
{
    return m_replaceEdit ? m_replaceEdit->text() : QString();
}

bool FindReplaceWidget::isCaseSensitive() const
{
    return m_caseAction && m_caseAction->isChecked();
}

bool FindReplaceWidget::isWholeWord() const
{
    return m_wordAction && m_wordAction->isChecked();
}

bool FindReplaceWidget::isSearchInSelection() const
{
    return m_searchInSelectionAction && m_searchInSelectionAction->isChecked();
}

bool FindReplaceWidget::isAutoScrollToFirst() const
{
    return m_autoScrollAction && m_autoScrollAction->isChecked();
}

void FindReplaceWidget::setSearchInSelectionEnabled(bool enabled)
{
    if (m_searchInSelectionAction) {
        m_searchInSelectionAction->setEnabled(enabled);
        if (!enabled && m_searchInSelectionAction->isChecked())
            m_searchInSelectionAction->setChecked(false);
    }
}

void FindReplaceWidget::setSearchInSelectionChecked(bool checked)
{
    if (m_searchInSelectionAction && m_searchInSelectionAction->isEnabled()) {
        m_searchInSelectionAction->setChecked(checked);
    }
}

void FindReplaceWidget::setMatchCount(int count)
{
    if (m_countLabel) {
        if (count < 0 || (m_searchEdit && m_searchEdit->text().isEmpty()))
            m_countLabel->setText(QString());
        else
            m_countLabel->setText(tr("결과 %1 개").arg(count));
    }
}

void FindReplaceWidget::setExcludedCount(int count)
{
    if (!m_excludedCountLabel)
        return;

    if (count <= 0 || (m_searchEdit && m_searchEdit->text().isEmpty()))
        m_excludedCountLabel->setText(QString());
    else
        m_excludedCountLabel->setText(tr("제외 %1 개").arg(count));
}

void FindReplaceWidget::focusSearchField()
{
    if (m_searchEdit) {
        m_searchEdit->setFocus();
        m_searchEdit->selectAll();
    }
}

void FindReplaceWidget::setSelectionRange(int startPos, int endPos)
{
    m_selectionStart = startPos;
    m_selectionEnd = endPos;
}

// ═══════════════════════════════════════════════════════════
// PlaceholderText 업데이트
// ═══════════════════════════════════════════════════════════

void FindReplaceWidget::updatePlaceholderText()
{
    if (!m_searchEdit)
        return;

    QStringList modes;
    if (m_caseAction && m_caseAction->isChecked())
        modes << tr("대/소문자 구분");
    if (m_wordAction && m_wordAction->isChecked())
        modes << tr("단어 일치");

    if (modes.isEmpty())
        m_searchEdit->setPlaceholderText(tr("검색"));
    else
        m_searchEdit->setPlaceholderText(modes.join(tr(", ")));
}

// ═══════════════════════════════════════════════════════════
// paintEvent (불투명 배경 보장)
// ═══════════════════════════════════════════════════════════

void FindReplaceWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    // 하단 구분선
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawLine(0, height() - 1, width(), height() - 1);
}

// ═══════════════════════════════════════════════════════════
// 이벤트 필터 (ESC, F3, Shift+F3)
// ═══════════════════════════════════════════════════════════

void FindReplaceWidget::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        // These pixmap icons retain their creation colors until regenerated.
        if (m_caseAction) m_caseAction->setIcon(iconFromText(QStringLiteral("Aa"), palette()));
        if (m_wordAction) m_wordAction->setIcon(iconFromText(QStringLiteral("W"), palette()));
        if (m_clearAction) m_clearAction->setIcon(style()->standardIcon(QStyle::SP_LineEditClearButton));
        if (m_closeBtn) m_closeBtn->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    }
}

bool FindReplaceWidget::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            hide();
            emit closed();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            if (obj == m_searchEdit) {
                if (keyEvent->modifiers() & Qt::ShiftModifier)
                    emit findPrev();
                else
                    emit findNext();
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}








void FindReplaceWidget::SetSearchText(const QString& Text) { m_searchEdit->setText(Text); }
