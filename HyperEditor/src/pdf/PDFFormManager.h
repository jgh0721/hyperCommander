#pragma once

#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QVariantMap>
#include <QVector>

#include <fpdfview.h>
#include <fpdf_formfill.h>

/// PDFium 폼 필드 상호작용 관리
class PDFFormManager : public QObject
{
    Q_OBJECT

public:
    /// 폼 필드 지원 수준
    enum class FieldSupport { Interactive, ReadOnly, Unsupported };

    /// 폼 필드 정보
    struct FieldInfo
    {
        int             type        = -1;
        FieldSupport    support     = FieldSupport::ReadOnly;
        QString         name;
        QString         value;
        QRectF          rect;           // 페이지 좌표계
        bool            pdfReadOnly = false;
    };

    explicit PDFFormManager(QObject* parent = nullptr);
    ~PDFFormManager() override;

    // 초기화/종료
    void initForm(FPDF_DOCUMENT doc);
    void closeForm();
    FPDF_FORMHANDLE handle() const;

    // 이벤트 전달
    bool handleMousePress(FPDF_PAGE page, const QPointF& pagePos);
    bool handleMouseRelease(FPDF_PAGE page, const QPointF& pagePos);
    bool handleMouseMove(FPDF_PAGE page, const QPointF& pagePos);
    bool handleChar(FPDF_PAGE page, QChar ch);
    bool handleKeyDown(FPDF_PAGE page, int key, int modifiers);

    // 필드 정보
    QVector<FieldInfo> getFormFields(FPDF_PAGE page) const;
    bool hasInteractiveFields(FPDF_PAGE page) const;

    // 폼 데이터
    QVariantMap exportFormData() const;
    void importFormData(const QVariantMap& data);

    /// 필드 타입 → 지원 수준 분류
    static FieldSupport classifyField(int fieldType);

signals:
    void fieldFocused(const FieldInfo& field);
    void fieldChanged(const QString& name, const QString& value);
    void unsupportedFieldClicked(const FieldInfo& field);

private:
    FPDF_FORMHANDLE       m_formHandle = nullptr;
    FPDF_DOCUMENT         m_document   = nullptr;
    // FPDF_FORMFILLINFO는 formHandle 수명 동안 유효해야 하므로 여기서 소유
    struct FormFillInfoData;
    FormFillInfoData*     m_fillInfo   = nullptr;
};

