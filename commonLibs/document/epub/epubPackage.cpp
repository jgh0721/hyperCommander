#include "stdafx.h"
#include "epubPackage.hpp"
#include "epubArchive.hpp"
#include <QSize>

/// Framework headers
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <new>
#include <algorithm>
#include <algorithm>

namespace hc::document::epub
{
    namespace
    {
        using namespace detail;
        const QString Opf = "http://www.idpf.org/2007/opf";
        const QString Xhtml = "http://www.w3.org/1999/xhtml";
        const QString Ncx = "http://www.daisy.org/z3986/2005/ncx/";
        const QString Ocf = "urn:oasis:names:tc:opendocument:xmlns:container";
        QString key(const QString& Ns, const QString& Name) { return Ns.isEmpty() ? Name : "{" + Ns + "}" + Name; }
        QString digest(const QByteArray& Bytes) { return QString::fromLatin1(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex()); }
        QStringList tokens(const QString& Value) { return Value.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts); }

        QString resolve(const QString& Base, const QString& Href)
        {
            if(Href.isEmpty() || QRegularExpression("%(?![0-9a-fA-F]{2})").match(Href).hasMatch()) Reject("invalid-resource-url", Href);
            const QUrl Url(Href, QUrl::StrictMode);
            if(!Url.isValid()) Reject("invalid-resource-url", Href);
            if(!Url.scheme().isEmpty() || !Url.authority().isEmpty() || Url.hasQuery() || Href.startsWith("//")) Reject("external-resource-url", Href);
            const auto Encoded = Href.section('#', 0, 0).toUtf8();
            const auto Path = Utf8(QByteArray::fromPercentEncoding(Encoded), Href);
            if(Path.startsWith('/') || Path.contains('\\') || Path.contains(':') || Path.contains(QChar(0))) Reject("unsafe-resource-path", Href);
            if(Path.isEmpty()) return Base;
            auto Parts = Base.split('/'); Parts.removeLast();
            for(const auto& Part : Path.split('/')) {
                if(Part.isEmpty() || Part == ".") continue;
                if(Part == "..") {
                    if(Parts.isEmpty()) Reject("unsafe-resource-path", Href);
                    Parts.removeLast();
                } else Parts.append(Part);
            }
            return Canonical(Parts.join('/'));
        }

        struct Node
        {
            QString tag_;
            QMap<QString, QString> attributes_;
            QList<int> children_;
            int end_ = 0;
            int parent_ = -1;
            qsizetype textBegin_ = 0, textEnd_ = 0;
            QString Attr(const QString& Name) const { return attributes_.value(Name); }
        };

