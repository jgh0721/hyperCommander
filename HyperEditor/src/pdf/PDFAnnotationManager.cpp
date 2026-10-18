#include "pdf/PDFAnnotationManager.h"

#include <QDateTime>
#include <QPainterPath>

PDFAnnotationManager::PDFAnnotationManager(FPDF_DOCUMENT document)
    : m_document(document)
{
}

void PDFAnnotationManager::setDocument(FPDF_DOCUMENT document)
{
    m_document = document;
}

FPDF_DOCUMENT PDFAnnotationManager::document() const
{
    return m_document;
}

bool PDFAnnotationManager::addTextMarkup(int pageIndex,
                                         FPDF_ANNOTATION_SUBTYPE subtype,
                                         const PdfRect& rect,
                                         const QColor& color,
                                         qreal thickness)
{
    if (!m_document || pageIndex < 0)
        return false;

    if (subtype != FPDF_ANNOT_HIGHLIGHT
        && subtype != FPDF_ANNOT_UNDERLINE
        && subtype != FPDF_ANNOT_STRIKEOUT
        && subtype != FPDF_ANNOT_SQUIGGLY) {
        return false;
    }

    if (!FPDFAnnot_IsSupportedSubtype(subtype))
        return false;

    const PdfRect baseRect = expandedRect(normalizedRect(rect), qMax<qreal>(0.0, thickness) * 0.25);
    if (!baseRect.isValid())
        return false;

    FPDF_PAGE page = FPDF_LoadPage(m_document, pageIndex);
    if (!page)
        return false;

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, subtype);
    if (!annot) {
        FPDF_ClosePage(page);
        return false;
    }

    const PdfRect annotRect = expandedRect(baseRect, qMax<qreal>(2.0, thickness));
    const FS_RECTF fsRect = toFsRect(annotRect);
    const FS_QUADPOINTSF quadPoints = toQuadPoints(baseRect);

    bool ok = FPDFAnnot_SetRect(annot, &fsRect);
    ok = FPDFAnnot_AppendAttachmentPoints(annot, &quadPoints) && ok;
    ok = FPDFAnnot_SetColor(annot,
                            FPDFANNOT_COLORTYPE_Color,
                            color.red(),
                            color.green(),
                            color.blue(),
                            qBound(32, color.alpha(), 255)) && ok;
    ok = FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT) && ok;

    const auto Index=FPDFPage_GetAnnotIndex(page,annot);
    FPDFPage_CloseAnnot(annot);
    if(!ok&&Index>=0)FPDFPage_RemoveAnnot(page,Index);
    FPDF_ClosePage(page);
    return ok;
}

bool PDFAnnotationManager::addInkStroke(int pageIndex,
                                        const QVector<QPointF>& points,
                                        const QColor& color,
                                        qreal thickness)
{
    if (!m_document || pageIndex < 0 || points.size() < 2 || !FPDFAnnot_IsSupportedSubtype(FPDF_ANNOT_INK))
        return false;

    FPDF_PAGE page = FPDF_LoadPage(m_document, pageIndex);
    if (!page)
        return false;

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_INK);
    if (!annot) {
        FPDF_ClosePage(page);
        return false;
    }

    QVector<FS_POINTF> strokePoints;
    strokePoints.reserve(points.size());

    qreal left = points.first().x();
    qreal right = points.first().x();
    qreal bottom = points.first().y();
    qreal top = points.first().y();
    for (const QPointF& point : points) {
        strokePoints.append(FS_POINTF{static_cast<float>(point.x()), static_cast<float>(point.y())});
        left = qMin(left, point.x());
        right = qMax(right, point.x());
        bottom = qMin(bottom, point.y());
        top = qMax(top, point.y());
    }

    const qreal effectiveThickness = qBound<qreal>(0.5, thickness, 64.0);
    const PdfRect bounds = expandedRect({left, right, bottom, top}, effectiveThickness * 2.0);
    const FS_RECTF fsRect = toFsRect(bounds);

    bool ok = FPDFAnnot_SetRect(annot, &fsRect);
    ok = FPDFAnnot_SetBorder(annot, 0.0f, 0.0f, static_cast<float>(effectiveThickness)) && ok;
    ok = FPDFAnnot_SetColor(annot,
                            FPDFANNOT_COLORTYPE_Color,
                            color.red(),
                            color.green(),
                            color.blue(),
                            qBound(32, color.alpha(), 255)) && ok;
    ok = FPDFAnnot_AddInkStroke(annot,
                                strokePoints.constData(),
                                static_cast<size_t>(strokePoints.size())) >= 0 && ok;
    ok = FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT) && ok;

    const auto Index=FPDFPage_GetAnnotIndex(page,annot);
    FPDFPage_CloseAnnot(annot);
    if(!ok&&Index>=0)FPDFPage_RemoveAnnot(page,Index);
    FPDF_ClosePage(page);
    return ok;
}

