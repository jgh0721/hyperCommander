#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFont>
#include <QStyledItemDelegate>

namespace hc::sqlite
{
    // 설정 › SQLite › 결과 · 편집 › 셀 표시(시안 ‘설정 창 · SQLite’).
    struct CellDisplay
    {
        QString nullText_ = QStringLiteral("NULL");
        int longText_ = 256;            // 이보다 길거나 줄이 여럿이면 셀에서 고치지 않고 값 패널로
        bool mismatch_ = true;          // 선언 형식과 저장 형식이 다르면 표시
        bool alignNumbers_ = true;      // 숫자를 오른쪽에
        bool blobPreview_ = true;       // 이미지 BLOB 은 작은 그림 + 형식 · 크기
    };

    // 결과 · 테이블 그리드의 셀 그리기와 고치기(설계 6.4). 모델은 값과 상태만 역할로 내주고(sqliteResultModel 의 CellRole),
    // 모양은 여기서 정한다: NULL 은 흐린 기울임, BLOB 은 흐린 요약, 숫자는 오른쪽 맞춤, 선언 형식과 저장 형식이
    // 어긋나면 오른쪽 위 삼각형, 보류 편집은 수정 칸 노랑 · 새 행 초록 · 지울 행 빨강 취소선, 커밋 충돌은 빨간 테두리.
    // 색은 FmStyle 테마 토큰을 쓴다. 고치기는 한 줄 편집기, 긴 글 · 여러 줄 · BLOB 은 값 패널로 넘긴다.
    class CellDelegate final : public QStyledItemDelegate
    {
        Q_OBJECT
    public:
        explicit CellDelegate(QObject* Parent = nullptr);
        // 설정 › SQLite › 글꼴: 결과 표 글꼴과 ‘숫자 · 16진 값은 쿼리 편집기 글꼴로’.
        void SetFonts(const QFont& Grid, const QFont& Mono, bool MonoValues);
        void SetDisplay(const CellDisplay& Display) { display_ = Display; }
        const CellDisplay& Display() const { return display_; }
        void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override;
        QSize sizeHint(const QStyleOptionViewItem& Option, const QModelIndex& Index) const override;
        QWidget* createEditor(QWidget* Parent, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override;
        void setEditorData(QWidget* Editor, const QModelIndex& Index) const override;
        void setModelData(QWidget* Editor, QAbstractItemModel* Model, const QModelIndex& Index) const override;
        // 셀에서 고치기 어려운 값(긴 글 · 여러 줄 · BLOB).
        bool NeedsPanel(const QModelIndex& Index) const;
    signals:
        // 셀 편집 대신 값 패널을 열어 달라.
        void PanelEditRequested(const QModelIndex& Index) const;
    private:
        QFont grid_, mono_;
        bool hasGrid_ = false, monoValues_ = false;
        CellDisplay display_;
    };
}
