#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QLabel>
#include <QMenu>
#include <QAction>
#include <QSet>
#include <QPair>

/// 텍스트 뷰어 상단에 표시되는 찾기/바꾸기 위젯 (전체 너비 밴드)
class FindReplaceWidget : public QWidget
{
    Q_OBJECT

public:
    explicit FindReplaceWidget(QWidget* parent = nullptr);

    void setReplaceMode(bool replace);
    bool isReplaceMode() const;

    QString searchText() const;
    void SetSearchText(const QString& Text);
    QString replaceText() const;

    bool isRegex() const {return regexAction_&&regexAction_->isChecked();}
    bool isCaseSensitive() const;
    bool isWholeWord() const;
    bool isSearchInSelection() const;
    bool isAutoScrollToFirst() const;

    void setSearchInSelectionEnabled(bool enabled);
    void setSearchInSelectionChecked(bool checked);
    void setMatchCount(int count);
    void setExcludedCount(int count);
    void focusSearchField();
    void setSelectionRange(int startPos, int endPos);
    int selectionRangeStart() const { return m_selectionStart; }
    int selectionRangeEnd() const { return m_selectionEnd; }

signals:
    void findNext();
    void findPrev();
    void findTextChanged(const QString& text);
    void replaceRequested();
    void replaceAllRequested();
    void excludeRequested();
    void closed();
    void optionsChanged();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QAction* regexAction_ = nullptr;
    void setupUi();
    void updatePlaceholderText();

    // ── 상단 행: 검색 ──
    QToolButton*  m_toggleBtn = nullptr;       // > 접기/펼치기
    QLineEdit*    m_searchEdit = nullptr;      // EditControl1
    QAction*      m_clearAction = nullptr;     // X clear
    QAction*      m_caseAction = nullptr;      // T1 대소문자 구분
    QAction*      m_wordAction = nullptr;      // T2 단어 일치
    QLabel*       m_countLabel = nullptr;      // 결과 X 개
    QLabel*       m_excludedCountLabel = nullptr; // 제외 X 개
    QToolButton*  m_prevBtn = nullptr;         // Btn1
    QToolButton*  m_nextBtn = nullptr;         // Btn2
    QToolButton*  m_filterBtn = nullptr;       // Btn3
    QToolButton*  m_menuBtn = nullptr;         // Btn4
    QToolButton*  m_closeBtn = nullptr;        // X 닫기

    // ── 하단 행: 치환 ──
    QWidget*      m_replaceRow = nullptr;
    QLineEdit*    m_replaceEdit = nullptr;     // EditControl2
    QPushButton*  m_replaceBtn = nullptr;      // Btn5 바꾸기
    QPushButton*  m_replaceAllBtn = nullptr;   // Btn6 모두 바꾸기
    QPushButton*  m_excludeBtn = nullptr;      // Btn7 제외

    // ── 필터/메뉴 ──
    QMenu*        m_filterMenu = nullptr;
    QAction*      m_searchInSelectionAction = nullptr;
    QMenu*        m_optionsMenu = nullptr;
    QAction*      m_autoScrollAction = nullptr;

    bool m_replaceMode = false;
    int  m_selectionStart = 0;
    int  m_selectionEnd = 0;
};



