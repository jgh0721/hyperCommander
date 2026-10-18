#include "pdf/PDFFormManager.h"

#include <fpdfview.h>
#include <fpdf_formfill.h>
#include <fpdf_annot.h>
#include <fpdf_doc.h>

// ── FormFillInfo 내부 데이터 ──
struct PDFFormManager::FormFillInfoData
{
    FPDF_FORMFILLINFO info{};
};

PDFFormManager::PDFFormManager(QObject* parent)
    : QObject(parent)
{}

PDFFormManager::~PDFFormManager()
{
    closeForm();
}

// ═══════════════════════════════════════════════════════════
// 초기화 / 종료
// ═══════════════════════════════════════════════════════════
void PDFFormManager::initForm(FPDF_DOCUMENT doc)
{
    closeForm();
    m_document = doc;
    if (!m_document) return;

    // NOTE:
    // PDFium의 form fill environment는 FFI_Invalidate, FFI_GetPage,
    // FFI_GetCurrentPage, FFI_SetCursor, FFI_SetTimer 등 필수 embedder
    // 콜백을 올바르게 제공해야만 안전하게 동작한다.
    // 현재 구현은 해당 콜백을 아직 제공하지 않으므로 환경을 초기화하면
    // 문서 로드/페이지 접근 중 PDFium 내부에서 잘못된 함수 포인터를 따라가며
    // 메모리 손상이나 접근 위반이 발생할 수 있다.
    // 따라서 formHandle 생성은 보류하고, 정적 렌더링만 수행한다.
    m_formHandle = nullptr;
}

void PDFFormManager::closeForm()
{
    if (m_formHandle) {
        FPDFDOC_ExitFormFillEnvironment(m_formHandle);
        m_formHandle = nullptr;
    }
    delete m_fillInfo;
    m_fillInfo = nullptr;
    m_document = nullptr;
}

FPDF_FORMHANDLE PDFFormManager::handle() const { return m_formHandle; }

// ═══════════════════════════════════════════════════════════
// 필드 분류 (권장사항: 타입별 지원/읽기전용/미지원)
// ═══════════════════════════════════════════════════════════
PDFFormManager::FieldSupport PDFFormManager::classifyField(int fieldType)
{
    switch (fieldType) {
        case FPDF_FORMFIELD_TEXTFIELD:
        case FPDF_FORMFIELD_CHECKBOX:
        case FPDF_FORMFIELD_RADIOBUTTON:
        case FPDF_FORMFIELD_COMBOBOX:
        case FPDF_FORMFIELD_LISTBOX:
            return FieldSupport::Interactive;

        case FPDF_FORMFIELD_PUSHBUTTON:
            return FieldSupport::ReadOnly;

        case FPDF_FORMFIELD_SIGNATURE:
#ifdef FPDF_FORMFIELD_XFA
        case FPDF_FORMFIELD_XFA:
#endif
            return FieldSupport::Unsupported;

        default:
            return FieldSupport::ReadOnly;
    }
}

// ═══════════════════════════════════════════════════════════
// 이벤트 전달
// ═══════════════════════════════════════════════════════════
bool PDFFormManager::handleMousePress(FPDF_PAGE page, const QPointF& pos)
{
    if (!m_formHandle || !page) return false;

    int fieldType = FPDFPage_HasFormFieldAtPoint(m_formHandle, page, pos.x(), pos.y());
    if (fieldType < 0) return false;

    auto support = classifyField(fieldType);
    if (support == FieldSupport::Interactive) {
        FORM_OnLButtonDown(m_formHandle, page, 0, pos.x(), pos.y());
        return true;
    }
    if (support == FieldSupport::Unsupported) {
        FieldInfo fi;
        fi.type = fieldType;
        fi.support = support;
        emit unsupportedFieldClicked(fi);
    }
    return false;
}

