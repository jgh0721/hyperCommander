#include "fileToolsDialogs.hpp"
#include "permissionDialogs.hpp"
#include <fmwidgets/WrapLayout.h>
#include <QtConcurrent>
#include <fmwidgets/Button.h>
#include <fmwidgets/Card.h>
#include <fmwidgets/DialogWidgets.h>
#include <fmwidgets/RenameComparisonView.h>
#include <fmwidgets/SegmentedControl.h>
#include "UniqueLibs/settings/appSettings.hpp"

namespace hc::ui {
namespace {
void RestoreToolGeometry(QDialog* Dialog, const QString& Key) {
    const auto Stored = settings::AppSettings().Read(settings::Section::Options).toObject()[Key].toString();
    if (!Stored.isEmpty()) Dialog->restoreGeometry(QByteArray::fromBase64(Stored.toLatin1()));
}
void SaveToolGeometry(QDialog* Dialog, const QString& Key) {
    auto& Store = settings::AppSettings(); if (Store.Editing()) return;
    auto Options = Store.Read(settings::Section::Options).toObject(); Options[Key] = QString::fromLatin1(Dialog->saveGeometry().toBase64()); Store.Put(settings::Section::Options, Options);
}
}
NewFileDialog::NewFileDialog(const QString& Folder, bool Supported, QWidget* Parent)
    : DialogShell(tr("새 파일 만들기"), Parent), folder_(Folder), supported_(Supported) {
    setObjectName("hc.newFile"); resize(680, 520); SetSummary(QDir::toNativeSeparators(Folder));
    RestoreToolGeometry(this, "newFileGeometry");
    controls_ = new QWidget(this); auto* Layout = new QVBoxLayout(controls_); Layout->setContentsMargins(0,0,0,0); BodyLayout()->addWidget(controls_);
    auto* Form = new QFormLayout; Layout->addLayout(Form); name_ = new QLineEdit(tr("새 파일"), this); name_->setObjectName("hc.newFile.name"); Form->addRow(tr("파일 이름(&N)"), name_);
    auto* Grid = new QGridLayout; Layout->addLayout(Grid); const auto Templates = filetools::Templates();
    for (int I = 0; I < Templates.size(); ++I) {
        auto* Card = new fm::ui::ChoiceCard(Templates[I].label_, controls_); Card->setObjectName("hc.newFile.template." + QString::number(I));
        Card->setDetail(!Templates[I].fixedName_.isEmpty() ? Templates[I].fixedName_ : Templates[I].extension_.isEmpty() ? tr("확장자 없음") : '.' + Templates[I].extension_);
        Card->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
        Card->setKeyHint(QString::number(I+1)); Card->setShortcut(QKeySequence("Alt+" + QString::number(I+1))); Grid->addWidget(Card, I/2, I%2);
        if (I == 0) Card->setChecked(true);
        connect(Card, &QRadioButton::clicked, this, [this, I] { type_ = I; name_->setText(filetools::TemplateName(name_->text(), I)); validate(); });
    }
    open_ = new QCheckBox(tr("만든 후 텍스트 편집기로 열기(&O)"), controls_); open_->setObjectName("hc.newFile.open"); Layout->addWidget(open_);
    editor_ = new QComboBox(controls_); editor_->setObjectName("hc.newFile.editor");
    editor_->addItem(tr("메모장"), QJsonObject{{"program", QDir(qEnvironmentVariable("SystemRoot", "C:/Windows")).filePath("System32/notepad.exe")}});
    for (const auto& Value : hc::document::EditorItems(settings::AppSettings().Read(settings::Section::Editors))) {
        const auto Record = Value.toObject(); if (!Record["program"].toString().isEmpty()) editor_->addItem(Record["name"].toString(Record["program"].toString()), Record);
    }
    auto* EditorForm = new QFormLayout; Layout->addLayout(EditorForm); EditorForm->addRow(tr("텍스트 편집기"), editor_); editor_->setEnabled(false);
    editor_->setToolTip(tr("설정의 편집기 목록을 사용합니다. 인자의 %FILE%를 생성된 파일 경로로 바꾸며, 토큰이 없으면 마지막 인자로 추가합니다."));
    connect(open_, &QCheckBox::toggled, editor_, &QComboBox::setEnabled);
    bom_ = new QCheckBox(tr("UTF-8 BOM 포함(&U)"), controls_); bom_->setObjectName("hc.newFile.bom"); Layout->addWidget(bom_);
    auto* Note = new QLabel(tr("기존 파일은 덮어쓰지 않습니다. 빈 파일은 항상 0바이트로 만듭니다."), controls_); Note->setWordWrap(true); Layout->addWidget(Note);
    AcceptButton()->setText(tr("만들기(&S)")); connect(name_, &QLineEdit::textChanged, this, [this] { validate(); }); validate(); SetInitialInput(name_); name_->selectAll();
}
void NewFileDialog::validate() {
    bom_->setEnabled(type_ != 0);
    const auto Error = supported_ ? filetools::NameError(name_->text()) : tr("이 위치의 새 파일 생성을 지원하지 않습니다. 압축 컨테이너의 파일 추가는 별도 구현이 필요합니다.");
    SetMessage(Error, !Error.isEmpty()); SetAcceptEnabled(!busy_ && Error.isEmpty());
}
bool NewFileDialog::OpenEditor() const { return open_->isChecked(); }
QString NewFileDialog::EditorProgram() const { return editor_->currentData().toJsonObject()["program"].toString(); }
QStringList NewFileDialog::EditorArguments() const {
    auto Arguments = QProcess::splitCommand(editor_->currentData().toJsonObject()["arguments"].toString()); bool Found = false;
    for (auto& Argument : Arguments) if (Argument.contains("%FILE%")) { Argument.replace("%FILE%", QDir::toNativeSeparators(createdPath_)); Found = true; }
    if (!Found) Arguments.append(QDir::toNativeSeparators(createdPath_)); return Arguments;
}
void NewFileDialog::SetProvider(std::shared_ptr<storage::IWritableStorage> Writer, storage::Locator Location,
    std::function<storage::Result<operations::JobHandle>(operations::PreparedJob)> Submit, std::function<void(const std::string&)> Cancel) {
    writer_ = std::move(Writer); location_ = std::move(Location); submit_ = std::move(Submit); cancel_ = std::move(Cancel);
    supported_ = bool(writer_) && bool(submit_); open_->setEnabled(location_.kind_ != storage::LocatorKind::Plugin); validate();
}
void NewFileDialog::done(int Result) { SaveToolGeometry(this, "newFileGeometry"); QDialog::done(Result); }
void NewFileDialog::accept() {
    if (busy_ || !AcceptButton()->isEnabled()) return;
    if (submit_) {
        auto Prepared = filetools::PrepareCreate(writer_, location_, name_->text(), type_, bom_->isChecked());
        if (!Prepared.HasValue()) { SetMessage(QString::fromStdString(Prepared.Failure().code_), true); return; }
        auto Submitted = submit_(std::move(Prepared.Value()));
        if (!Submitted.HasValue()) { SetMessage(QString::fromStdString(Submitted.Failure().code_), true); return; }
        busy_ = true; controls_->setEnabled(false); SetAcceptEnabled(false); CancelButton()->setEnabled(true); SetMessage(tr("작업 관리자를 통해 파일을 만들고 있습니다…"));
        const auto Handle = Submitted.Value(); jobId_ = Handle.id_; const auto Name = name_->text(); auto* Timer = new QTimer(this);
        connect(Timer, &QTimer::timeout, this, [this, Timer, Handle, Name] {
            if (Handle.completion_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            const auto Result = Handle.completion_.get(); Timer->stop(); Timer->deleteLater(); busy_ = false; jobId_.clear(); controls_->setEnabled(true);
            if (Result.snapshot_.state_ == operations::JobState::Completed) { createdPath_ = QDir(folder_).filePath(Name); QDialog::accept(); }
            else { validate(); SetMessage(Result.snapshot_.error_ ? QString::fromStdString(Result.snapshot_.error_->code_) : tr("생성을 취소했습니다."), true); }
        }); Timer->start(50); return;
    }
    busy_ = true; controls_->setEnabled(false); SetAcceptEnabled(false); CancelButton()->setEnabled(false); SetMessage(tr("파일을 만들고 있습니다…"));
    using Result = QPair<QString,QJsonObject>; auto* Watcher = new QFutureWatcher<Result>(this); const auto Name = name_->text();
    connect(Watcher, &QFutureWatcher<Result>::finished, this, [this, Watcher, Name] {
        const auto Result = Watcher->result(); const auto Error=Result.first; Watcher->deleteLater(); busy_ = false; controls_->setEnabled(true); CancelButton()->setEnabled(true);
        if (Error.isEmpty()) { createdPath_ = QDir(folder_).filePath(Name); QDialog::accept(); }
        else {
            validate(); SetMessage(Error, true);
            if (!Result.second.isEmpty()) {
                PermissionDialog Permission({"A06",Name,folder_,Error,Result.second},this); Permission.exec();
                if (Permission.ResultDecision()==PermissionDialog::Succeeded) { createdPath_=QDir(folder_).filePath(Name); QDialog::accept(); }
                else if (Permission.ResultDecision()==PermissionDialog::Alternate) { folder_=Permission.AlternateFolder(); SetSummary(folder_); SetMessage(tr("위치를 변경했습니다. 이름과 템플릿을 확인하고 만들기를 누르세요.")); }
            }
        }
    });
    Watcher->setFuture(QtConcurrent::run([Folder = folder_, Name, Type = type_, Bom = bom_->isChecked()] { const auto Error=filetools::Create(Folder,Name,Type,Bom); return Result{Error, Error.contains(QRegularExpression(QStringLiteral("Windows 오류 5(?![0-9])"))) ? permissions::Request("create",Folder,Name,{},Type,Bom) : QJsonObject{}}; }));
}
void NewFileDialog::reject() { if (busy_ && cancel_ && !jobId_.empty()) { cancel_(jobId_); SetMessage(tr("생성 취소를 요청했습니다. 정리 결과를 기다립니다…")); } else if (!busy_) QDialog::reject(); }
void NewFileDialog::closeEvent(QCloseEvent* Event) { if (busy_) { reject(); Event->ignore(); } else DialogShell::closeEvent(Event); }

BatchRenameDialog::BatchRenameDialog(const QStringList& Paths, bool Supported, QWidget* Parent, std::shared_ptr<storage::ISelectionManifest> Selection, filetools::RenameJobController* Jobs)
    : DialogShell(tr("다중 이름 변경"), Parent), jobs_(Jobs ? Jobs : dynamic_cast<filetools::RenameJobController*>(permissions::Controller())), jobOwner_(jobs_ ? jobs_->RenameOwner() : nullptr) {
    setObjectName("hc.batchRename"); resize(1150, 820); SetSummary(tr("%1개 · %2").arg(Selection ? Selection->Count() : Paths.size()).arg(Paths.isEmpty() ? QString() : QFileInfo(Paths.first()).absolutePath()));
    RestoreToolGeometry(this, "batchRenameGeometry");
    previewTimer_ = new QTimer(this); previewTimer_->setSingleShot(true); previewTimer_->setInterval(80);
    controls_ = new QWidget(this); auto* Cards = new QGridLayout(controls_); Cards->setContentsMargins(0,0,0,0); BodyLayout()->addWidget(controls_);
    const auto Card = [&](int Column, const QString& Title) { auto* W = new fm::ui::Card(controls_); auto* F = new QFormLayout(W); F->addRow(new QLabel(Title, W)); Cards->addWidget(W, 0, Column); return F; };
    const auto Edit = [&](QFormLayout* Form, const QString& Label, const QString& Value, const char* Object) { auto* E = new QLineEdit(Value, controls_); E->setObjectName(Object); Form->addRow(Label, E); connect(E, &QLineEdit::textChanged, this, [this] { preview(); }); return E; };
    auto* Names = Card(0, tr("이름 마스크")); mask_ = Edit(Names, tr("이름(&M)"), "[N]", "hc.rename.mask"); extension_ = Edit(Names, tr("확장자(&E)"), "[E]", "hc.rename.extension");
    auto* Tokens = new QWidget(controls_); auto* TokenLayout = new QGridLayout(Tokens); TokenLayout->setContentsMargins(0,0,0,0);
    int T = 0; for (const auto& Token : {"[N]", "[E]", "[C]", "[P]", "[Y]", "[M]", "[D]", "[t]"}) {
        auto* Button = new fm::ui::Button(Token, Tokens); Button->setAutoDefault(false); TokenLayout->addWidget(Button, T/4, T%4); ++T;
        connect(Button, &QPushButton::clicked, this, [this, Text = QString(Token)] { mask_->insert(Text); mask_->setFocus(); });
    } Names->addRow(Tokens);
    auto* Search = Card(1, tr("찾기 및 바꾸기")); find_ = Edit(Search, tr("찾기(&F)"), {}, "hc.rename.find"); replace_ = Edit(Search, tr("바꾸기(&R)"), {}, "hc.rename.replace");
    regex_ = new QCheckBox(tr("정규식(&X)"), controls_); regex_->setObjectName("hc.rename.regex"); sensitive_ = new QCheckBox(tr("대소문자 구분(&A)"), controls_); Search->addRow(regex_); Search->addRow(sensitive_);
    includeExtension_ = new QCheckBox(tr("확장자를 포함하여 치환(&I)"), controls_); includeExtension_->setObjectName("hc.rename.includeExtension"); Search->addRow(includeExtension_);
    auto* Counter = Card(2, tr("대소문자 · 카운터")); nameCase_ = new QComboBox(controls_); extensionCase_ = new QComboBox(controls_);
    for (auto* C : {nameCase_, extensionCase_}) { C->addItems({tr("그대로"), tr("소문자"), tr("대문자")}); connect(C, &QComboBox::currentIndexChanged, this, [this] { preview(); }); }
    Counter->addRow(tr("이름"), nameCase_); Counter->addRow(tr("확장자"), extensionCase_);
    start_ = new QSpinBox(controls_); step_ = new QSpinBox(controls_); digits_ = new QSpinBox(controls_);
    start_->setRange(0, 100000000); step_->setRange(0, 100000); digits_->setRange(1, 12); start_->setValue(1); step_->setValue(1); digits_->setValue(3);
    start_->setObjectName("hc.rename.start"); step_->setObjectName("hc.rename.step"); digits_->setObjectName("hc.rename.digits");
    Counter->addRow(tr("시작"), start_); Counter->addRow(tr("증가"), step_); Counter->addRow(tr("자릿수"), digits_);
    auto* Dates = Card(3, tr("날짜 기준 · 프리셋")); dateSource_ = new QComboBox(controls_); dateSource_->setObjectName("hc.rename.dateSource"); dateSource_->addItems({tr("수정한 날짜"), tr("촬영한 날짜(EXIF · JPEG/TIFF)")}); Dates->addRow(dateSource_);
    dateOffset_ = new QSpinBox(controls_); dateOffset_->setObjectName("hc.rename.dateOffset"); dateOffset_->setRange(-864000,864000); Dates->addRow(tr("날짜 오프셋(초)"), dateOffset_);
    date_ = Edit(Dates, tr("[date] 형식"), "yyyy-MM-dd", "hc.rename.date"); preset_ = new QComboBox(controls_); preset_->setEditable(true); preset_->setObjectName("hc.rename.preset"); Dates->addRow(tr("프리셋"), preset_);
    const auto Stored = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["renamePresets"].toObject(); preset_->addItems(Stored.keys());
    auto* Save = new fm::ui::Button(tr("저장"), controls_); auto* Load = new fm::ui::Button(tr("불러오기"), controls_); auto* Delete = new fm::ui::Button(tr("삭제"), controls_);
    Save->setObjectName("hc.rename.presetSave"); Load->setObjectName("hc.rename.presetLoad"); Delete->setObjectName("hc.rename.presetDelete");
    auto* PresetButtons = new QWidget(controls_); auto* PresetLayout = new QHBoxLayout(PresetButtons); PresetLayout->setContentsMargins(0,0,0,0);
    for (auto* B : {Save, Load, Delete}) { B->setAutoDefault(false); PresetLayout->addWidget(B); } Dates->addRow(PresetButtons);
    connect(Save, &QPushButton::clicked, this, [this] {
        const auto Name = preset_->currentText().trimmed(); if (Name.isEmpty()) { SetMessage(tr("프리셋 이름을 입력하세요."), true); return; }
        auto& Store = hc::settings::AppSettings(); if (Store.Editing()) { SetMessage(tr("설정 편집을 닫은 뒤 저장하세요."), true); return; }
        auto Options = Store.Read(hc::settings::Section::Options).toObject(); auto Presets = Options["renamePresets"].toObject(); const auto R = rules();
        Presets[Name] = QJsonObject{{"name", R.name_}, {"extension", R.extension_}, {"find", R.find_}, {"replace", R.replace_}, {"regex", R.regex_}, {"sensitive", R.caseSensitive_},
            {"nameCase", R.nameCase_}, {"extensionCase", R.extensionCase_}, {"start", R.start_}, {"step", R.step_}, {"digits", R.digits_}, {"date", R.dateFormat_}, {"dateSource", R.dateSource_}, {"dateOffset", R.dateOffsetSeconds_}, {"includeExtension", R.includeExtension_}};
        Options["renamePresets"] = Presets; if (!Store.Put(hc::settings::Section::Options, Options)) { Store.Reload(); SetMessage(tr("프리셋 저장 실패. 디스크 설정을 다시 읽었습니다."), true); return; }
        if (preset_->findText(Name) < 0) preset_->addItem(Name); SetMessage(tr("프리셋을 저장했습니다."));
    });
    connect(Load, &QPushButton::clicked, this, [this] {
        const auto P = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["renamePresets"].toObject()[preset_->currentText()].toObject(); if (P.isEmpty()) return;
        ready_ = false; mask_->setText(P["name"].toString("[N]")); extension_->setText(P["extension"].toString("[E]")); find_->setText(P["find"].toString()); replace_->setText(P["replace"].toString());
        regex_->setChecked(P["regex"].toBool()); sensitive_->setChecked(P["sensitive"].toBool()); nameCase_->setCurrentIndex(P["nameCase"].toInt()); extensionCase_->setCurrentIndex(P["extensionCase"].toInt());
        start_->setValue(P["start"].toInt(1)); step_->setValue(P["step"].toInt(1)); digits_->setValue(P["digits"].toInt(3)); date_->setText(P["date"].toString("yyyy-MM-dd")); includeExtension_->setChecked(P["includeExtension"].toBool()); dateOffset_->setValue(P["dateOffset"].toInt()); dateSource_->setCurrentIndex(P["dateSource"].toInt()); ready_ = true; loadDates();
    });
    connect(Delete, &QPushButton::clicked, this, [this] {
        auto& Store = hc::settings::AppSettings(); if (Store.Editing()) { SetMessage(tr("설정 편집을 닫은 뒤 삭제하세요."), true); return; }
        auto Options = Store.Read(hc::settings::Section::Options).toObject(); auto Presets = Options["renamePresets"].toObject(); Presets.remove(preset_->currentText()); Options["renamePresets"] = Presets;
        if (Store.Put(hc::settings::Section::Options, Options)) preset_->removeItem(preset_->currentIndex()); else { Store.Reload(); SetMessage(tr("프리셋 삭제 실패. 디스크 설정을 다시 읽었습니다."), true); }
    });
    for (auto* C : {regex_, sensitive_, includeExtension_}) connect(C, &QCheckBox::toggled, this, [this] { preview(); });
    for (auto* S : {start_, step_, digits_, dateOffset_}) connect(S, &QSpinBox::valueChanged, this, [this] { preview(); });
    auto* Modes = new fm::ui::SegmentedControl({tr("1줄"), tr("2줄"), tr("자동")}, this); Modes->setObjectName("hc.rename.mode"); Modes->setCurrentIndex(2);
    auto* ViewOptions = new QHBoxLayout; BodyLayout()->addLayout(ViewOptions); ViewOptions->addWidget(Modes); ViewOptions->addStretch();
    auto* HelpButton = new fm::ui::Button(tr("마스크 도움말(&H)"), this); HelpButton->setAutoDefault(false); HelpButton->setObjectName("hc.rename.helpButton"); ViewOptions->addWidget(HelpButton);
    comparison_ = new fm::ui::RenameComparisonView(this); comparison_->setObjectName("hc.rename.preview"); comparison_->setMinimumHeight(280); BodyLayout()->addWidget(comparison_, 1);
    connect(Modes, &fm::ui::SegmentedControl::currentIndexChanged, this, [this](int Mode) { comparison_->SetMode(Mode); });
    summary_ = new QLabel(this); summary_->setObjectName("hc.rename.summary"); BodyLayout()->addWidget(summary_);
    auto* Pages = new QHBoxLayout; BodyLayout()->addLayout(Pages);
    previousPage_ = new fm::ui::Button(tr("이전 페이지"), this); nextPage_ = new fm::ui::Button(tr("다음 페이지"), this); pageLabel_ = new QLabel(this);
    previousPage_->setObjectName("hc.rename.previousPage"); nextPage_->setObjectName("hc.rename.nextPage"); pageLabel_->setObjectName("hc.rename.page");
    for (auto* Button : {previousPage_, nextPage_}) { Button->setAutoDefault(false); Button->setEnabled(false); }
    Pages->addWidget(previousPage_); Pages->addWidget(pageLabel_); Pages->addWidget(nextPage_); Pages->addStretch();
    connect(previousPage_, &QPushButton::clicked, this, [this] { --page_; showPage(); }); connect(nextPage_, &QPushButton::clicked, this, [this] { ++page_; showPage(); });
    auto* Help = new QLabel(tr("마스크: [N]/[E] 이름/확장자 · [N2-4] 2~4번째 문자 · [N:2:3] 2번째부터 3자 · [N:-3:3] 뒤 3자 · [C:+5] 카운터+5 · [date] 선택한 날짜. EXIF가 없으면 날짜 마스크 실행을 막습니다. 촬영일은 카메라의 현지 시각입니다."), this); Help->setWordWrap(true); Help->setTextFormat(Qt::PlainText); Help->setObjectName("hc.rename.help"); BodyLayout()->addWidget(Help); Help->hide();
    connect(HelpButton, &QPushButton::clicked, this, [Help] { Help->setVisible(!Help->isVisible()); });
    auto* History = new QHBoxLayout; BodyLayout()->addLayout(History); undoButton_ = new fm::ui::Button(tr("마지막 변경 되돌리기(&U)"), this); redoButton_ = new fm::ui::Button(tr("다시 실행(&Y)"), this);
    undoButton_->setObjectName("hc.rename.undo"); redoButton_->setObjectName("hc.rename.redo"); for (auto* B : {undoButton_, redoButton_}) { B->setAutoDefault(false); B->setEnabled(false); History->addWidget(B); }
    connect(undoButton_, &QPushButton::clicked, this, [this] { run(true); }); connect(redoButton_, &QPushButton::clicked, this, [this] { run(true); });
    auto* HistoryButton = new fm::ui::Button(tr("이름 변경 이력(&L)"), this); HistoryButton->setObjectName("hc.rename.history"); HistoryButton->setAutoDefault(false); History->addWidget(HistoryButton);
    connect(HistoryButton, &QPushButton::clicked, this, [this] { RenameHistoryDialog History(jobs_, this); History.exec(); if (onChanged_) onChanged_(); });
    auto* QueueRow = new QHBoxLayout; BodyLayout()->addLayout(QueueRow);
    queued_ = new QCheckBox(tr("대기열에 추가(&Q)"), this); queued_->setObjectName("hc.rename.queued"); QueueRow->addWidget(queued_); QueueRow->addStretch();
    background_ = new fm::ui::Button(tr("백그라운드로(&B)"), this); background_->setObjectName("hc.rename.background"); background_->setAutoDefault(false); background_->setEnabled(false); QueueRow->addWidget(background_);
    connect(background_, &QPushButton::clicked, this, [this] { if (busy_ && !jobId_.empty()) QDialog::reject(); });
    auto* Note = new QLabel(tr("최대 100,000개를 계획하고 미리보기는 256개씩 표시합니다. 작업 관리자에서 일시 정지·취소할 수 있으며 이력은 재시작 후에도 유지됩니다. 중단된 작업은 이력에서 이어 실행하거나 원래 이름으로 복구하세요. 압축·플러그인 다중 이름 변경은 아직 지원하지 않습니다."), this); Note->setWordWrap(true); BodyLayout()->addWidget(Note);
    AcceptButton()->setText(tr("이름 바꾸기(&S)")); CancelButton()->setText(tr("닫기")); SetAcceptEnabled(false); controls_->setEnabled(false);
    const auto Count = Selection ? Selection->Count() : std::uint64_t(Paths.size());
    if (!jobOwner_ || !Supported || !Count || Count > 100000) { SetMessage(tr("로컬·SMB의 1~100,000개 항목을 선택하세요. 압축·플러그인 다중 이름 변경은 지원하지 않습니다."), true); return; }
    SetMessage(tr("선택한 원본을 256개씩 확인하고 있습니다…"));
    struct Scan { QList<filetools::Entry> entries_; QSet<QString> names_; QString error_; };
    auto* Watcher = new QFutureWatcher<Scan>(this); const auto Stop = scanStop_.get_token();
    connect(Watcher, &QFutureWatcher<Scan>::finished, this, [this, Watcher] {
        const auto S = Watcher->result(); Watcher->deleteLater();
        if (scanStop_.stop_requested()) return;
        if (!S.error_.isEmpty()) { SetMessage(S.error_, true); return; }
        sources_ = S.entries_; existing_ = S.names_; if (!sources_.isEmpty()) SetSummary(tr("%1개 · %2").arg(sources_.size()).arg(QDir::toNativeSeparators(QFileInfo(sources_.first().path_).absolutePath()))); ready_ = true; controls_->setEnabled(true); SetMessage({}); preview();
    });
    Watcher->setFuture(QtConcurrent::run([Paths, Selection, Stop, Count] {
        Scan S; QStringList Selected = Paths;
        if (Selection) {
            storage::OperationContext Context; Context.stopToken_ = Stop;
            for (std::uint64_t Offset = 0; Offset < Count;) {
                if (Stop.stop_requested()) return S;
                auto Page = Selection->Read(Offset, std::size_t((std::min)(std::uint64_t(256), Count-Offset)), Context);
                if (!Page.HasValue()) { S.error_ = QString::fromStdString(Page.Failure().code_); return S; }
                if (Page.Value().entries_.empty() || Page.Value().entries_.size() > Count-Offset || Page.Value().entries_.size() > 256) { S.error_ = QObject::tr("선택 목록이 변경되었거나 페이지가 누락되었습니다."); return S; }
                for (const auto& Entry : Page.Value().entries_) {
                    if (Entry.locator_.kind_ != storage::LocatorKind::Local && Entry.locator_.kind_ != storage::LocatorKind::Smb) { S.error_ = QObject::tr("선택 항목의 공급자를 지원하지 않습니다."); return S; }
                    if (!Selection->Provider()) { S.error_ = QObject::tr("선택 공급자의 수명이 종료되었습니다."); return S; }
                    auto Current = Selection->Provider()->Stat(Entry.id_, Context).get();
                    if (!Current.HasValue() || Current.Value().id_ != Entry.id_) { S.error_ = QObject::tr("선택 후 원본이 변경되었습니다."); return S; }
                    Selected.append(QString::fromStdString(Entry.locator_.root_));
                }
                Offset += Page.Value().entries_.size();
                if (Page.Value().end_ && Offset != Count) { S.error_ = QObject::tr("전체 선택에 누락된 항목이 있습니다."); return S; }
            }
        }
        QSet<QString> Unique; QString Folder;
        for (const auto& Path : Selected) {
            if (Stop.stop_requested()) return S;
            const auto Parent = QFileInfo(Path).absolutePath(); if (Folder.isEmpty()) Folder = Parent;
            if (Parent.compare(Folder, Qt::CaseInsensitive) || Unique.contains(Path.toCaseFolded())) { S.error_ = QObject::tr("동일 폴더의 중복 없는 선택이 필요합니다."); return S; }
            Unique.insert(Path.toCaseFolded()); auto Entry = filetools::Inspect(Path);
            if (Entry.identity_.isEmpty()) { S.error_ = QObject::tr("원본 확인 실패 또는 링크 미지원: %1").arg(Path); return S; }
            S.entries_.append(Entry);
        }
        QDirIterator Names(Folder, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        while (Names.hasNext()) { if (Stop.stop_requested()) return S; Names.next(); S.names_.insert(Names.fileName().toCaseFolded()); }
        return S;
    }));
    connect(dateSource_, &QComboBox::currentIndexChanged, this, [this] { loadDates(); });
    connect(previewTimer_, &QTimer::timeout, this, [this] {
        const auto Generation = generation_; const auto Stop = planStop_.get_token(); const auto R = rules();
        using Preview = QList<filetools::RenameRow>; auto* Watcher = new QFutureWatcher<Preview>(this);
        connect(Watcher, &QFutureWatcher<Preview>::finished, this, [this, Watcher, Generation] {
            auto Rows = Watcher->result(); Watcher->deleteLater(); if (Generation != generation_ || !ready_ || busy_ || metadataLoading_) return;
            plan_ = std::move(Rows); int Changes = 0, Errors = 0, Unchanged = 0;
            for (const auto& Row : plan_) { if (!Row.error_.isEmpty()) ++Errors; else if (Row.source_.path_ != Row.target_) ++Changes; else ++Unchanged; }
            summary_->setText(tr("%1개 중 변경 %2 · 충돌/오류 %3 · 변경 없음 %4").arg(plan_.size()).arg(Changes).arg(Errors).arg(Unchanged));
            page_ = 0; showPage(); SetAcceptEnabled(allowExecute_ && Changes > 0 && !Errors); if (allowExecute_) SetMessage({});
        });
        Watcher->setFuture(QtConcurrent::run([Sources = sources_, Existing = existing_, R, Stop] {
            auto Rows = filetools::Plan(Sources, R, [Stop] { return Stop.stop_requested(); }); QSet<QString> Original;
            for (const auto& Entry : Sources) Original.insert(QFileInfo(Entry.path_).fileName().toCaseFolded());
            for (auto& Row : Rows) if (Existing.contains(QFileInfo(Row.target_).fileName().toCaseFolded()) && !Original.contains(QFileInfo(Row.target_).fileName().toCaseFolded())) Row.error_ = QObject::tr("충돌 · 이미 있음");
            return Rows;
        }));
    });
}
BatchRenameDialog::~BatchRenameDialog() { scanStop_.request_stop(); planStop_.request_stop(); metadataStop_.request_stop(); }
void BatchRenameDialog::done(int Result) { scanStop_.request_stop(); planStop_.request_stop(); metadataStop_.request_stop(); SaveToolGeometry(this, "batchRenameGeometry"); QDialog::done(Result); }
void BatchRenameDialog::loadDates() {
    metadataStop_.request_stop(); metadataStop_ = std::stop_source{}; ++generation_; planStop_.request_stop(); previewTimer_->stop(); SetAcceptEnabled(false);
    if (!ready_) return;
    if (dateSource_->currentIndex() != 1) { metadataLoading_ = false; undoButton_->setEnabled(!undo_.isEmpty() && !undone_); redoButton_->setEnabled(!undo_.isEmpty() && undone_); preview(); return; }
    metadataLoading_ = true; undoButton_->setEnabled(false); redoButton_->setEnabled(false); SetMessage(tr("EXIF 촬영일을 읽고 있습니다. 닫기로 조회를 취소할 수 있습니다…"));
    const auto Generation = generation_; const auto Stop = metadataStop_.get_token(); auto* Watcher = new QFutureWatcher<QList<filetools::Entry>>(this);
    connect(Watcher, &QFutureWatcher<QList<filetools::Entry>>::finished, this, [this, Watcher, Generation] {
        const auto Entries = Watcher->result(); Watcher->deleteLater(); if (Generation != generation_ || metadataStop_.stop_requested()) return;
        sources_ = Entries; metadataLoading_ = false; undoButton_->setEnabled(!undo_.isEmpty() && !undone_); redoButton_->setEnabled(!undo_.isEmpty() && undone_); preview();
    });
    Watcher->setFuture(QtConcurrent::run([Sources = sources_, Stop]() mutable {
        if (Stop.stop_requested()) return Sources;
        for (auto& Entry : Sources) {
            if (Stop.stop_requested()) return Sources;
            if (Entry.directory_) continue;
            Entry.captured_ = filetools::ReadExifDate(Entry.path_); const auto After = filetools::Inspect(Entry.path_);
            if (Entry.identity_ != After.identity_ || Entry.size_ != After.size_ || Entry.modified_ != After.modified_) { Entry.captured_ = {}; Entry.identity_.clear(); }
        }
        return Sources;
    }));
}
filetools::Rules BatchRenameDialog::rules() const {
    filetools::Rules R; R.name_ = mask_->text(); R.extension_ = extension_->text(); R.find_ = find_->text(); R.replace_ = replace_->text(); R.regex_ = regex_->isChecked(); R.caseSensitive_ = sensitive_->isChecked();
    R.nameCase_ = nameCase_->currentIndex(); R.extensionCase_ = extensionCase_->currentIndex(); R.start_ = start_->value(); R.step_ = step_->value(); R.digits_ = digits_->value(); R.dateFormat_ = date_->text(); R.dateSource_ = dateSource_->currentIndex(); R.dateOffsetSeconds_ = dateOffset_->value(); R.includeExtension_ = includeExtension_->isChecked(); return R;
}
void BatchRenameDialog::preview() {
    if (!ready_ || busy_ || metadataLoading_) return;
    allowExecute_ = true; ++generation_; planStop_.request_stop(); planStop_ = std::stop_source{}; SetAcceptEnabled(false); SetMessage(tr("이름 변경 계획을 계산하고 있습니다…")); previewTimer_->start();
}
void BatchRenameDialog::showPage() {
    const int Pages = (plan_.size()+255)/256; page_ = qBound(0,page_,qMax(0,Pages-1)); QList<fm::ui::RenameComparisonView::Row> Rows;
    for (int I = page_*256; I < qMin((page_+1)*256,int(plan_.size())); ++I) {
        const auto& R = plan_[I]; const bool Changed = R.source_.path_ != R.target_;
        Rows.append({QFileInfo(R.source_.path_).fileName(), QFileInfo(R.target_).fileName(), R.error_.isEmpty() ? Changed ? tr("정상") : tr("변경 없음") : R.error_, !R.error_.isEmpty(), R.source_.directory_ ? tr("폴더") : QLocale().formattedDataSize(R.source_.size_), QLocale().toString((dateSource_->currentIndex() == 1 ? R.source_.captured_ : R.source_.modified_).addSecs(dateOffset_->value()), QLocale::ShortFormat)});
    }
    comparison_->SetRows(Rows); comparison_->horizontalHeaderItem(3)->setText(dateSource_->currentIndex() == 1 ? tr("촬영 날짜") : tr("수정 날짜")); pageLabel_->setText(tr("%1 / %2 페이지 · 전체 %3개").arg(Pages ? page_+1 : 0).arg(Pages).arg(plan_.size()));
    previousPage_->setEnabled(!busy_ && page_ > 0); nextPage_->setEnabled(!busy_ && page_+1 < Pages);
}
void BatchRenameDialog::accept() { if (AcceptButton()->isEnabled()) run(false); }
void BatchRenameDialog::run(bool Undo) {
    if (!jobOwner_ || !ready_ || busy_ || metadataLoading_ || (Undo && (undo_.isEmpty() || recordId_.isEmpty()))) return;
    ++generation_; planStop_.request_stop(); previewTimer_->stop(); cancelWhenReady_ = false;
    const auto Plan = Undo ? undo_ : plan_; busy_ = true; controls_->setEnabled(false); queued_->setEnabled(false);
    SetAcceptEnabled(false); undoButton_->setEnabled(false); redoButton_->setEnabled(false); CancelButton()->setEnabled(true); previousPage_->setEnabled(false); nextPage_->setEnabled(false);
    SetMessage(tr("이름 변경 계획을 저장하고 작업 관리자에 접수하고 있습니다…"));
    using Prepared = storage::Result<operations::PreparedJob>; auto* Watcher = new QFutureWatcher<Prepared>(this);
    connect(Watcher, &QFutureWatcher<Prepared>::finished, this, [this, Watcher, Plan, Undo] {
        auto Prepared = Watcher->future().takeResult(); Watcher->deleteLater();
        if (!Prepared.HasValue() || !jobOwner_) {
            busy_ = false; controls_->setEnabled(true); queued_->setEnabled(true); preview();
            undoButton_->setEnabled(!undo_.isEmpty() && !undone_); redoButton_->setEnabled(!undo_.isEmpty() && undone_);
            SetMessage(Prepared.HasValue() ? tr("작업 관리자가 종료되었습니다.") : QString::fromStdString(Prepared.Failure().code_), true); return;
        }
        pendingRecordId_ = QString::fromStdString(Prepared.Value().request_.metadata_.options_).mid(14);
        Prepared.Value().request_.deferred_ = queued_->isChecked() || cancelWhenReady_;
        auto Started = jobs_->SubmitRename(std::move(Prepared.Value()), Undo ? undone_ ? tr("이름 변경 다시 실행") : tr("이름 변경 실행 취소") : tr("다중 이름 변경"));
        if (!Started.HasValue()) { busy_ = false; controls_->setEnabled(true); queued_->setEnabled(true); preview(); SetMessage(QString::fromStdString(Started.Failure().code_), true); return; }
        const auto Handle = Started.Value(); jobId_ = Handle.id_;
        if (cancelWhenReady_) jobs_->RenameScheduler().Cancel(jobId_);
        else if (queued_->isChecked()) { QDialog::accept(); return; }
        background_->setEnabled(true); SetMessage(tr("작업 관리자에서 이름을 변경하고 있습니다. 백그라운드로 보내도 작업은 계속됩니다."));
        auto* Timer = new QTimer(this);
        connect(Timer, &QTimer::timeout, this, [this, Timer, Handle, Plan, Undo] {
            if (Handle.completion_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            const auto JobResult = Handle.completion_.get(); Timer->stop(); Timer->deleteLater(); jobId_.clear(); background_->setEnabled(false);
            auto* ResultWatcher = new QFutureWatcher<filetools::BatchResult>(this);
            connect(ResultWatcher, &QFutureWatcher<filetools::BatchResult>::finished, this, [this, ResultWatcher, Plan, Undo] { const auto Result = ResultWatcher->result(); ResultWatcher->deleteLater(); finish(Result, Plan, Undo); });
            ResultWatcher->setFuture(QtConcurrent::run([Plan, Directory = filetools::RenameJournalDirectory(), Id = pendingRecordId_, JobResult] {
                filetools::BatchResult Result; auto Record = filetools::ReadRenameRecord(Directory, Id);
                Result.complete_ = Record.HasValue() && Record.Value().state_ == filetools::RenameState::Completed;
                for (int I = 0; I < Plan.size(); ++I) {
                    auto Entry = Plan[I].source_;
                    if (Result.complete_) Entry.path_ = Plan[I].target_;
                    else if (!Record.HasValue() || Record.Value().state_ != filetools::RenameState::Restored) {
                        Entry.identity_.clear();
                        const auto Temporary = Record.HasValue() ? Record.Value().temporary_.value(I) : QString();
                        for (const auto& Path : {Plan[I].source_.path_, Plan[I].target_, Temporary}) {
                            if (Path.isEmpty()) continue; const auto Actual = filetools::Inspect(Path);
                            if (Actual.identity_ == Plan[I].source_.identity_) { Entry = Actual; break; }
                        }
                    }
                    Result.current_.append(Entry);
                }
                if (!Result.complete_) Result.error_ = JobResult.snapshot_.error_ ? QString::fromStdString(JobResult.snapshot_.error_->code_) : QObject::tr("작업을 취소하고 원래 이름으로 복구했습니다.");
                return Result;
            }));
        }); Timer->start(50);
    });
    Watcher->setFuture(QtConcurrent::run([Plan, Undo, Id = recordId_, Directory = filetools::RenameJournalDirectory()] { return Undo ? filetools::PrepareRenameUndo(Directory, Id) : filetools::PrepareBatchRename(Directory, Plan); }));
}
void BatchRenameDialog::finish(const filetools::BatchResult& Result, const QList<filetools::RenameRow>& Plan, bool Undo) {
    busy_ = false; controls_->setEnabled(true); queued_->setEnabled(true); CancelButton()->setEnabled(true);
    if (Result.complete_) {
        recordId_ = pendingRecordId_;
        undo_.clear(); for (int I = 0; I < Plan.size(); ++I) undo_.append({Result.current_[I], Plan[I].source_.path_, {}});
        undone_ = Undo ? !undone_ : false;
        for (const auto& E : sources_) existing_.remove(QFileInfo(E.path_).fileName().toCaseFolded());
        sources_ = Result.current_; for (const auto& E : sources_) existing_.insert(QFileInfo(E.path_).fileName().toCaseFolded());
        preview(); allowExecute_ = false; SetAcceptEnabled(false); SetMessage(undone_ ? tr("원래 이름으로 되돌렸습니다. 다시 실행할 수 있습니다.") : tr("이름 변경 완료. 다음 실행은 마스크를 수정한 뒤 진행하세요."));
    } else {
        bool Restored = true; for (int I = 0; I < Plan.size(); ++I) if (Result.current_[I].path_ != Plan[I].source_.path_ || Result.current_[I].identity_.isEmpty()) Restored = false;
        sources_ = Result.current_; preview(); SetMessage(Result.error_, true);
        if (!Restored) { undo_.clear(); ready_ = false; controls_->setEnabled(false); SetAcceptEnabled(false);
            QList<fm::ui::RenameComparisonView::Row> Rows; for (int I = 0; I < Plan.size(); ++I) Rows.append({Plan[I].source_.path_, Result.current_[I].path_, tr("복구 결과 · 실제 경로"), true}); comparison_->SetRows(Rows); }
    }
    undoButton_->setEnabled(!undo_.isEmpty() && !undone_); redoButton_->setEnabled(!undo_.isEmpty() && undone_); if (onChanged_) onChanged_();
}
void BatchRenameDialog::reject() { if (busy_) { if (jobOwner_ && !jobId_.empty()) jobs_->RenameScheduler().Cancel(jobId_); else cancelWhenReady_ = true; SetMessage(tr("중지와 원래 이름 복구를 요청했습니다…")); } else QDialog::reject(); }
void BatchRenameDialog::closeEvent(QCloseEvent* Event) { if (busy_) { reject(); Event->ignore(); } else DialogShell::closeEvent(Event); }
} // namespace hc::ui
