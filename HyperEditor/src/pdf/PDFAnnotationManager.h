#pragma once

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QString>
#include <QVector>

#include <fpdfview.h>
#include <fpdf_annot.h>

class PDFAnnotationManager
{
public:
    struct PdfRect
    {
        qreal left = 0.0;
        qreal right = 0.0;
        qreal bottom = 0.0;
        qreal top = 0.0;

        bool isValid() const { return right > left && top > bottom; }
    };

    explicit PDFAnnotationManager(FPDF_DOCUMENT document = nullptr);

    void setDocument(FPDF_DOCUMENT document);
    FPDF_DOCUMENT document() const;

    bool addTextMarkup(int pageIndex, FPDF_ANNOTATION_SUBTYPE subtype, const PdfRect& rect, const QColor& color, qreal thickness);
    bool addInkStroke(int pageIndex, const QVector<QPointF>& points, const QColor& color, qreal thickness);
    bool addFreeText(int pageIndex, const PdfRect& rect, const QString& text, const QFont& font, qreal fontSize, const QColor& color);
    bool addTextNote(int pageIndex, const QPointF& pdfPoint, const QString& text, const QColor& color);

private:
    static FS_RECTF toFsRect(const PdfRect& rect);
    static FS_QUADPOINTSF toQuadPoints(const PdfRect& rect);
    static PdfRect normalizedRect(const PdfRect& rect);
    static PdfRect expandedRect(const PdfRect& rect, qreal amount);
    static QString escapeAppearanceText(const QString& text);
    static bool setStringValue(FPDF_ANNOTATION annot, const char* key, const QString& value);
    static bool setAppearance(FPDF_ANNOTATION annot, const QString& appearance);

    FPDF_DOCUMENT m_document = nullptr;
};

