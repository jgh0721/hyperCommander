#pragma once
#include "zipFixture.hpp"
namespace hc::compare::test
{
    inline QByteArray Docx(const QByteArray& Body, const QByteArray& Extra = {}, bool Strict = false, const QByteArray& Target = "word/document.xml")
    {
        const QByteArray ns = Strict ? "http://purl.oclc.org/ooxml/wordprocessingml/main" : "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
        const auto document = "<w:document xmlns:w=\"" + ns + "\"><w:body>" + Body + "</w:body></w:document>";
        const auto types = "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/></Types>";
        const auto rel = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"" + Target + "\"/></Relationships>";
        return StoredZip({{"[Content_Types].xml", types}, {"_rels/.rels", rel}, {"word/document.xml", document}, {"word/styles.xml", Extra}});
    }
    inline QByteArray Paragraph(const QString& Text) { return "<w:p><w:r><w:t>" + Text.toHtmlEscaped().toUtf8() + "</w:t></w:r></w:p>"; }
}
