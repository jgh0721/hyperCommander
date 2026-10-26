#include "qtAutomation.hpp"
#include <QCheckBox>
#include "commandUiConfig.hpp"
#include <QDynamicPropertyChangeEvent>
#include "commonLibs/scripting/scriptExecution.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QAction>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include "settings/appSettings.hpp"
#include "qtDialogFrame.hpp"
#include <QToolBar>

namespace hc::automation {
namespace {
class AutomationDialog final : public QDialog {
public:
    AutomationDialog(QWidget* Parent, std::function<Host()> MakeHost, hc::settings::Repository* Settings, const QString& Id, bool Run, std::function<void()> Changed)
        : QDialog(Parent), makeHost_(std::move(MakeHost)), settings_(Settings), changed_(std::move(Changed)) {
        setAttribute(Qt::WA_DeleteOnClose); setObjectName("hc.automationDialog"); setWindowTitle(tr("명령 자동화")); resize(720, 460);
        auto* Layout = new QVBoxLayout(this);
        auto* Note = new QLabel(tr("시작한 탭과 선택 항목을 사용합니다. 각 명령의 작업이 끝나면 다음 줄을 실행합니다."), this); Note->setWordWrap(true); Layout->addWidget(Note);
        source_ = new QPlainTextEdit(this); source_->setObjectName("hc.automation.source"); source_->setPlainText("invoke \"hc.navigate.refresh\" {}\n"); Layout->addWidget(source_);
        language_ = new QComboBox(this); language_->setObjectName("hc.automation.language");
        language_->addItem(tr("명령 DSL"), "dsl"); language_->addItem(tr("Lua"), "lua"); Layout->insertWidget(1, language_);
        auto* Limits = new QLabel(tr("Lua: 메모리 8 MiB · 명령어 100만 회 · 호스트 호출 1만 회 · 실행 30초"), this);
        Limits->setWordWrap(true); Layout->addWidget(Limits);
        write_ = new QCheckBox(tr("이번 실행에서 파일 변경 허용"), this); write_->setObjectName("hc.automation.write"); Layout->addWidget(write_);
        status_ = new QLabel(this); status_->setObjectName("hc.automation.status"); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); Layout->addWidget(status_);
        auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Close, this); Layout->addWidget(Buttons);
        run_ = Buttons->addButton(tr("실행"), QDialogButtonBox::ActionRole); run_->setObjectName("hc.automation.run");
        stop_ = Buttons->addButton(tr("중지"), QDialogButtonBox::ActionRole); stop_->setObjectName("hc.automation.stop"); stop_->setEnabled(false);
        connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        connect(run_, &QPushButton::clicked, this, [this] { run(); });
        connect(stop_, &QPushButton::clicked, this, [this] { if (execution_) execution_->Cancel(); if (lua_) lua_->Cancel(); update(); });
        timer_ = new QTimer(this); timer_->setInterval(10); connect(timer_, &QTimer::timeout, this, [this] { if (execution_) execution_->Tick(); if (lua_) lua_->Tick(); update(); });
        connect(this, &QDialog::finished, this, [this] { timer_->stop(); if (execution_) execution_->Cancel(); if (lua_) lua_->Cancel(); });
        if (Settings) {
            store_ = std::make_unique<LibraryStore>(*Settings); auto Loaded = store_->Load();
            library_ = new QGroupBox(tr("저장한 스크립트"), this); Layout->insertWidget(1, library_); auto* Form = new QFormLayout(library_);
            choices_ = new QComboBox(library_); choices_->setObjectName("hc.automation.saved"); Form->addRow(tr("목록"), choices_);
            title_ = new QLineEdit(library_); title_->setObjectName("hc.automation.title"); Form->addRow(tr("이름"), title_);
            shortcut_ = new QKeySequenceEdit(library_); shortcut_->setObjectName("hc.automation.shortcut"); Form->addRow(tr("단축키 (Ctrl+Alt 조합)"), shortcut_);
            toolbar_ = new QCheckBox(tr("도구 모음에 표시"), library_); toolbar_->setObjectName("hc.automation.toolbar"); Form->addRow(toolbar_);
            auto* Save = new QPushButton(tr("저장"), library_); Save->setObjectName("hc.automation.save");
            auto* Remove = new QPushButton(tr("삭제"), library_); Remove->setObjectName("hc.automation.remove"); Form->addRow(Save, Remove);
            connect(choices_, &QComboBox::currentIndexChanged, this, [this](int Index) { select(Index); });
            connect(Save, &QPushButton::clicked, this, [this] { save(false); }); connect(Remove, &QPushButton::clicked, this, [this] { save(true); });
            if (!Loaded.HasValue()) { library_->setEnabled(false); status_->setText(QString::fromStdString(Loaded.Failure().code_)); }
            else { libraryWritable_ = true; scripts_ = std::move(Loaded.Value()); refill(Id); }
        }
        // The note becomes the header subtitle; status · run · stop · close share the footer band.
        Note->hide();
        auto* Footer = hc::ui::ApplyDialogFrame(this, {{}, Note->text(), fm::ui::glyph::Play});
        status_->setParent(Footer->parentWidget()); Footer->insertWidget(0, status_, 1); hc::ui::MoveToFooter(Footer, Buttons);
        fm::style::setButtonRole(run_, fm::style::ButtonRole::Primary);
        if (Run && libraryWritable_ && choices_->currentData().toString() == Id && !Id.isEmpty()) QTimer::singleShot(0, this, [this] { run(); });
    }
private:
    void refill(const QString& Id) {
        choices_->blockSignals(true); choices_->clear(); choices_->addItem(tr("새 스크립트"), QString());
        for (const auto& Script : scripts_) choices_->addItem(Script.title_, Script.id_);
        const int Index = choices_->findData(Id); choices_->setCurrentIndex(qMax(0,Index)); choices_->blockSignals(false); select(choices_->currentIndex());
    }
    void select(int Index) {
        write_->setChecked(false);
        if (Index <= 0 || Index > scripts_.size()) { title_->clear(); shortcut_->clear(); toolbar_->setChecked(false); language_->setCurrentIndex(0); source_->setPlainText("version 1\ninvoke \"hc.navigate.refresh\" {}\n"); return; }
        const auto& Script = scripts_[Index-1]; title_->setText(Script.title_); shortcut_->setKeySequence(QKeySequence(Script.shortcut_,QKeySequence::PortableText));
        toolbar_->setChecked(Script.toolbar_); source_->setPlainText(Script.source_);
        language_->setCurrentIndex(language_->findData(Script.language_));
    }
    void save(bool Remove) {
        if (!settings_ || !store_) return; auto Scripts = scripts_; QString Id = choices_->currentData().toString();
        const auto Existing = std::find_if(Scripts.begin(),Scripts.end(),[&](const auto& Script) { return Script.id_ == Id; });
        if (Remove) { if (Existing == Scripts.end()) return; Scripts.erase(Existing); Id.clear(); }
        else {
            const auto Shortcut = shortcut_->keySequence();
            const auto CoreKeys = hc::ui::EffectiveShortcuts(settings_->Read(hc::settings::Section::General).toObject(), settings_->Read(hc::settings::Section::Shortcuts).toObject());
            if (Shortcut.count() == 1 && CoreKeys.contains(Shortcut[0])) { status_->setText(tr("파일 관리자 명령에서 사용하는 단축키입니다.")); return; }
            if (!Shortcut.isEmpty() && parentWidget()) for (auto* Action : parentWidget()->findChildren<QAction*>()) {
                if ((Id.isEmpty() || Action->property("hc.scriptId").toString() != Id) && Action->shortcuts().contains(Shortcut)) { status_->setText(tr("다른 명령에서 사용하는 단축키입니다.")); return; }
            }
            if (!checkSource()) return;
            if (Id.isEmpty()) Id = QUuid::createUuid().toString(QUuid::Id128);
            SavedScript Script{Id,title_->text().trimmed(),source_->toPlainText(),Shortcut.toString(QKeySequence::PortableText),toolbar_->isChecked()};
            Script.language_ = language_->currentData().toString();
            if (Existing == Scripts.end()) Scripts.append(std::move(Script)); else *Existing = std::move(Script);
        }
        auto Saved = store_->Save(Scripts); if (!Saved.HasValue()) { status_->setText(QString::fromStdString(Saved.Failure().code_)); return; }
        scripts_ = std::move(Scripts); refill(Id); status_->setText(Remove ? tr("삭제했습니다.") : tr("저장했습니다. 파일 변경 권한은 실행마다 선택합니다.")); if (changed_) changed_();
    }
    void run() {
        if ((execution_ && execution_->Status() == State::Running) || (lua_ && lua_->IsRunning())) return;
        execution_.reset(); lua_.reset();
        if (language_->currentData() == "lua") {
            if (!checkSource()) return;
            std::set<std::string> Permissions{"navigation"}; if (write_->isChecked()) Permissions.insert("file-write");
            lua_ = std::make_unique<script::ScriptExecution>(std::make_shared<script::CommandScriptApi>(makeHost_(), Permissions), source_->toPlainText().toUtf8().toStdString());
            setRunning(true); timer_->start(); update(); return;
        }
        auto Host = makeHost_(); Diagnostic Details; auto Parsed = Compile(source_->toPlainText(), &Details, Host.schema_);
        if (!Parsed.HasValue()) { status_->setText(tr("%1행 %2열: %3").arg(Details.line_).arg(Details.column_).arg(Details.message_)); return; }
        if (Host.schema_ && !write_->isChecked()) for (const auto& Step : Parsed.Value()) if (Step.kind_ == StepKind::Invoke) {
            const auto Schema = Host.schema_(Step.command_);
            if (Schema.HasValue() && Schema.Value().permission_ == "file-write") { status_->setText(tr("permission-required · 파일 변경 허용을 선택한 후 실행하십시오.")); return; }
        }
        std::set<std::string> Permissions{"navigation"}; if (write_->isChecked()) Permissions.insert("file-write");
        execution_ = std::make_unique<Execution>(std::move(Parsed.Value()), std::move(Host), Permissions, QUuid::createUuid().toString(QUuid::Id128).toStdString());
        setRunning(true); timer_->start(); update();
    }
    bool checkSource() {
        if (language_->currentData() == "lua") {
            auto Valid = script::CheckLuaSyntax(source_->toPlainText().toUtf8().toStdString());
            if (!Valid.HasValue()) status_->setText(QString::fromStdString(Valid.Failure().code_));
            return Valid.HasValue();
        }
        auto Host = makeHost_(); Diagnostic Details; auto Parsed = Compile(source_->toPlainText(), &Details, Host.schema_);
        if (!Parsed.HasValue()) status_->setText(tr("%1행 %2열: %3").arg(Details.line_).arg(Details.column_).arg(Details.message_));
        return Parsed.HasValue();
    }
    void setRunning(bool Running) {
        source_->setReadOnly(Running); write_->setEnabled(!Running); language_->setEnabled(!Running);
        run_->setEnabled(!Running); stop_->setEnabled(Running);
        if (library_) library_->setEnabled(!Running && libraryWritable_);
    }
    void update() {
        if (lua_) {
            if (lua_->IsRunning()) { status_->setText(tr("Lua 실행 중")); return; }
            lua_->Tick(); timer_->stop(); setRunning(false); write_->setChecked(false);
            const auto Outcome = lua_->Outcome();
            status_->setText(Outcome && Outcome->HasValue() ? tr("Lua 실행 완료") : tr("Lua 중단: %1").arg(Outcome ? QString::fromStdString(Outcome->Failure().code_) : tr("결과 없음")));
            return;
        }
        if (!execution_) return;
        if (execution_->Status() == State::Running) { status_->setText(tr("%1개 명령 완료 · %2행 실행 중").arg(execution_->CompletedSteps()).arg(execution_->Line())); return; }
        timer_->stop(); setRunning(false); write_->setChecked(false);
        if (library_) library_->setEnabled(libraryWritable_);
        if (execution_->Status() == State::CompletedWithErrors) { status_->setText(tr("일부 오류로 완료 · 성공 %1개, 오류 %2개\n%3행: %4").arg(execution_->CompletedSteps()).arg(execution_->Errors().size()).arg(execution_->Errors().front().line_).arg(QString::fromStdString(execution_->Errors().front().error_.code_))); return; }
        status_->setText(execution_->Status() == State::Completed ? tr("완료 · %1개 명령").arg(execution_->CompletedSteps())
            : tr("%1행에서 중단: %2").arg(execution_->Line()).arg(QString::fromStdString(execution_->Failure()->code_)));
    }
    std::function<Host()> makeHost_;
    std::unique_ptr<Execution> execution_;
    std::unique_ptr<script::ScriptExecution> lua_;
    QComboBox* language_ = nullptr;
    QPlainTextEdit* source_;
    QCheckBox* write_;
    QLabel* status_;
    QPushButton* run_;
    QPushButton* stop_;
    QTimer* timer_;
    QPointer<hc::settings::Repository> settings_;
    std::function<void()> changed_;
    std::unique_ptr<LibraryStore> store_;
    QList<SavedScript> scripts_;
    QGroupBox* library_ = nullptr;
    bool libraryWritable_ = false;
    QComboBox* choices_ = nullptr;
    QLineEdit* title_ = nullptr;
    QKeySequenceEdit* shortcut_ = nullptr;
    QCheckBox* toolbar_ = nullptr;
};
class LibraryActions final : public QObject {
public:
    LibraryActions(QMainWindow* Window, hc::settings::Repository& Settings, std::function<Host()> MakeHost)
        : QObject(Window), window_(Window), settings_(&Settings), makeHost_(std::move(MakeHost)) {
        Window->installEventFilter(this);
        auto* MenuBar = Window->findChild<QMenuBar*>("menubar");
        if (!MenuBar) MenuBar = Window->menuBar();
        menu_ = MenuBar->addMenu(tr("자동화")); toolbar_ = Window->addToolBar(tr("자동화")); toolbar_->setObjectName("hc.automationToolbar");
        auto* Edit = menu_->addAction(tr("스크립트 편집·실행…")); Edit->setObjectName("hc.menu.automation");
        connect(Edit,&QAction::triggered,this,[this] { open({},false); }); menu_->addSeparator(); reload();
    }
private:
    bool eventFilter(QObject* Watched, QEvent* Event) override {
        if (Watched == window_ && Event->type() == QEvent::DynamicPropertyChange
            && static_cast<QDynamicPropertyChangeEvent*>(Event)->propertyName() == "hc.shortcutRevision") reload();
        return QObject::eventFilter(Watched, Event);
    }
    void open(const QString& Id, bool Run) {
        if (!window_ || !settings_) return;
        // One live editor per window bounds executions and makes shortcut repeats
        // focus the existing run instead of creating parallel file operations.
        if (dialog_) { dialog_->show(); dialog_->raise(); dialog_->activateWindow(); return; }
        dialog_ = Show(window_,makeHost_,settings_,Id,Run,[Guard=QPointer<LibraryActions>(this)] { if(Guard) Guard->reload(); });
    }
    void reload() {
        qDeleteAll(actions_); actions_.clear(); if (!settings_) return; LibraryStore Store(*settings_); auto Loaded = Store.Load();
        if (!Loaded.HasValue()) { menu_->setToolTip(QString::fromStdString(Loaded.Failure().code_)); toolbar_->hide(); return; }
        for (const auto& Script : Loaded.Value()) {
            auto* Action = new QAction(Script.title_,this); Action->setObjectName("hc.script." + Script.id_); Action->setProperty("hc.scriptId",Script.id_);
            const QKeySequence Shortcut(Script.shortcut_,QKeySequence::PortableText); bool Conflict=false;
            const auto CoreKeys = hc::ui::EffectiveShortcuts(settings_->Read(hc::settings::Section::General).toObject(), settings_->Read(hc::settings::Section::Shortcuts).toObject());
            if (Shortcut.count() == 1 && CoreKeys.contains(Shortcut[0])) Conflict = true;
            if (!Shortcut.isEmpty()) for (const auto* Other : window_->findChildren<QAction*>()) if (Other != Action && Other->shortcuts().contains(Shortcut)) Conflict=true;
            if (!Conflict) Action->setShortcut(Shortcut); else Action->setToolTip(tr("단축키 충돌: 메뉴에서 실행할 수 있습니다."));
            menu_->addAction(Action); if(Script.toolbar_) toolbar_->addAction(Action); actions_.append(Action);
            connect(Action,&QAction::triggered,this,[this,Id=Script.id_] { open(Id,true); });
        }
        toolbar_->setVisible(!toolbar_->actions().isEmpty());
    }
    QPointer<QMainWindow> window_;
    QPointer<hc::settings::Repository> settings_;
    std::function<Host()> makeHost_;
    QMenu* menu_;
    QToolBar* toolbar_;
    QList<QAction*> actions_;
    QPointer<QDialog> dialog_;
};
}
QDialog* Show(QWidget* Parent, std::function<Host()> MakeHost, hc::settings::Repository* Settings, QString Id, bool Run, std::function<void()> Changed)
{ auto* Dialog = new AutomationDialog(Parent,std::move(MakeHost),Settings,Id,Run,std::move(Changed)); Dialog->show(); return Dialog; }
void Install(QMainWindow* Window, hc::settings::Repository& Settings, std::function<Host()> MakeHost) { new LibraryActions(Window,Settings,std::move(MakeHost)); }
}