bool PDFAnnotationManager::addFreeText(int pageIndex,
                                       const PdfRect& rect,
                                       const QString& text,
                                       const QFont& font,
                                       qreal fontSize,
                                       const QColor& color)
{
    if (!m_document || pageIndex < 0 || text.trimmed().isEmpty() || !FPDFAnnot_IsSupportedSubtype(FPDF_ANNOT_FREETEXT))
        return false;

    const PdfRect normalized = normalizedRect(rect);
    if (!normalized.isValid())
        return false;

    FPDF_PAGE page = FPDF_LoadPage(m_document, pageIndex);
    if (!page)
        return false;

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_FREETEXT);
    if (!annot) {
        FPDF_ClosePage(page);
        return false;
    }

    const FS_RECTF fsRect = toFsRect(normalized);
    const qreal effectiveFontSize = qBound<qreal>(4.0, fontSize, 96.0);
    const qreal width = qMax<qreal>(1.0, normalized.right - normalized.left);
    const qreal height = qMax<qreal>(1.0, normalized.top - normalized.bottom);
    const qreal baseline = qBound<qreal>(effectiveFontSize, height - effectiveFontSize * 1.25, height - 4.0);
    const QString escapedText = escapeAppearanceText(text.simplified());
    const qreal r = color.redF();
    const qreal g = color.greenF();
    const qreal b = color.blueF();
    const QString baseFont = font.family().contains(QStringLiteral("Courier"), Qt::CaseInsensitive)
        ? QStringLiteral("Cour")
        : QStringLiteral("Helv");
    // Standard Helvetica appearances cannot display Korean/Unicode text.
    // Shape with Qt's font fallback and store vector outlines while retaining
    // the exact Unicode Contents string for annotation tools and round trips.
    QFont outlineFont=font;outlineFont.setPixelSize(qRound(effectiveFontSize));
    QPainterPath glyphs;glyphs.addText(QPointF(4,effectiveFontSize),outlineFont,text);
    QString appearance=QStringLiteral("q %1 %2 %3 rg ").arg(r,0,'f',4).arg(g,0,'f',4).arg(b,0,'f',4);
    for(const auto& polygon:glyphs.toSubpathPolygons()) {
        bool first=true;for(const auto& point:polygon) {
            appearance+=QStringLiteral("%1 %2 %3 ").arg(point.x(),0,'f',4).arg(height-point.y(),0,'f',4).arg(first?"m":"l");first=false;
        }appearance+="h ";
    }appearance+="f* Q";
    Q_UNUSED(width);Q_UNUSED(baseline);Q_UNUSED(escapedText);

    bool ok = FPDFAnnot_SetRect(annot, &fsRect);
    ok = FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, color.red(), color.green(), color.blue(), 255) && ok;
    ok = FPDFAnnot_SetBorder(annot, 0.0f, 0.0f, 1.0f) && ok;
    ok = setStringValue(annot, "Contents", text) && ok;
    ok = setStringValue(annot, "DA", QStringLiteral("/%1 %2 Tf %3 %4 %5 rg")
                         .arg(baseFont)
                         .arg(effectiveFontSize, 0, 'f', 2)
                         .arg(r, 0, 'f', 4)
                         .arg(g, 0, 'f', 4)
                         .arg(b, 0, 'f', 4)) && ok;
    ok = setAppearance(annot, appearance) && ok;
    ok = FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT) && ok;
    Q_UNUSED(width);

    const auto Index=FPDFPage_GetAnnotIndex(page,annot);
    FPDFPage_CloseAnnot(annot);
    if(!ok&&Index>=0)FPDFPage_RemoveAnnot(page,Index);
    FPDF_ClosePage(page);
    return ok;
}