        // Flat, bounded XML tree. Text offsets retain mixed-content label order
        // without copying every descendant's text into each ancestor.
        class Xml final
        {
        public:
            Xml(const QByteArray& Bytes, const QString& Path, const Cancellation& Cancel)
            {
                if(Bytes.size() > 2 * 1024 * 1024) Reject("xml-limit", Path);
                const auto Source = Utf8(Bytes, Path);
                if(Source.contains(QChar(0))) Reject("xml-dtd-or-entity", Path);
                QXmlStreamReader Reader(Source);
                XhtmlEntities Entities;
                Reader.setEntityResolver(&Entities);
                QList<int> Stack;
                while(!Reader.atEnd()) {
                    Check(Cancel);
                    const auto Token = Reader.readNext();
                    if(Token == QXmlStreamReader::StartDocument) {
                        const auto Encoding = Reader.documentEncoding().toString().toLower();
                        if(!Encoding.isEmpty() && Encoding != "utf-8" && Encoding != "utf8") Reject("xml-encoding-unsupported", Path);
                    } else if(Token == QXmlStreamReader::DTD) {
                        if(!PassiveDoctype(Reader)) Reject("xml-dtd-or-entity", Path);
                        Entities.enabled_ = Reader.dtdName() == "html";
                    } else if(Token == QXmlStreamReader::EntityReference) Reject("xml-dtd-or-entity", Path);
                    else if(Token == QXmlStreamReader::StartElement) {
                        if(nodes_.size() >= 32768 || Stack.size() >= 64 || Reader.attributes().size() > 256) Reject("xml-complexity-limit", Path);
                        const auto Index = static_cast<int>(nodes_.size());
                        Node Item; Item.tag_ = key(Reader.namespaceUri().toString(), Reader.name().toString());
                        Item.textBegin_ = text_.size(); Item.parent_ = Stack.isEmpty() ? -1 : Stack.last();
                        for(const auto& Attr : Reader.attributes()) Item.attributes_.insert(key(Attr.namespaceUri().toString(), Attr.name().toString()), Attr.value().toString());
                        if(!Stack.isEmpty()) nodes_[Stack.last()].children_.append(Index);
                        else if(!nodes_.isEmpty()) Reject("invalid-xml", Path);
                        nodes_.append(Item); Stack.append(Index);
                    } else if(Token == QXmlStreamReader::Characters) text_ += Reader.text();
                    else if(Token == QXmlStreamReader::EndElement) {
                        if(Stack.isEmpty()) Reject("invalid-xml", Path);
                        auto& Item = nodes_[Stack.takeLast()]; Item.end_ = static_cast<int>(nodes_.size()); Item.textEnd_ = text_.size();
                    }
                }
                if(Reader.hasError() || nodes_.isEmpty() || !Stack.isEmpty()) Reject("invalid-xml", Path);
            }
            const Node& At(int Index) const { return nodes_[Index]; }
            QString Text(int Index) const { const auto& Item = At(Index); return text_.mid(Item.textBegin_, Item.textEnd_ - Item.textBegin_); }
            QList<int> Children(int Parent, const QString& Ns, const QString& Name) const
            {
                QList<int> Result;
                for(const auto Index : At(Parent).children_) if(At(Index).tag_ == key(Ns, Name)) Result.append(Index);
                return Result;
            }
            QList<int> Descendants(int Parent, const QString& Ns, const QString& Name) const
            {
                QList<int> Result;
                for(int Index = Parent + 1; Index < At(Parent).end_; ++Index) if(At(Index).tag_ == key(Ns, Name)) Result.append(Index);
                return Result;
            }
            int One(int Parent, const QString& Ns, const QString& Name, const QString& Error) const
            {
                const auto Matches = Children(Parent, Ns, Name);
                if(Matches.size() != 1) Reject(Error);
                return Matches.first();
            }
            void Passive(const QString& Path) const
            {
                static const QSet<QString> Forbidden{"script", "iframe", "frame", "frameset", "object", "embed", "applet", "form",
                    "base", "foreignobject", "animate", "animatetransform", "animatemotion", "set"};
                for(const auto& Item : nodes_) {
                    const auto Local = Item.tag_.section('}', -1).toLower();
                    static const QRegularExpression Utf8ContentType(QStringLiteral(R"(^\s*(?:text/html|application/xhtml\+xml)\s*;\s*charset\s*=\s*utf-?8\s*$)"), QRegularExpression::CaseInsensitiveOption);
                    const bool EncodingMeta = Local == "meta" && Item.Attr("http-equiv").trimmed().compare("content-type", Qt::CaseInsensitive) == 0
                        && Utf8ContentType.match(Item.Attr("content")).hasMatch();
                    if(Forbidden.contains(Local) || (Local == "meta" && Item.attributes_.contains("http-equiv") && !EncodingMeta)) Reject("renderer-active-markup", Path);
                    for(auto It = Item.attributes_.cbegin(); It != Item.attributes_.cend(); ++It) {
                        const auto Name = It.key().section('}', -1).toLower(), Value = It.value().trimmed().toLower();
                        if(Name.startsWith("on") || It.key() == "{http://www.w3.org/XML/1998/namespace}base") Reject("renderer-active-attribute", Path);
                        if((Name == "href" || Name == "src" || Name == "action") && (Value.startsWith("javascript:") || Value.startsWith("vbscript:") || Value.startsWith("data:text/")))
                            Reject("renderer-active-url", Path);
                    }
                }
            }
        private:
            QList<Node> nodes_;
            QString text_;
        };

