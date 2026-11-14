#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QHeaderView>
#include <QTableView>

namespace hc::sqlite
{
    class CellDelegate;

    // 두 줄 열 머리글: 이름(기본 키는 경고색) + 선언 형식(고정폭 · 흐림). 시안 ‘결과 · 표’ 의 머리글.
    class GridHeader final : public QHeaderView
    {
        Q_OBJECT
    public:
        explicit GridHeader(QWidget* Parent = nullptr);
        QSize sizeHint() const override;
    protected:
        void paintSection(QPainter* Painter, const QRect& Rect, int Section) const override;
    };

    // 결과 · 테이블 그리드. 머리글을 누르면 정렬을 모델(ORDER BY)에 맡기고, Ctrl+C 는 고른 칸을 탭 구분 글로 복사한다.
    class GridView final : public QTableView
    {
        Q_OBJECT
    public:
        explicit GridView(QWidget* Parent = nullptr);
        CellDelegate* Delegate() const { return delegate_; }
        // 고른 칸(직사각형 묶음)을 탭 · 줄바꿈 글로. 고른 것이 없으면 현재 칸.
        QString SelectionText() const;
        // 처음 페이지 기준으로 열 너비를 맞추고 너무 넓은 열은 줄인다.
        void FitColumns(int MaxWidth = 320);
        // ORDER BY 를 손으로 바꾸거나 다른 테이블을 열면 머리글의 정렬 표시를 지운다.
        void ClearSort() { sortColumn_ = -1; horizontalHeader()->setSortIndicatorShown(false); }
        // 클립보드가 막힌 호스트(빠른 보기 · 제한된 작업): Ctrl+C 가 직접 쓰지 않고 CopyRequested 로 문서 창에 맡긴다.
        void SetBrokered(bool Enabled) { brokered_ = Enabled; }
    signals:
        void CopyRequested();
    protected:
        void keyPressEvent(QKeyEvent* Event) override;
        void changeEvent(QEvent* Event) override;
        void showEvent(QShowEvent* Event) override;
    private:
        CellDelegate* delegate_ = nullptr;
        int sortColumn_ = -1;
        Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
        bool brokered_ = false;
        // 행 높이는 글꼴에 맞춘다(스타일의 기본 머리글 높이는 결과 표에 너무 크다).
        void updateRowHeight();
    };
}