bool PDFFormManager::handleMouseRelease(FPDF_PAGE page, const QPointF& pos)
{
    if (!m_formHandle || !page) return false;
    int fieldType = FPDFPage_HasFormFieldAtPoint(m_formHandle, page, pos.x(), pos.y());
    if (fieldType < 0) return false;
    if (classifyField(fieldType) == FieldSupport::Interactive) {
        FORM_OnLButtonUp(m_formHandle, page, 0, pos.x(), pos.y());
        return true;
    }
    return false;
}

bool PDFFormManager::handleMouseMove(FPDF_PAGE page, const QPointF& pos)
{
    if (!m_formHandle || !page) return false;
    FORM_OnMouseMove(m_formHandle, page, 0, pos.x(), pos.y());
    return true;
}

bool PDFFormManager::handleChar(FPDF_PAGE page, QChar ch)
{
    if (!m_formHandle || !page) return false;
    FORM_OnChar(m_formHandle, page, ch.unicode(), 0);
    return true;
}

bool PDFFormManager::handleKeyDown(FPDF_PAGE page, int key, int modifiers)
{
    if (!m_formHandle || !page) return false;
    FORM_OnKeyDown(m_formHandle, page, key, modifiers);
    return true;
}

// ═══════════════════════════════════════════════════════════
// 필드 정보 조회
// ═══════════════════════════════════════════════════════════
QVector<PDFFormManager::FieldInfo> PDFFormManager::getFormFields(FPDF_PAGE page) const
{
    QVector<FieldInfo> fields;
    if (!m_formHandle || !page) return fields;

    int annotCount = FPDFPage_GetAnnotCount(page);
    for (int i = 0; i < annotCount; ++i) {
        FPDF_ANNOTATION annot = FPDFPage_GetAnnot(page, i);
        if (!annot) continue;

        int subtype = FPDFAnnot_GetSubtype(annot);
        if (subtype == FPDF_ANNOT_WIDGET) {
            FieldInfo fi;
            fi.type    = FPDFAnnot_GetFormFieldType(m_formHandle, annot);
            fi.support = classifyField(fi.type);

            // 이름
            unsigned long nameLen = FPDFAnnot_GetFormFieldName(m_formHandle, annot, nullptr, 0);
            if (nameLen > 0) {
                QVector<char16_t> buf(nameLen / 2 + 1, 0);
                FPDFAnnot_GetFormFieldName(m_formHandle, annot,
                                           reinterpret_cast<FPDF_WCHAR*>(buf.data()), nameLen);
                fi.name = QString::fromUtf16(buf.data());
            }

            // 값
            unsigned long valLen = FPDFAnnot_GetFormFieldValue(m_formHandle, annot, nullptr, 0);
            if (valLen > 0) {
                QVector<char16_t> buf(valLen / 2 + 1, 0);
                FPDFAnnot_GetFormFieldValue(m_formHandle, annot,
                                            reinterpret_cast<FPDF_WCHAR*>(buf.data()), valLen);
                fi.value = QString::fromUtf16(buf.data());
            }

            // 영역
            FS_RECTF rect;
            if (FPDFAnnot_GetRect(annot, &rect))
                fi.rect = QRectF(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);

            int flags = FPDFAnnot_GetFormFieldFlags(m_formHandle, annot);
            fi.pdfReadOnly = (flags & FPDF_FORMFLAG_READONLY);

            fields.append(fi);
        }
        FPDFPage_CloseAnnot(annot);
    }
    return fields;
}

bool PDFFormManager::hasInteractiveFields(FPDF_PAGE page) const
{
    auto fields = getFormFields(page);
    for (const auto& f : fields) {
        if (f.support == FieldSupport::Interactive && !f.pdfReadOnly)
            return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════
// 폼 데이터 내보내기 / 가져오기
// ═══════════════════════════════════════════════════════════
QVariantMap PDFFormManager::exportFormData() const
{
    // NOTE: 전체 페이지를 순회하여 필드 데이터 수집 — 간략 구현
    QVariantMap data;
    // 실제 구현에서는 FPDF_GetPageCount + 각 페이지 순회
    return data;
}

void PDFFormManager::importFormData(const QVariantMap& /*data*/)
{
    // TODO: 필드명으로 매치하여 값 설정
}