        struct Resource { QString id_, path_, media_; QStringList properties_; };

        QJsonObject prepare(const QByteArray& Bytes, const Cancellation& Cancel, Archive& Book, QMap<QString,QByteArray>* Overrides = nullptr)
        {
            if(!Overrides) Book.VerifyAll();
            if(Book.Contains("META-INF/encryption.xml")) Reject("encryption-needs-classification");
            if(Book.Contains("META-INF/signatures.xml")) Reject("signed-publication");
            const Xml Container(Book.Read("META-INF/container.xml"), "META-INF/container.xml", Cancel);
            if(Container.At(0).tag_ != key(Ocf, "container")) Reject("invalid-container-root");
            const auto Roots = Container.One(0, Ocf, "rootfiles", "missing-rootfiles");
            const auto Root = Container.One(Roots, Ocf, "rootfile", "multiple-or-missing-rootfiles");
            if(Container.At(Root).Attr("media-type") != "application/oebps-package+xml") Reject("invalid-rootfile-type");
            const auto Rootfile = Canonical(Container.At(Root).Attr("full-path"));
            const auto PackageBytes = Book.Read(Rootfile);
            const Xml Package(PackageBytes, Rootfile, Cancel);
            if(Package.At(0).tag_ != key(Opf, "package")) Reject("invalid-package-root", Rootfile);
            const auto Version = Package.At(0).Attr("version");
            if(Version != "2.0" && Version != "3.0") Reject("unsupported-package-version", Version);
            const auto Metadata = Package.One(0, Opf, "metadata", "invalid-metadata");
            const auto Titles = Package.Children(Metadata, "http://purl.org/dc/elements/1.1/", "title");
            const auto Title = Titles.isEmpty() ? QString() : Package.Text(Titles.first());
            QJsonArray Languages;
            for(const auto Index : Package.Children(Metadata, "http://purl.org/dc/elements/1.1/", "language")) {
                const auto Language = Package.Text(Index).trimmed();
                if(Language.size() > 128 || Languages.size() >= 32) Reject("language-metadata-limit");
                if(!Language.isEmpty() && !Languages.contains(Language)) Languages.append(Language);
            }
            QString GlobalLayout="reflowable";bool HasLayout=false;
            for(const auto Index : Package.Children(Metadata, Opf, "meta")) {
                const auto& Meta=Package.At(Index);if(Meta.Attr("property")!="rendition:layout")continue;
                const auto Value=Package.Text(Index).trimmed();
                if(HasLayout || !Meta.Attr("refines").isEmpty() || (Value!="reflowable" && Value!="pre-paginated"))Reject("invalid-rendition-layout",Rootfile);
                GlobalLayout=Value;HasLayout=true;
            }
            const auto SpineIndex = Package.One(0, Opf, "spine", "missing-spine");
            const auto NcxId = Package.At(SpineIndex).Attr("toc");
            QSet<QString> SpineIds;
            for(const auto Index : Package.Children(SpineIndex, Opf, "itemref")) SpineIds.insert(Package.At(Index).Attr("idref"));
            QMap<QString, QString> NormalizedMedia;
            static const QSet<QString> Allowed{"application/xhtml+xml", "text/css", "image/svg+xml", "image/png", "image/jpeg", "image/gif", "image/webp",
                "font/woff", "font/woff2", "font/ttf", "font/otf", "application/x-dtbncx+xml"};
            QList<Resource> Resources;
            QMap<QString, int> Ids;
            QSet<QString> Paths;
            const auto Manifest = Package.One(0, Opf, "manifest", "missing-manifest");
            for(const auto Index : Package.Children(Manifest, Opf, "item")) {
                Check(Cancel);
                const auto& Item = Package.At(Index);
                Resource Resource{Item.Attr("id"), resolve(Rootfile, Item.Attr("href")), Item.Attr("media-type"), tokens(Item.Attr("properties"))};
                if(Resource.media_ == "application/x-font-ttf") Resource.media_ = "font/ttf";
                if(Resource.media_ == "application/xhtml+xml" && Resource.path_.endsWith(".css", Qt::CaseInsensitive)
                    && !SpineIds.contains(Resource.id_) && !Resource.properties_.contains("nav")) Resource.media_ = "text/css";
                // A few EPUB2 converters label the designated NCX as XHTML.
                // Normalize only the spine's explicit TOC role and a verified
                // NCX namespace/root; never reinterpret arbitrary resources.
                if(Version == "2.0" && Resource.id_ == NcxId && Resource.media_ == "application/xhtml+xml") {
                    const Xml Toc(Book.Read(Resource.path_), Resource.path_, Cancel);
                    if(Toc.At(0).tag_ != key(Ncx, "ncx")) Reject("invalid-ncx-root", Resource.path_);
                    Resource.media_ = "application/x-dtbncx+xml";
                }
                if(Resource.id_.isEmpty() || Ids.contains(Resource.id_)) Reject("duplicate-or-empty-manifest-id", Resource.id_);
                if(Paths.contains(Resource.path_) || Resource.path_ == Rootfile) Reject("duplicate-manifest-path", Resource.path_);
                if(!Allowed.contains(Resource.media_)) Reject("renderer-media-unsupported", Resource.path_);
                if(Resource.properties_.contains("scripted")) Reject("scripted-content", Resource.path_);

                if(Resource.media_ != Item.Attr("media-type")) NormalizedMedia.insert(Resource.id_, Resource.media_);
                Ids.insert(Resource.id_, static_cast<int>(Resources.size())); Paths.insert(Resource.path_); Resources.append(Resource);
            }
            if(Resources.isEmpty() || Resources.size() >= 4096) Reject("manifest-count-limit");
            if(Overrides) {
                QSet<QString> Fonts;
                for(const auto& Resource:Resources) if(Resource.media_.startsWith("font/")) Fonts.insert(Resource.path_);
                Book.ValidateResourceLimits(Fonts);
            }
            auto Progression = Package.At(SpineIndex).Attr("page-progression-direction");
            if(Progression.isEmpty()) Progression = "default";
            if(Progression != "default" && Progression != "ltr" && Progression != "rtl") Reject("invalid-reading-progression");
            QJsonArray Spine, Navigation, Images, Entries;
            // Bound repeated references before appending to implicitly shared JSON.
            // A small OPF can otherwise amplify a long path thousands of times.
            qint64 MetadataBytes = 1024;
            const auto Account = [&](const QJsonObject& Record) {
                Check(Cancel);
                MetadataBytes += QJsonDocument(Record).toJson(QJsonDocument::Compact).size() + 1;
                if(MetadataBytes > 4 * 1024 * 1024) Reject("snapshot-metadata-limit");
            };
            Account({{"title", Title}, {"rootfile", Rootfile}, {"languages", Languages}, {"readingProgression", Progression}});
            const auto Append = [&](QJsonArray& Array, const QJsonObject& Record) {
                if(Array.size() >= 4096) Reject("snapshot-record-limit");
                Account(Record); Array.append(Record);
            };
            const auto SpineItems = Package.Children(SpineIndex, Opf, "itemref");
            int MissingSpineItems = 0, FixedSpineItems = 0;QMap<QString,QSize> FixedViewports;
            for(const auto Index : SpineItems) if(!Ids.contains(Package.At(Index).Attr("idref"))) ++MissingSpineItems;
            if(MissingSpineItems > 4096) Reject("snapshot-record-limit");
            for(const auto Index : SpineItems) {
                const auto& Item = Package.At(Index); const auto Id = Item.Attr("idref");
                if(!Ids.contains(Id)) continue;
                const auto& Resource = Resources[Ids[Id]];
                if(Resource.media_ != "application/xhtml+xml") Reject("spine-media-unsupported", Resource.path_);
                const auto Properties=tokens(Item.Attr("properties"));
                const bool Fixed=Properties.contains("rendition:layout-pre-paginated"),Reflow=Properties.contains("rendition:layout-reflowable");
                if(Fixed && Reflow)Reject("conflicting-rendition-layout",Resource.path_);
                const auto Layout=Fixed?QString("pre-paginated"):Reflow?QString("reflowable"):GlobalLayout;
                if(Layout=="pre-paginated") {
                    const Xml Document(Book.Read(Resource.path_),Resource.path_,Cancel);int Width=0,Height=0,Count=0;
                    for(const auto Meta:Document.Descendants(0,Xhtml,"meta"))if(Document.At(Meta).Attr("name").compare("viewport",Qt::CaseInsensitive)==0) {
                        ++Count;const auto Viewport=Document.At(Meta).Attr("content");
                        const auto W=QRegularExpression("(?:^|[,;\\s])width\\s*=\\s*([0-9]+)(?=$|[,;\\s])",QRegularExpression::CaseInsensitiveOption).match(Viewport);
                        const auto H=QRegularExpression("(?:^|[,;\\s])height\\s*=\\s*([0-9]+)(?=$|[,;\\s])",QRegularExpression::CaseInsensitiveOption).match(Viewport);
                        Width=W.captured(1).toInt();Height=H.captured(1).toInt();
                        if(Viewport.count(QRegularExpression("(?:^|[,;\\s])width\\s*=",QRegularExpression::CaseInsensitiveOption))!=1 || Viewport.count(QRegularExpression("(?:^|[,;\\s])height\\s*=",QRegularExpression::CaseInsensitiveOption))!=1)Reject("fixed-layout-viewport",Resource.path_);
                    }
                    if(Count!=1 || Width<1 || Height<1 || Width>16384 || Height>16384 || qint64(Width)*Height>64*1024*1024)Reject("fixed-layout-viewport",Resource.path_);
                    ++FixedSpineItems;FixedViewports.insert(Resource.path_,QSize(Width,Height));
                }
                auto Linear = Item.Attr("linear"); if(Linear.isEmpty()) Linear = "yes";
                if(Linear != "yes" && Linear != "no") Reject("invalid-spine-linear", Resource.path_);
                // Recovery retains available source order and makes all of it
                // reachable. Retain the missing count in publication diagnostics.
                QJsonObject Section{{"id",Id},{"path",Resource.path_},{"linear",MissingSpineItems?"yes":Linear},{"layout",Layout}};
                if(Layout=="pre-paginated"){const auto Size=FixedViewports[Resource.path_];Section["viewport"]=QJsonObject{{"width",Size.width()},{"height",Size.height()}};}
                Append(Spine,Section);
            }
            if(Spine.isEmpty()) Reject(MissingSpineItems ? "missing-spine-resource" : "missing-spine");
            QByteArray ReaderPackage = PackageBytes;
            if(MissingSpineItems || !NormalizedMedia.isEmpty()) {
                // Repair only the immutable rendering snapshot. The archive and
                // editor's OPF source remain untouched, with all original bytes.
                ReaderPackage.clear(); QXmlStreamReader Reader(PackageBytes); QXmlStreamWriter Writer(&ReaderPackage);
                int Depth = 0, SpineDepth = -1, ManifestDepth = -1;
                while(!Reader.atEnd()) {
                    Check(Cancel); const auto Token = Reader.readNext();
                    if(Token == QXmlStreamReader::StartElement) {
                        ++Depth;
                        if(Depth == 2 && Reader.namespaceUri() == Opf && Reader.name() == "spine") SpineDepth = Depth;
                        if(Depth == 2 && Reader.namespaceUri() == Opf && Reader.name() == "manifest") ManifestDepth = Depth;
                        if(ManifestDepth > 0 && Depth == ManifestDepth + 1 && Reader.namespaceUri() == Opf && Reader.name() == "item"
                            && NormalizedMedia.contains(Reader.attributes().value("id").toString())) {
                            Writer.writeStartElement(Reader.namespaceUri().toString(), Reader.name().toString());
                            for(const auto& Namespace : Reader.namespaceDeclarations()) Writer.writeNamespace(Namespace.namespaceUri().toString(), Namespace.prefix().toString());
                            for(const auto& Attribute : Reader.attributes()) if(Attribute.name() != "media-type") Writer.writeAttribute(Attribute);
                            Writer.writeAttribute("media-type", NormalizedMedia[Reader.attributes().value("id").toString()]); continue;
                        }
                        if(MissingSpineItems && SpineDepth > 0 && Depth == SpineDepth + 1 && Reader.namespaceUri() == Opf && Reader.name() == "itemref") {
                            if(!Ids.contains(Reader.attributes().value("idref").toString())) { Reader.skipCurrentElement(); --Depth; continue; }
                            Writer.writeStartElement(Reader.namespaceUri().toString(), Reader.name().toString());
                            for(const auto& Namespace : Reader.namespaceDeclarations()) Writer.writeNamespace(Namespace.namespaceUri().toString(), Namespace.prefix().toString());
                            for(const auto& Attribute : Reader.attributes()) if(Attribute.name() != "linear") Writer.writeAttribute(Attribute);
                            Writer.writeAttribute("linear", "yes"); continue;
                        }
                    }
                    Writer.writeCurrentToken(Reader);
                    if(Token == QXmlStreamReader::EndElement) { if(Depth == SpineDepth) SpineDepth = -1; if(Depth == ManifestDepth) ManifestDepth = -1; --Depth; }
                }
                if(Reader.hasError() || Writer.hasError() || ReaderPackage.size() > 2 * 1024 * 1024) Reject("invalid-reader-package");
            }
            const auto AddNavigation = [&](const QString& Base, const QString& Href, const QString& Label, int Depth) {
                const auto Target = resolve(Base, Href);
                if(!Paths.contains(Target)) Reject("missing-navigation-target", Target);
                Append(Navigation, {{"label", Label}, {"path", Target},
                    {"fragment", QUrl(Href).fragment()}, {"depth", Depth}});
            };
            qint64 Total = PackageBytes.size();
            const auto AddResource = [&](const QString& Path, const QString& Media, const QByteArray& Raw) {
                QJsonObject Record{{"path", Path}, {"mediaType", Media}, {"bytes", Overrides && Raw.isNull() ? Book.Size(Path) : Raw.size()}};
                if(!Overrides) { Record["sha256"] = digest(Raw); Record["base64"] = ""; }
                Account(Record);
                if(!Overrides) Record["base64"] = QString::fromLatin1(Raw.toBase64());
                Entries.append(Record);
            };
            for(const auto& Resource : Resources) {
                Check(Cancel);
                const auto Limit = Overrides && Resource.media_.startsWith("font/") ? 32 * 1024 * 1024 : 16 * 1024 * 1024;
                if(Book.Size(Resource.path_) > Limit) Reject("decompressed-size-limit", Resource.path_);
                const bool Markup = Resource.media_ == "application/xhtml+xml" || Resource.media_ == "image/svg+xml";
                const auto Raw = !Overrides || Markup ? Book.Read(Resource.path_) : QByteArray{};
                Total += Book.Size(Resource.path_);
                if(!Overrides && Total > SnapshotLimit) Reject("renderer-snapshot-limit");
                if(Resource.media_ == "application/xhtml+xml" || Resource.media_ == "image/svg+xml") {
                    const Xml Document(Raw, Resource.path_, Cancel); Document.Passive(Resource.path_);
                    const auto ExpectedRoot = Resource.media_ == "application/xhtml+xml" ? key(Xhtml, "html") : key("http://www.w3.org/2000/svg", "svg");
                    if(Document.At(0).tag_ != ExpectedRoot) Reject("invalid-document-root", Resource.path_);
                    int Occurrence = 0;
                    for(const auto Index : Document.Descendants(0, Xhtml, "img")) {
                        const auto& Item = Document.At(Index); const auto Target = resolve(Resource.path_, Item.Attr("src"));
                        if(!Paths.contains(Target)) Reject("missing-image-manifest-reference", Target);
                        const bool HasAlt = Item.attributes_.contains("alt");
                        Append(Images, {{"document", Resource.path_}, {"occurrence", Occurrence++}, {"resource", Target},
                            {"alt", HasAlt ? QJsonValue(Item.Attr("alt")) : QJsonValue(QJsonValue::Null)},
                            {"altState", !HasAlt ? "missing" : Item.Attr("alt").isEmpty() ? "decorative" : "text"}});
                    }
                    if(Resource.properties_.contains("nav")) {
                        for(const auto Nav : Document.Descendants(0, Xhtml, "nav"))
                            if(tokens(Document.At(Nav).Attr("{http://www.idpf.org/2007/ops}type")).contains("toc")) {
                                auto Links=Document.Descendants(Nav,Xhtml,"a");
                                for(const auto Span:Document.Descendants(Nav,Xhtml,"span"))if(Document.At(Span).parent_>=0 && Document.At(Document.At(Span).parent_).tag_==key(Xhtml,"li"))Links.append(Span);
                                std::sort(Links.begin(),Links.end());
                                for(const auto Link : Links) {
                                    int Depth = -1;
                                    for(int Parent = Document.At(Link).parent_; Parent >= 0 && Parent != Nav; Parent = Document.At(Parent).parent_)
                                        if(Document.At(Parent).tag_ == key(Xhtml, "li")) ++Depth;
                                    if(Document.At(Link).tag_==key(Xhtml,"span"))Append(Navigation,{{"label",Document.Text(Link)},{"path",""},{"depth",qMax(0,Depth)}});
                                    else AddNavigation(Resource.path_, Document.At(Link).Attr("href"), Document.Text(Link), qMax(0, Depth));
                                }
                            }
                    }
                }
                AddResource(Resource.path_, Resource.media_, Raw);
            }
            if(Version == "2.0") {
                const auto Toc = Package.At(SpineIndex).Attr("toc");
                if(!Ids.contains(Toc)) Reject("missing-ncx", Toc);
                const auto& Resource = Resources[Ids[Toc]];
                if(Resource.media_ != "application/x-dtbncx+xml") Reject("invalid-ncx-media", Resource.path_);
                const Xml Document(Book.Read(Resource.path_), Resource.path_, Cancel);
                if(Document.At(0).tag_ != key(Ncx, "ncx")) Reject("invalid-ncx-root", Resource.path_);
                for(const auto Point : Document.Descendants(0, Ncx, "navPoint")) {
                    const auto Content = Document.One(Point, Ncx, "content", "invalid-ncx-point");
                    const auto Labels = Document.Children(Point, Ncx, "navLabel");
                    QString Label;
                    if(!Labels.isEmpty()) {
                        const auto Text = Document.Children(Labels.first(), Ncx, "text");
                        if(!Text.isEmpty()) Label = Document.Text(Text.first());
                    }
                    int Depth = 0;
                    for(int Parent = Document.At(Point).parent_; Parent >= 0; Parent = Document.At(Parent).parent_)
                        if(Document.At(Parent).tag_ == key(Ncx, "navPoint")) ++Depth;
                    AddNavigation(Resource.path_, Document.At(Content).Attr("src"), Label, Depth);
                }
            }
            if(Navigation.isEmpty()) Reject("missing-navigation");
            AddResource(Rootfile, "application/oebps-package+xml", ReaderPackage);
            if(Overrides) Overrides->insert(Rootfile, ReaderPackage);
            Check(Cancel);
            return {{"schemaVersion", 1}, {"phase", "P0-B"}, {"policy", "passive-reflowable-v1"}, {"sourceSha256", digest(Bytes)},
                {"title", Title}, {"languages", Languages}, {"readingProgression", Progression},
                {"renditionLayout",GlobalLayout},{"hasFixedLayout",FixedSpineItems>0},{"mixedLayout",FixedSpineItems>0 && FixedSpineItems<Spine.size()},
                {"recovery", QJsonObject{{"missingSpineItems", MissingSpineItems}, {"normalizedMedia", NormalizedMedia.size()}, {"sourceUnchanged", true},{"legacyZip",QJsonArray::fromStringList(Book.CompatibilityWarnings())}}},
                {"packageVersion", Version}, {"rootfile", Rootfile}, {"spine", Spine}, {"navigation", Navigation}, {"images", Images}, {"resources", Entries},
                {"limitations", QJsonArray{"UTF-8 XML and ZIP names only", "ZIP64, encrypted, signed and active publications rejected; fixed layout requires XHTML with numeric viewport dimensions",
                    "CSS URLs, SVG references and srcset not structurally validated; renderer allowlist remains required", "immutable reader preparation; authoring requires separate Save As validation; not EPUBCheck"}}};
        }
    }

    PackageResult Prepare(const QByteArray& Bytes, const Cancellation& Cancel)
    {
        try { Archive Book(Bytes, Cancel); return {prepare(Bytes, Cancel, Book), {}, {}}; }
        catch(const detail::Rejected& Error) { return {{}, Error.code_, Error.location_}; }
        catch(const std::bad_alloc&) { return {{}, "memory-limit", {}}; }
    }

    struct PublicationSession::State {
        QByteArray bytes_;
        Cancellation cancel_;
        std::unique_ptr<detail::Archive> archive_;
        QMap<QString,QByteArray> overrides_;
        QSet<QString> paths_;
        PackageResult index_;
    };
    PublicationSession::PublicationSession(std::unique_ptr<State> Value) : state_(std::move(Value)) {}
    PublicationSession::~PublicationSession() = default;
    std::shared_ptr<PublicationSession> PublicationSession::Open(QByteArray Bytes, const Cancellation& Cancel)
    {
        auto Value = std::make_unique<State>();
        Value->bytes_ = std::move(Bytes); Value->cancel_ = Cancel;
        try {
            Value->archive_ = std::make_unique<detail::Archive>(Value->bytes_, Cancel, 32 * 1024 * 1024);
            Value->index_.publication_ = prepare(Value->bytes_, Cancel, *Value->archive_, &Value->overrides_);
            for(const auto& Resource : Value->index_.publication_["resources"].toArray())
                Value->paths_.insert(Resource.toObject()["path"].toString());
        } catch(const detail::Rejected& Error) { Value->index_ = {{}, Error.code_, Error.location_}; }
        catch(const std::bad_alloc&) { Value->index_ = {{}, "memory-limit", {}}; }
        return std::shared_ptr<PublicationSession>(new PublicationSession(std::move(Value)));
    }
    PackageResult PublicationSession::Index() const { return state_->index_; }
    ResourceResult PublicationSession::ReadValidatedResource(const QString& Path) const
    {
        try {
            detail::Check(state_->cancel_);
            if(!state_->index_ || !state_->paths_.contains(Path)) detail::Reject("resource-not-admitted", Path);
            if(state_->overrides_.contains(Path)) return {state_->overrides_.value(Path), {}, {}};
            return {state_->archive_->Read(Path), {}, {}};
        } catch(const detail::Rejected& Error) { return {{}, Error.code_, Error.location_}; }
        catch(const std::bad_alloc&) { return {{}, "memory-limit", Path}; }
    }
    ResourceResult PublicationSession::ValidateAll() const
    {
        try {
            if(!state_->index_) return {{}, state_->index_.error_, state_->index_.location_};
            state_->archive_->VerifyAll(); return {};
        } catch(const detail::Rejected& Error) { return {{}, Error.code_, Error.location_}; }
        catch(const std::bad_alloc&) { return {{}, "memory-limit", {}}; }
    }

    PackageResult PrepareFile(const QString& Path, const Cancellation& Cancel)
    {
        try {
            detail::Check(Cancel);
            QFile File(Path);
            if(!File.open(QIODevice::ReadOnly)) detail::Reject("input-open", Path);
            if(File.size() > InputLimit) detail::Reject("input-limit", Path);
            QByteArray Bytes;
            while(!File.atEnd()) {
                detail::Check(Cancel);
                const auto Chunk = File.read(256 * 1024);
                if(File.error() != QFileDevice::NoError || (Chunk.isEmpty() && !File.atEnd())) detail::Reject("input-read", Path);
                Bytes.append(Chunk);
                if(Bytes.size() > InputLimit) detail::Reject("input-limit", Path);
            }
            return Prepare(Bytes, Cancel);
        } catch(const detail::Rejected& Error) { return {{}, Error.code_, Error.location_}; }
        catch(const std::bad_alloc&) { return {{}, "memory-limit", {}}; }
    }
}
