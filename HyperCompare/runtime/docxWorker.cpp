#include "stdafx.h"
#include "zipArchive.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QXmlStreamReader>

namespace hc::compare
{
    namespace
    {
        const QString wordNs = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
        const QString strictWordNs = "http://purl.oclc.org/ooxml/wordprocessingml/main";
        const QString relNs = "http://schemas.openxmlformats.org/package/2006/relationships";
        const QString typesNs = "http://schemas.openxmlformats.org/package/2006/content-types";
        class Xml final
        {
        public:
            Xml(const QByteArray& Bytes, const Budget& Limits) : reader_(Bytes), limits_(Limits) { reader_.setEntityExpansionLimit(64); }
            bool Next()
            {
                limits_.Check(); if (reader_.atEnd()) { if (reader_.hasError()) throw Failure("DocxInvalidXml"); return false; }
                reader_.readNext(); if (++tokens_ > 1000000) throw Failure("DocxXmlLimit");
                if (reader_.isDTD() || reader_.isEntityReference()) throw Failure("DocxDtdUnsupported");
                if (reader_.isStartElement() && ++depth_ > 256) throw Failure("DocxXmlDepthLimit");
                if (reader_.isEndElement()) --depth_;
                return true;
            }
            QXmlStreamReader reader_; int depth_ = 0;
        private:
            const Budget& limits_; int tokens_ = 0;
        };
        QString partPath(QString Path)
        {
            if (Path.startsWith('/')) Path.remove(0, 1);
            if (Path.isEmpty() || Path.contains('\\') || Path.contains(':') || Path.contains('%') || Path.contains('?') || Path.contains('#')) throw Failure("DocxPartPathUnsupported");
            for (const auto& component : Path.split('/')) if (component.isEmpty() || component == "." || component == "..") throw Failure("DocxPartPathUnsupported");
            return Path;
        }
        QByteArray readPart(ZipArchive& Archive, const QString& Path, const Budget& Limits, int* Ordinal = nullptr)
        {
            for (const auto& entry : Archive.Entries()) if (entry.path_ == Path && !entry.directory_)
            { if (Ordinal) *Ordinal = entry.ordinal_; return Archive.Read(entry.ordinal_, Limits); }
            throw Failure("DocxMissingPart");
        }
        QString mainPart(const QByteArray& Bytes, const Budget& Limits)
        {
            Xml xml(Bytes, Limits); QString path; bool root = false;
            while (xml.Next())
            {
                const auto& r = xml.reader_; if (!r.isStartElement()) continue;
                if (xml.depth_ == 1) { if (r.name() != u"Relationships" || r.namespaceUri() != relNs) throw Failure("DocxInvalidRelationships"); root = true; }
                if (xml.depth_ != 2 || r.name() != u"Relationship" || r.namespaceUri() != relNs) continue;
                const auto type = r.attributes().value("Type");
                if (type != u"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" && type != u"http://purl.oclc.org/ooxml/officeDocument/relationships/officeDocument") continue;
                if (!path.isEmpty()) throw Failure("DocxAmbiguousMainPart");
                const auto mode = r.attributes().value("TargetMode"); if (!mode.isEmpty() && mode != u"Internal") throw Failure("DocxExternalMainPart");
                path = partPath(r.attributes().value("Target").toString());
            }
            if (!root || path.isEmpty()) throw Failure("DocxMissingMainRelationship"); return path;
        }
        void validateType(const QByteArray& Bytes, const QString& Part, const Budget& Limits)
        {
            Xml xml(Bytes, Limits); bool found = false;
            while (xml.Next())
            {
                const auto& r = xml.reader_; if (!r.isStartElement()) continue;
                if (xml.depth_ == 1 && (r.name() != u"Types" || r.namespaceUri() != typesNs)) throw Failure("DocxInvalidContentTypes");
                if (xml.depth_ != 2 || r.name() != u"Override" || r.namespaceUri() != typesNs || r.attributes().value("PartName") != '/' + Part) continue;
                if (found || r.attributes().value("ContentType") != u"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml") throw Failure("DocxMainTypeUnsupported");
                found = true;
            }
            if (!found) throw Failure("DocxMainTypeUnsupported");
        }
        QJsonArray paragraphs(const QByteArray& Bytes, const QString& Part, const QJsonObject& EntryLocation, const Budget& Limits)
        {
            Xml xml(Bytes, Limits); QJsonArray result; QString text, ns; int bodyDepth = 0, paraDepth = 0, textDepth = 0, skipDepth = 0, textUnits = 0; bool sawBody = false;
            auto append = [&](QStringView Value) { textUnits += Value.size(); if (textUnits > 4 * 1024 * 1024 || text.size() + Value.size() > 256 * 1024) throw Failure("DocxTextLimit"); text += Value; };
            while (xml.Next())
            {
                const auto& r = xml.reader_;
                if (r.isStartElement())
                {
                    if (xml.depth_ == 1)
                    {
                        ns = r.namespaceUri().toString(); if (r.name() != u"document" || (ns != wordNs && ns != strictWordNs)) throw Failure("DocxInvalidDocument");
                    }
                    if (xml.depth_ == 2 && r.name() == u"body" && r.namespaceUri() == ns)
                    { if (sawBody) throw Failure("DocxInvalidDocument"); sawBody = true; bodyDepth = xml.depth_; }
                    if (!bodyDepth || skipDepth) continue;
                    // Text boxes, deletions, alternate content and fields are outside this projection.
                    if ((r.namespaceUri() == ns && (r.name() == u"del" || r.name() == u"moveFrom" || r.name() == u"drawing" || r.name() == u"pict" || r.name() == u"txbxContent")) ||
                        r.namespaceUri() == u"http://schemas.openxmlformats.org/markup-compatibility/2006") { skipDepth = xml.depth_; continue; }
                    if (r.namespaceUri() != ns) continue;
                    if (r.name() == u"p") { if (paraDepth) throw Failure("DocxNestedParagraphUnsupported"); paraDepth = xml.depth_; text.clear(); }
                    if (paraDepth && r.name() == u"t") textDepth = xml.depth_;
                    if (paraDepth && r.name() == u"tab") append(u"\t");
                    if (paraDepth && (r.name() == u"br" || r.name() == u"cr")) append(u"\n");
                }
                else if (r.isCharacters() && textDepth && !skipDepth) append(r.text());
                else if (r.isEndElement())
                {
                    if (skipDepth) { if (xml.depth_ < skipDepth) skipDepth = 0; continue; }
                    if (textDepth && xml.depth_ < textDepth) textDepth = 0;
                    if (paraDepth && xml.depth_ < paraDepth)
                    {
                        if (result.size() >= 50000) throw Failure("DocxParagraphLimit");
                        auto location = EntryLocation; location["schema"] = "office.paragraph.v1";
                        auto payload = location["payload"].toObject(); payload["part"] = Part; payload["paragraph"] = result.size(); payload["projection"] = "main-body-text-v1"; location["payload"] = payload;
                        result.append(QJsonObject{{"text", text}, {"ordinal", result.size()}, {"location", location}}); paraDepth = 0;
                    }
                    if (bodyDepth && xml.depth_ < bodyDepth) bodyDepth = 0;
                }
            }
            if (!sawBody) throw Failure("DocxMissingBody"); return result;
        }
        std::string paragraphKeys(const QJsonArray& Paragraphs)
        {
            std::string keys; for (const auto& p : Paragraphs) { keys += QCryptographicHash::hash(p.toObject()["text"].toString().toUtf8(), QCryptographicHash::Sha256).toHex().toStdString(); keys += '\n'; } return keys;
        }
    }
    QJsonObject DocxWorkerRequest(const QJsonObject& Payload)
    {
        // Verifies CRC and content hash of every part, even when it is not interpreted as Word text.
        auto result = ArchiveWorkerRequest(Payload); const auto resources = Payload["resources"].toArray(); auto documents = result["documents"].toArray();
        Budget limits; limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (int i = 0; i < resources.size(); ++i)
        {
            ZipArchive archive(QByteArray::fromBase64(resources[i].toString().toLatin1()), limits);
            const auto path = mainPart(readPart(archive, "_rels/.rels", limits), limits);
            validateType(readPart(archive, "[Content_Types].xml", limits), path, limits);
            int ordinal = -1; const auto xml = readPart(archive, path, limits, &ordinal);
            auto document = documents[i].toObject(); document["mainPart"] = path;
            document["paragraphs"] = paragraphs(xml, path, archive.Location(ordinal, QString::fromLatin1(QCryptographicHash::hash(xml, QCryptographicHash::Sha256).toHex())), limits); documents[i] = document;
        }
        QJsonArray changes;
        if (documents.size() == 2)
        {
            const auto left = paragraphKeys(documents[0].toObject()["paragraphs"].toArray()), right = paragraphKeys(documents[1].toObject()["paragraphs"].toArray());
            for (const auto& edit : Diff(left, right, limits)) changes.append(QJsonObject{{"begin", qint64(edit.begin_)}, {"end", qint64(edit.end_)}, {"otherBegin", qint64(edit.otherBegin_)}, {"otherEnd", qint64(edit.otherEnd_)}});
        }
        result["payloadSchema"] = "office.paragraphs.v1"; result["documents"] = documents; result["changes"] = changes; result["textEqual"] = changes.isEmpty(); result["scopeComplete"] = false;
        result["coverage"] = QJsonObject{{"projection", "main-body-text-v1"}, {"packageParts", "all-bytes-verified"},
            {"excluded", QJsonArray{"layout", "styles", "headers-footers", "notes-comments", "images-textboxes", "deleted-revisions", "alternate-content", "field-evaluation"}}}; return result;
    }
}
