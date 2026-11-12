#include "stdafx.h"
#include "documentSearchEngine.hpp"
#include "core/bodyProjection.hpp"
#include "UniqueLibs/scratchSession.hpp"

/// 모듈
#include "UniqueLibs/document/documentTextCodec.hpp"
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QThread>
#include <QLibrary>

namespace hc::document::search {
namespace {
QString pathKey(const QString& Path) { return QDir::cleanPath(QFileInfo(Path).absoluteFilePath()).toCaseFolded(); }
QByteArray encodeMatch(const Match& Hit) {
    wire::Writer Data; Data.U64(Hit.range_.start_); Data.U64(Hit.range_.length_); Data.U64(Hit.line_); Data.U64(Hit.column_);
    Data.U32(quint32(Hit.captures_.size()));
    for (const auto& Capture : Hit.captures_) { Data.U64(Capture.start_); Data.U64(Capture.length_); }
    return Data.bytes_;
}
Match decodeMatch(QByteArray Bytes) {
    wire::Reader Data(std::move(Bytes)); Match Hit;
    Hit.range_ = {qint64(Data.U64()), qint64(Data.U64())}; Hit.line_ = Data.U64(); Hit.column_ = Data.U64();
    const auto Count = Data.U32();
    if (Count > 65536) return {};
    for (quint32 I = 0; I < Count; ++I) Hit.captures_.append({qint64(Data.U64()), qint64(Data.U64())});
    return Data.Done() ? Hit : Match{};
}
}
Engine::Engine() = default;
Engine::~Engine() {
    const auto Name = database_.connectionName(); database_.close(); database_ = {};
    if (!Name.isEmpty()) QSqlDatabase::removeDatabase(Name);
    const auto JournalName = journal_.connectionName(); journal_.close(); journal_ = {};
    if (!JournalName.isEmpty()) QSqlDatabase::removeDatabase(JournalName);
}
bool Engine::initialize() {
    if (worker_) return database_.isOpen();
    // The kit uses a separate SQLite runtime. Keep it in the updater-owned Libs
    // tree and preload the exact path before Qt loads qsqlite.dll by plugin name.
    static const bool SqliteReady = [] {
        auto* Library = new QLibrary(QCoreApplication::applicationDirPath() + "/Libs/sqldrivers/" +
#ifdef QT_DEBUG
            "sqlite3d.dll"
#else
            "sqlite3.dll"
#endif
        );
        Library->setLoadHints(QLibrary::PreventUnloadHint);
        return Library->load();
    }();
    if (!SqliteReady) return false;
    directory_ = std::make_unique<QTemporaryDir>(hc::scratch::Session::Current().Template("document-search"));
    if (!directory_->isValid()) return false;
    errorLog_=std::make_unique<QFile>(directory_->filePath("search-errors.log"));
    if(!errorLog_->open(QIODevice::WriteOnly|QIODevice::Truncate))return false;
    database_ = QSqlDatabase::addDatabase("QSQLITE", QUuid::createUuid().toString());
    database_.setDatabaseName(directory_->filePath("results.sqlite"));
    if (!database_.open()) return false;
    QSqlQuery Query(database_); Query.exec("PRAGMA journal_mode=WAL"); Query.exec("PRAGMA cache_size=-4096");
    if (!Query.exec("CREATE TABLE sources(id INTEGER PRIMARY KEY, data BLOB NOT NULL)") ||
        !Query.exec("CREATE TABLE hits(id INTEGER PRIMARY KEY, source INTEGER NOT NULL, data BLOB NOT NULL)") ||
        !Query.exec("CREATE INDEX hits_source ON hits(source,id)")) return false;
    worker_ = new WorkerClient(this);
    connect(worker_, &WorkerClient::ReadyChanged, this, [this] {
        if (running_ && worker_->Ready()) next();
        else if (running_ && !worker_->IsRunning()) QTimer::singleShot(0, this, [this] { if (running_) worker_->Start(); });
    });
    connect(worker_, &WorkerClient::Registered, this, [this](const QString& Id) {
        if (running_ && !stop_ && Id == workerSourceId_ && worker_->Ready() && !worker_->Busy()) worker_->Search(Id, {}, request_["query"].toString(), {request_["regex"].toBool(), request_["case"].toBool()}, generation_);
    });
    connect(worker_, &WorkerClient::SourceMetadata, this, [this](const QByteArray& Hash, const QString& Encoding, bool Bom) {
        current_["hash"] = QString::fromLatin1(Hash.toHex()); current_["encoding"] = Encoding; current_["bom"] = Bom;
    });
    connect(worker_, &WorkerClient::PatternGroups, this, [this](const QStringList& Groups) { groups_ = Groups; });
    connect(worker_, &WorkerClient::Matches, this, [this](quint64 Generation, const QVector<Match>& Hits) { if (Generation == generation_ && running_ && !stop_) append(Hits); });
    connect(worker_, &WorkerClient::Finished, this, [this](quint64 Generation) { if (Generation == generation_ && running_) finishFile(); });
    connect(worker_, &WorkerClient::Failed, this, [this](quint64 Generation, const QString& Error) {
        if (!running_ || (Generation != generation_ && Generation != 0)) return;
        if (Generation == 0) { Stop(); report(true, Error); return; }
        diagnostic(current_["path"].toString(),Error);
        if (sourceInserted_) {
            current_["incomplete"] = true;
            QSqlQuery Failed(database_); Failed.prepare("UPDATE sources SET data=? WHERE id=?");
            Failed.addBindValue(QJsonDocument(current_).toJson(QJsonDocument::Compact)); Failed.addBindValue(sourceId_);
            if (!Failed.exec()) { Stop(); report(true, Failed.lastError().text()); return; }
        }
        ++errors_; report(false, current_["path"].toString() + ": " + Error); finishFile();
    });
    connect(worker_, &WorkerClient::Canceled, this, [this] { if (!running_) report(true, tr("검색 중지")); else QTimer::singleShot(0,this,&Engine::next); });
    return true;
}
void Engine::Begin(quint64 Generation, QJsonObject Request, QJsonArray Buffers, std::function<QJsonArray()> BookSources) {
    Stop();
    generation_ = Generation; request_ = std::move(Request); buffers_ = BookSources ? BookSources() : std::move(Buffers);
    stop_ = false; running_ = false; directories_.clear(); overlayPaths_.clear(); cachedId_.clear(); cachedText_.clear();
    rows_ = files_ = errors_ = sourceId_ = 0; bufferIndex_ = 0; current_ = {}; reporting_.start();
    for(const auto& Buffer:buffers_)if(!Buffer.toObject()["error"].toString().isEmpty()){report(true,Buffer.toObject()["error"].toString());return;}
    if (!initialize()) { report(true, tr("검색 결과 저장소를 열 수 없습니다.")); return; }
    lastError_.clear();errorLog_->resize(0);errorLog_->seek(0);emit DiagnosticLog(generation_,errorLog_->fileName());
    // Keep immutable, content-addressed snapshots resident across query edits.
    // Generation tags reject old matches; cancellation acknowledgement gates next().
    QSqlQuery Clear(database_);
    if(!Clear.exec("DELETE FROM hits") || !Clear.exec("DELETE FROM sources")) { report(true,Clear.lastError().text());return; }
    filters_.clear(); workerSourceId_.clear();
    Matcher Pattern(request_["query"].toString(), {request_["regex"].toBool(), request_["case"].toBool()});
    if (!Pattern.Error().isEmpty()) { report(true, Pattern.Error()); return; }
    if (request_["query"].toString().isEmpty()) { report(true); return; }
    groups_ = Pattern.Groups();
    for (const auto& Buffer : buffers_) overlayPaths_.insert(pathKey(Buffer.toObject()["path"].toString()));
    if (!request_["book"].toBool()) {
        const auto Root = request_["root"].toString();
        if (!QFileInfo(Root).isDir()) { report(true, tr("검색 루트가 폴더가 아닙니다.")); return; }
        directories_.push_back(std::make_unique<QDirIterator>(Root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System | QDir::NoSymLinks));
    }
    running_ = true; report(); worker_->Start(); if(worker_->Ready() && !worker_->Busy())next();
}
bool Engine::matchesFilter(const QString& Path, const QString& Patterns) const {
    if(!filters_.contains(Patterns)) {
        QList<QRegularExpression> Expressions;
        for(const auto& Pattern:Patterns.split(QRegularExpression("[;,]"),Qt::SkipEmptyParts))
            Expressions.append(QRegularExpression(QRegularExpression::wildcardToRegularExpression(Pattern.trimmed(),QRegularExpression::NonPathWildcardConversion),QRegularExpression::CaseInsensitiveOption));
        filters_.insert(Patterns,Expressions);
    }
    for (const auto& Expression : filters_[Patterns]) {
        if (Expression.match(Path).hasMatch() || Expression.match(QFileInfo(Path).fileName()).hasMatch()) return true;
    }
    return false;
}
void Engine::next() {
    if (!running_ || stop_) { if (running_) Stop(); return; }
    if (!worker_->Ready() || worker_->Busy()) return;
    const auto Included = [this](const QString& Path) {
        return (request_["include"].toString().trimmed().isEmpty() || matchesFilter(Path, request_["include"].toString())) &&
            !matchesFilter(Path, request_["exclude"].toString());
    };
    while (bufferIndex_ < buffers_.size()) {
        auto Source = buffers_[bufferIndex_++].toObject();
        const auto Path = Source["path"].toString();
        const auto Relative = QDir(request_["root"].toString()).relativeFilePath(Path);
        if (!request_["book"].toBool() && (Relative == ".." || Relative.startsWith("../") || (!request_["recursive"].toBool(true) && Relative.contains('/')))) continue;
        if (Included(Relative)) { searchFile(Source); return; }
    }
    QElapsedTimer Budget; Budget.start();
    while (!directories_.empty()) {
        if (stop_) { Stop(); return; }
        auto& Cursor = directories_.back();
        if (!Cursor->hasNext()) { directories_.pop_back(); continue; }
        const auto Path = Cursor->next(); const auto Info = Cursor->fileInfo();
        const auto Relative = QDir(request_["root"].toString()).relativeFilePath(Path);
        if (Info.isDir()) {
            if (request_["recursive"].toBool(true) && !matchesFilter(Relative + "/", request_["exclude"].toString()) && !matchesFilter(Relative, request_["exclude"].toString()))
                directories_.push_back(std::make_unique<QDirIterator>(Path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System | QDir::NoSymLinks));
        } else if (Info.isFile() && !overlayPaths_.contains(pathKey(Path)) && Included(Relative)) {
            searchFile({{"path", Path}}); return;
        }
        if (Budget.elapsed() >= 4) { QTimer::singleShot(0, this, &Engine::next); return; }
    }
    running_ = false; report(true);
}
void Engine::searchFile(QJsonObject Source) {
    current_ = std::move(Source); current_["id"] = QString::number(++sourceId_); sourceInserted_ = false;
    if (request_["book"].toBool() && request_["body"].toBool()) {
        const auto Path = current_["resource"].toString();
        if (!Path.endsWith(".xhtml", Qt::CaseInsensitive) && !Path.endsWith(".html", Qt::CaseInsensitive) && !Path.endsWith(".htm", Qt::CaseInsensitive)) { finishFile(); return; }
        BodyProjection Body(current_["text"].toString());
        if (!Body.Error().isEmpty()) { diagnostic(Path,Body.Error());++errors_; report(false, Path + ": " + Body.Error()); finishFile(); return; }
        current_["sourceText"] = Body.Source(); current_["text"] = Body.Text(); current_["body"] = true;
    }
    const auto Id = current_["id"].toString();
    if (current_.contains("text")) {
        const auto Text=current_["text"].toString();
        workerSourceId_=QString::fromLatin1(QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(Text.utf16()),Text.size()*2),QCryptographicHash::Sha256).toHex());
        worker_->RegisterSnapshot(workerSourceId_, Text);
    }
    else worker_->Search(Id, current_["path"].toString(), request_["query"].toString(), {request_["regex"].toBool(), request_["case"].toBool()}, generation_);
}
void Engine::append(const QVector<Match>& Hits) {
    const auto PreviousRows = rows_;
    if (!database_.transaction()) { Stop(); report(true, database_.lastError().text()); return; }
    QSqlQuery Query(database_);
    bool Ok = true;
    if (!sourceInserted_) {
        Query.prepare("INSERT INTO sources VALUES(?,?)"); Query.addBindValue(sourceId_); Query.addBindValue(QJsonDocument(current_).toJson(QJsonDocument::Compact));
        Ok = Query.exec();
    }
    Query.prepare("INSERT INTO hits VALUES(?,?,?)");
    for (const auto& Hit : Hits) {
        Query.bindValue(0, rows_ + 1); Query.bindValue(1, sourceId_); Query.bindValue(2, encodeMatch(Hit));
        if (!Ok || !Query.exec()) { Ok = false; break; } ++rows_;
    }
    if (!Ok || !database_.commit()) { database_.rollback(); rows_ = PreviousRows; Stop(); report(true, Query.lastError().text()); return; }
    sourceInserted_ = true; report();
}
void Engine::finishFile() {
    if(sourceInserted_ && !current_["incomplete"].toBool()) {
        current_["complete"]=true;QSqlQuery Finished(database_);Finished.prepare("UPDATE sources SET data=? WHERE id=?");
        Finished.addBindValue(QJsonDocument(current_).toJson(QJsonDocument::Compact));Finished.addBindValue(sourceId_);
        if(!Finished.exec()){Stop();report(true,Finished.lastError().text());return;}
    }
    ++files_; current_ = {}; report();
    QTimer::singleShot(0, this, &Engine::next);
}
void Engine::report(bool Complete, QString Message) {
    if(!Complete && !Message.isEmpty())lastError_=Message;
    if (!Complete && reporting_.isValid() && reporting_.elapsed() < 50) return;
    if(errorLog_)errorLog_->flush();
    reporting_.restart(); emit Progress(generation_, rows_, files_, errors_, Complete, Complete?Message:lastError_);
}
void Engine::diagnostic(const QString& Path,const QString& Error) {
    if(!errorLog_)return;
    const auto Bytes=QJsonDocument(QJsonObject{{"path",Path},{"error",Error}}).toJson(QJsonDocument::Compact)+'\n';
    if(errorLog_->write(Bytes)!=Bytes.size()){Stop();report(true,tr("검색 오류 기록을 저장하지 못했습니다."));}
}
std::optional<QString> Engine::sourceText(const QJsonObject& Source) {
    const auto Id = Source["id"].toString(); if (cachedId_ == Id) return cachedText_;
    QString Text;
    if (Source.contains("text")) Text = Source["text"].toString();
    else {
        QFile File(Source["path"].toString()); if (!File.open(QIODevice::ReadOnly) || File.size() > 32 * 1024 * 1024) return {};
        const auto Bytes = File.read(32 * 1024 * 1024 + 1);
        if (File.error() != QFileDevice::NoError || QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex() != Source["hash"].toString().toLatin1()) return {};
        const auto Decoded = DecodeDocumentText(Bytes, Source["encoding"].toString()); if (!Decoded.HasValue()) return {};
        Text = Decoded.Value().text_;
    }
    cachedId_ = Id; cachedText_ = Text; return Text;
}
void Engine::Page(quint64 Generation, int First, int Count, QString ReplacementText, bool ReplaceMode, quint64 PreviewVersion) {
    if (Generation != generation_ || !database_.isOpen()) return;
    QSqlQuery Query(database_); Query.prepare("SELECT id,data,source FROM hits WHERE id>? ORDER BY id LIMIT ?");
    Query.addBindValue(First); Query.addBindValue(qBound(1, Count, 128));
    QJsonArray Rows; Replacement Replace(ReplacementText, request_["regex"].toBool(), groups_);
    qint64 PreviousSource = -1; QJsonObject Source; std::unique_ptr<BodyProjection> Body;
    if (Query.exec()) while (Query.next()) {
        const auto Hit = decodeMatch(Query.value(1).toByteArray());
        const auto SourceId = Query.value(2).toLongLong();
        if (SourceId != PreviousSource) {
            QSqlQuery Lookup(database_); Lookup.prepare("SELECT data FROM sources WHERE id=?"); Lookup.addBindValue(SourceId);
            if (!Lookup.exec() || !Lookup.next()) break;
            Source = QJsonDocument::fromJson(Lookup.value(0).toByteArray()).object(); PreviousSource = SourceId;
            Body = Source["body"].toBool() ? std::make_unique<BodyProjection>(Source["sourceText"].toString()) : nullptr;
        }
        QJsonObject Row{{"id", Query.value(0).toDouble()}, {"source", Source["id"]}, {"path", Source["path"]}, {"document", Source["document"]}, {"resource", Source["resource"]}, {"chapter",Source["chapter"]},
            {"start", double(Hit.range_.start_)}, {"length", double(Hit.range_.length_)}, {"line", double(Hit.line_)}, {"column", double(Hit.column_)}};
        const auto Relative=request_["book"].toBool()?Source["path"].toString():QDir(request_["root"].toString()).relativeFilePath(Source["path"].toString());
        const QFileInfo Display(Relative);Row["displayPath"]=Display.fileName()+(Display.path()=="."?QString{}:QStringLiteral(" · ")+Display.path());
        // An untitled or recovered buffer has no path of its own to show ("새 문서 2").
        if(const auto Name=Source["displayName"].toString();!Name.isEmpty())Row["displayPath"]=Name;
        const auto Text = sourceText(Source);
        if (!Text || !Hit.range_.Valid(Text->size())) Row["error"] = tr("검색 후 원문이 변경되었거나 읽을 수 없습니다.");
        else {
            const auto Start = qMax<qint64>(0, Hit.range_.start_ - 100); const auto End = qMin<qint64>(Text->size(), Hit.range_.start_ + Hit.range_.length_ + 160);
            Row["before"] = Text->mid(Start, Hit.range_.start_ - Start);
            Row["match"] = Text->mid(Hit.range_.start_, qMin<qint64>(Hit.range_.length_, 500));
            Row["after"] = Text->mid(Hit.range_.start_ + Hit.range_.length_, End - Hit.range_.start_ - Hit.range_.length_);
            if (ReplaceMode) {
                const auto Newline = Text->contains("\r\n") ? QStringLiteral("\r\n") : QStringLiteral("\n");
                const auto Expanded = Replace.Expand(*Text, Hit, Newline);
                if (Expanded) Row["replacement"] = Expanded->left(500); else Row["error"] = Replace.Error();
            }
            if (Body) {
                const auto Mapped = Body->Map(Hit.range_);
                if (Mapped) { Row["start"] = double(Mapped->first().start_); Row["length"] = double(Mapped->last().start_ + Mapped->last().length_ - Mapped->first().start_); Row["body"] = true; }
                else { Row["replaceError"] = tr("구조/블록 경계를 포함하여 바꿀 수 없습니다."); Row["start"] = -1; }
            }
        }
        Rows.append(Row);
    }
    emit PageReady(Generation, First, Rows, PreviewVersion);
}
void Engine::Stop() { stop_ = true; running_ = false; directories_.clear(); if (worker_ && worker_->IsRunning() && worker_->Busy()) worker_->Cancel(); }
void Engine::Shutdown() { Stop(); delete worker_; worker_ = nullptr; QThread::currentThread()->quit(); }
bool Engine::journal() {
    if (journal_.isOpen()) return true;
    const auto Root = journalDirectory_.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/document-search-undo" : journalDirectory_;
    if (!QDir().mkpath(Root)) return false;
    // Each live session holds an OS-backed lock. Prefer recoverable edits over a
    // newer empty/fully undone journal, without touching another workspace's batch.
    journal_ = QSqlDatabase::addDatabase("QSQLITE", QUuid::createUuid().toString());
    QString EmptyPath;
    for (const auto& Name : QDir(Root).entryList({"*.sqlite"}, QDir::Files, QDir::Time)) {
        auto Lock = std::make_unique<QLockFile>(Root + '/' + Name + ".lock"); Lock->setStaleLockTime(0);
        if (!Lock->tryLock(0)) continue;
        const auto Path=Root+'/'+Name;journal_.setDatabaseName(Path);
        if(!journal_.open())continue;
        bool Pending=false,Valid=false;
        {QSqlQuery Existing(journal_);Valid=Existing.exec("SELECT 1 FROM edits WHERE state<2 LIMIT 1");Pending=Valid&&Existing.next();}
        journal_.close();
        if(Pending){journalPath_=Path;journalLock_=std::move(Lock);break;}
        if(Valid&&EmptyPath.isEmpty())EmptyPath=Path;
    }
    if(journalPath_.isEmpty()&&!EmptyPath.isEmpty()){
        auto Lock=std::make_unique<QLockFile>(EmptyPath+".lock");Lock->setStaleLockTime(0);
        if(Lock->tryLock(0)){journalPath_=EmptyPath;journalLock_=std::move(Lock);}
    }
    if (journalPath_.isEmpty()) {
        journalPath_ = Root + '/' + QUuid::createUuid().toString(QUuid::Id128) + ".sqlite";
        journalLock_ = std::make_unique<QLockFile>(journalPath_ + ".lock"); if (!journalLock_->tryLock(0)) return false;
    }
    journal_.setDatabaseName(journalPath_);
    if (!journal_.open()) return false;
    QSqlQuery Query(journal_); Query.exec("PRAGMA synchronous=FULL");
    return Query.exec("CREATE TABLE IF NOT EXISTS edits(id INTEGER PRIMARY KEY, op INTEGER NOT NULL, source BLOB NOT NULL, beforeText TEXT NOT NULL, afterText TEXT NOT NULL, state INTEGER NOT NULL)");
}
void Engine::Apply(quint64 Generation, QString Scope, QJsonArray Ids, QString ReplacementText) {
    if (Generation != generation_ || applying_ || !database_.isOpen() || (running_ && Scope=="all")) { emit ApplyProgress(false,0,tr("검색 상태가 바뀌었거나 전체 검색이 완료되지 않았습니다."));return; }
    if(running_)Stop();
    if (!journal()) { emit ApplyProgress(false, 0, tr("되돌리기 기록을 만들지 못했습니다.")); return; }
    Replacement Replace(ReplacementText, request_["regex"].toBool(), groups_);
    if (!Replace.Error().isEmpty()) { emit ApplyProgress(false, 0, Replace.Error()); return; }
    QSqlQuery Select(database_); Select.exec("DROP TABLE IF EXISTS selected");
    if (!Select.exec("CREATE TEMP TABLE selected(id INTEGER PRIMARY KEY)")) { emit ApplyProgress(false, 0, Select.lastError().text()); return; }
    database_.transaction();
    for (const auto& Id : Ids) {
        if(Id.isObject()) {
            const auto Range=Id.toObject();Select.prepare("INSERT OR IGNORE INTO selected SELECT "+QString(Scope=="files"?"source":"id")+" FROM hits WHERE id BETWEEN ? AND ?");
            Select.addBindValue(Range["first"].toInteger());Select.addBindValue(Range["last"].toInteger());
        } else {Select.prepare("INSERT OR IGNORE INTO selected VALUES(?)");Select.addBindValue(Id.isString()?Id.toString().toLongLong():Id.toInteger());}
        if (!Select.exec()) { database_.rollback(); emit ApplyProgress(false, 0, Select.lastError().text()); return; }
    }
    if (!database_.commit()) { emit ApplyProgress(false, 0, database_.lastError().text()); return; }
    if(Scope=="files") {
        QSqlQuery Ready(database_);if(!Ready.exec("SELECT data FROM sources WHERE id IN (SELECT id FROM selected)")){emit ApplyProgress(false,0,Ready.lastError().text());return;}
        while(Ready.next())if(!QJsonDocument::fromJson(Ready.value(0).toByteArray()).object()["complete"].toBool()){emit ApplyProgress(false,0,tr("선택한 파일의 검색이 아직 끝나지 않았습니다. 다시 검색한 뒤 실행하십시오."));return;}
    }
    QSqlQuery Last(journal_); if (!Last.exec("SELECT coalesce(max(op),0)+1 FROM edits") || !Last.next()) {emit ApplyProgress(false,0,Last.lastError().text());return;}
    operation_ = Last.value(0).toLongLong(); applyScope_ = Scope; replacementText_ = ReplacementText;
    applySource_ = changed_ = pendingJournal_ = 0; pendingBook_ = {}; stop_ = false; applying_ = true; undoing_ = false;
    emit ApplyProgress(true, 0, tr("바꾸는 중…")); QTimer::singleShot(0, this, &Engine::applyNext);
}
void Engine::Undo() {
    if (running_ || applying_) return;
    if (!journal()) { emit ApplyProgress(false, 0, tr("되돌리기 기록을 열 수 없습니다.")); return; }
    QSqlQuery Last(journal_); if (!Last.exec("SELECT max(op) FROM edits WHERE state<2") || !Last.next() || Last.value(0).isNull()) { emit ApplyProgress(false, 0, tr("되돌릴 바꾸기가 없습니다.")); return; }
    operation_ = Last.value(0).toLongLong(); changed_ = pendingJournal_ = 0; pendingBook_ = {}; stop_ = false; applying_ = undoing_ = true;
    emit ApplyProgress(true, 0, tr("바꾸기 되돌리는 중…")); QTimer::singleShot(0, this, &Engine::applyNext);
}
void Engine::applyNext() {
    if (!applying_) return;
    if (stop_) { applying_ = false; emit ApplyProgress(false, changed_, tr("중지했습니다. 적용한 파일은 바꾸기 취소로 복원할 수 있습니다.")); return; }
    if (undoing_) {
        QSqlQuery Query(journal_); Query.prepare("SELECT id,source,beforeText,afterText FROM edits WHERE op=? AND state<2 ORDER BY id DESC LIMIT 1"); Query.addBindValue(operation_);
        if (!Query.exec()) { editFinished(Query.lastError().text()); return; }
        if (!Query.next()) { applying_ = false; emit ApplyProgress(false, changed_, tr("바꾸기 취소 완료")); return; }
        auto Source = QJsonDocument::fromJson(Query.value(1).toByteArray()).object();
        const auto Before = Query.value(2).toString(), After = Query.value(3).toString();
        if (!Source["resource"].toString().isEmpty()) {
            QSqlQuery Book(journal_); Book.prepare("SELECT id,source,beforeText,afterText FROM edits WHERE op=? AND state<2 ORDER BY id"); Book.addBindValue(operation_);
            if (!Book.exec()) { editFinished(Book.lastError().text()); return; }
            while (Book.next()) { auto Item = QJsonDocument::fromJson(Book.value(1).toByteArray()).object(); Item["undo"] = true; pendingBook_.append(QJsonObject{{"journal", Book.value(0).toDouble()}, {"source", Item}, {"before", Book.value(3).toString()}, {"after", Book.value(2).toString()}}); }
            emit EditBuffer(++editTicket_, {{"document", Source["document"]}, {"bookBatch", pendingBook_}}, {}, {}); return;
        }
        commitEdit(Source, After, Before, Query.value(0).toLongLong(), true); return;
    }
    QSqlQuery SourceQuery(database_);
    QString Sql = "SELECT id,data FROM sources WHERE id>?";
    if (applyScope_ == "files") Sql += " AND id IN (SELECT id FROM selected)";
    if (applyScope_ == "selected") Sql += " AND id IN (SELECT source FROM hits WHERE id IN (SELECT id FROM selected))";
    SourceQuery.prepare(Sql + " ORDER BY id LIMIT 1"); SourceQuery.addBindValue(applySource_);
    if (!SourceQuery.exec()) { editFinished(SourceQuery.lastError().text()); return; }
    if (!SourceQuery.next()) {
        if (!pendingBook_.isEmpty()) { emit EditBuffer(++editTicket_, {{"document", pendingBook_.first().toObject()["source"].toObject()["document"]}, {"bookBatch", pendingBook_}}, {}, {}); return; }
        applying_ = false; emit ApplyProgress(false, changed_, tr("바꾸기 완료")); return;
    }
    applySource_ = SourceQuery.value(0).toLongLong(); const auto Source = QJsonDocument::fromJson(SourceQuery.value(1).toByteArray()).object();
    if(applyScope_=="files" && !Source["complete"].toBool()){editFinished(tr("선택한 파일의 검색이 아직 끝나지 않았습니다: %1").arg(Source["path"].toString()));return;}
    if (Source["incomplete"].toBool()) { editFinished(tr("검색이 완료되지 않은 파일은 바꿀 수 없습니다: %1").arg(Source["path"].toString())); return; }
    if (Source["readOnly"].toBool()) { editFinished(tr("읽기 전용 문서는 바꿀 수 없습니다: %1").arg(Source["path"].toString())); return; }
    cachedId_.clear(); const auto Original = sourceText(Source);
    if (!Original) { editFinished(tr("검색 후 원문이 변경되었습니다: %1").arg(Source["path"].toString())); return; }
    QSqlQuery Hits(database_); Hits.prepare("SELECT data FROM hits WHERE source=?" + QString(applyScope_ == "selected" ? " AND id IN (SELECT id FROM selected)" : "") + " ORDER BY id"); Hits.addBindValue(applySource_);
    if (!Hits.exec()) { editFinished(Hits.lastError().text()); return; }
    Replacement Replace(replacementText_, request_["regex"].toBool(), groups_);
    const auto RawOriginal = Source["body"].toBool() ? Source["sourceText"].toString() : *Original;
    std::unique_ptr<BodyProjection> Body = Source["body"].toBool() ? std::make_unique<BodyProjection>(RawOriginal) : nullptr;
    QString After; qint64 End = 0;
    const auto Newline = Original->contains("\r\n") ? QStringLiteral("\r\n") : QStringLiteral("\n");
    while (Hits.next()) {
        if (stop_) { QTimer::singleShot(0, this, &Engine::applyNext); return; }
        const auto Hit = decodeMatch(Hits.value(0).toByteArray()); const auto Expanded = Replace.Expand(*Original, Hit, Newline);
        const auto Ranges = Body ? Body->Map(Hit.range_) : std::optional<QVector<Range>>(QVector<Range>{Hit.range_});
        if (!Expanded || !Ranges || Ranges->isEmpty()) { editFinished(tr("치환 범위 또는 본문 매핑이 유효하지 않습니다. 구조/블록 경계는 바꿀 수 없습니다.")); return; }
        bool First = true;
        for (const auto& Range : *Ranges) {
            if (!Range.Valid(RawOriginal.size()) || Range.start_ < End) { editFinished(tr("치환 범위가 겹칩니다.")); return; }
            After += RawOriginal.mid(End, Range.start_ - End);
            if (First) After += Body ? BodyProjection::Escape(*Expanded) : *Expanded;
            End = Range.start_ + Range.length_; First = false;
        }
        if (After.size() > 32 * 1024 * 1024) { editFinished(tr("치환 결과가 문서 편집 크기 제한을 초과합니다.")); return; }
    }
    After += RawOriginal.mid(End);
    if (Body && !BodyProjection(After).Error().isEmpty()) { editFinished(tr("치환 결과의 XHTML 검증에 실패했습니다.")); return; }
    if (After == RawOriginal) { QTimer::singleShot(0, this, &Engine::applyNext); return; }
    QSqlQuery Record(journal_); Record.prepare("INSERT INTO edits(op,source,beforeText,afterText,state) VALUES(?,?,?,?,0)");
    Record.addBindValue(operation_); Record.addBindValue(QJsonDocument(Source).toJson(QJsonDocument::Compact)); Record.addBindValue(RawOriginal.isNull() ? QStringLiteral("") : RawOriginal); Record.addBindValue(After.isNull() ? QStringLiteral("") : After);
    if (!Record.exec()) { editFinished(Record.lastError().text()); return; }
    if (!Source["resource"].toString().isEmpty()) {
        pendingBook_.append(QJsonObject{{"journal", Record.lastInsertId().toDouble()}, {"source", Source}, {"before", RawOriginal}, {"after", After}});
        QTimer::singleShot(0, this, &Engine::applyNext); return;
    }
    commitEdit(Source, RawOriginal, After, Record.lastInsertId().toLongLong(), false);
}
void Engine::commitEdit(QJsonObject Source, QString Before, QString After, qint64 JournalId, bool Undo) {
    pendingJournal_ = JournalId;
    if (!Source["document"].toString().isEmpty()) {
        Source["undo"] = Undo; emit EditBuffer(++editTicket_, Source, Before, After); return;
    }
    if (!writer_) { editFinished(tr("파일 저장 경로가 연결되지 않았습니다.")); return; }
    const auto Old = EncodeDocumentText(Before, Source["encoding"].toString(), Source["bom"].toBool());
    const auto New = EncodeDocumentText(After, Source["encoding"].toString(), Source["bom"].toBool());
    if (!Old.HasValue() || !New.HasValue()) { editFinished(tr("원래 인코딩으로 치환문을 저장할 수 없습니다.")); return; }
    const auto Expected = QCryptographicHash::hash(Old.Value(), QCryptographicHash::Sha256);
    if (!Undo && Expected.toHex() != Source["hash"].toString().toLatin1()) { editFinished(tr("원문 인코딩의 무손실 왕복을 확인하지 못했습니다.")); return; }
    // A planned record may have been persisted immediately before a crash. If its
    // write never happened (or was already undone), recovery must be idempotent.
    if (Undo) {
        QFile File(Source["path"].toString());
        if (File.open(QIODevice::ReadOnly) && File.size() <= 64 * 1024 * 1024 && File.readAll() == New.Value()) { editFinished({}); return; }
    }
    editFinished(writer_(Source["path"].toString(), Expected, New.Value()));
}
void Engine::BufferApplied(quint64 Ticket, const QString& Error) {
    if (!applying_ || Ticket != editTicket_) return;
    if (!pendingBook_.isEmpty() && Error.isEmpty()) {
        if (!journal_.transaction()) { editFinished(journal_.lastError().text()); return; }
        QSqlQuery Update(journal_); Update.prepare("UPDATE edits SET state=? WHERE id=?");
        for (const auto& Item : pendingBook_) {
            Update.bindValue(0, undoing_ ? 2 : 1); Update.bindValue(1, Item.toObject()["journal"].toInteger());
            if (!Update.exec()) { journal_.rollback(); editFinished(Update.lastError().text()); return; }
        }
        if (!journal_.commit()) { editFinished(journal_.lastError().text()); return; }
        changed_ += pendingBook_.size(); pendingBook_ = {}; applying_ = false; emit ApplyProgress(false, changed_, undoing_ ? tr("책 바꾸기 취소 완료") : tr("책 바꾸기 완료")); return;
    }
    if (pendingJournal_ || !Error.isEmpty()) editFinished(Error);
}
void Engine::editFinished(const QString& Error) {
    if (!Error.isEmpty()) { applying_ = false; pendingJournal_ = 0; emit ApplyProgress(false, changed_, Error + tr(" · 이미 적용한 파일은 바꾸기 취소로 복원할 수 있습니다.")); return; }
    if (pendingJournal_) {
        QSqlQuery Update(journal_); Update.prepare("UPDATE edits SET state=? WHERE id=?"); Update.addBindValue(undoing_ ? 2 : 1); Update.addBindValue(pendingJournal_);
        if (!Update.exec()) { applying_ = false; emit ApplyProgress(false, changed_, Update.lastError().text()); return; }
        pendingJournal_ = 0; ++changed_;
    }
    cachedId_.clear(); emit ApplyProgress(true, changed_, undoing_ ? tr("되돌리는 중…") : tr("바꾸는 중…"));
    QTimer::singleShot(0, this, &Engine::applyNext);
}
}