bool PDFAnnotationManager::addTextNote(int pageIndex,
                                       const QPointF& pdfPoint,
                                       const QString& text,
                                       const QColor& color)
{
    if (!m_document || pageIndex < 0 || text.trimmed().isEmpty() || !FPDFAnnot_IsSupportedSubtype(FPDF_ANNOT_TEXT))
        return false;

    FPDF_PAGE page = FPDF_LoadPage(m_document, pageIndex);
    if (!page)
        return false;

    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page, FPDF_ANNOT_TEXT);
    if (!annot) {
        FPDF_ClosePage(page);
        return false;
    }

    const qreal size = 24.0;
    const PdfRect rect{pdfPoint.x(), pdfPoint.x() + size, pdfPoint.y() - size, pdfPoint.y()};
    const FS_RECTF fsRect = toFsRect(rect);

    bool ok = FPDFAnnot_SetRect(annot, &fsRect);
    ok = FPDFAnnot_SetColor(annot,
                            FPDFANNOT_COLORTYPE_Color,
                            color.red(),
                            color.green(),
                            color.blue(),
                            255) && ok;
    ok = setStringValue(annot, "Contents", text) && ok;
    ok = setStringValue(annot, "Name", QStringLiteral("Comment")) && ok;
    ok = setStringValue(annot, "M", QDateTime::currentDateTime().toString(Qt::ISODate)) && ok;
    ok = FPDFAnnot_SetFlags(annot, FPDF_ANNOT_FLAG_PRINT) && ok;

    const auto Index=FPDFPage_GetAnnotIndex(page,annot);
    FPDFPage_CloseAnnot(annot);
    if(!ok&&Index>=0)FPDFPage_RemoveAnnot(page,Index);
    FPDF_ClosePage(page);
    return ok;
}

FS_RECTF PDFAnnotationManager::toFsRect(const PdfRect& rect)
{
    return FS_RECTF{static_cast<float>(rect.left),
                    static_cast<float>(rect.top),
                    static_cast<float>(rect.right),
                    static_cast<float>(rect.bottom)};
}

FS_QUADPOINTSF PDFAnnotationManager::toQuadPoints(const PdfRect& rect)
{
    return FS_QUADPOINTSF{static_cast<float>(rect.left),
                          static_cast<float>(rect.top),
                          static_cast<float>(rect.right),
                          static_cast<float>(rect.top),
                          static_cast<float>(rect.left),
                          static_cast<float>(rect.bottom),
                          static_cast<float>(rect.right),
                          static_cast<float>(rect.bottom)};
}

PDFAnnotationManager::PdfRect PDFAnnotationManager::normalizedRect(const PdfRect& rect)
{
    return {qMin(rect.left, rect.right),
            qMax(rect.left, rect.right),
            qMin(rect.bottom, rect.top),
            qMax(rect.bottom, rect.top)};
}

PDFAnnotationManager::PdfRect PDFAnnotationManager::expandedRect(const PdfRect& rect, qreal amount)
{
    const PdfRect normalized = normalizedRect(rect);
    return {normalized.left - amount,
            normalized.right + amount,
            normalized.bottom - amount,
            normalized.top + amount};
}

QString PDFAnnotationManager::escapeAppearanceText(const QString& text)
{
    QString escaped;
    escaped.reserve(text.size());
    for (const QChar ch : text) {
        if (ch == QLatin1Char('(') || ch == QLatin1Char(')') || ch == QLatin1Char('\\'))
            escaped.append(QLatin1Char('\\'));
        if (ch.unicode() < 0x20 || ch.unicode() > 0x7e)
            escaped.append(QLatin1Char('?'));
        else
            escaped.append(ch);
    }
    return escaped;
}

bool PDFAnnotationManager::setStringValue(FPDF_ANNOTATION annot, const char* key, const QString& value)
{
    if (!annot || !key)
        return false;
    return FPDFAnnot_SetStringValue(annot,
                                    key,
                                    reinterpret_cast<FPDF_WIDESTRING>(value.utf16()));
}

bool PDFAnnotationManager::setAppearance(FPDF_ANNOTATION annot, const QString& appearance)
{
    if (!annot)
        return false;
    return FPDFAnnot_SetAP(annot,
                           FPDF_ANNOT_APPEARANCEMODE_NORMAL,
                           reinterpret_cast<FPDF_WIDESTRING>(appearance.utf16()));
}



