#include "dialogShell.hpp"
#include <fmwidgets/Banner.h>
#include "UniqueLibs/renameSelection.hpp"
#include <fmwidgets/Button.h>
#include <fmwidgets/WrapCheckBox.h>
#include <fmwidgets/WrapLayout.h>
#include <fmwidgets/Card.h>
#include <fmwidgets/DialogFooter.h>
#include <fmwidgets/DialogHeader.h>
#include <fmwidgets/DialogWidgets.h>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QRadioButton>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QLabel>
#include <QLineEdit>
#include <QFormLayout>
#include <QKeyEvent>
#include <QShowEvent>
#include <QFileDialog>
#include <QSpinBox>
#include "commonLibs/operations/fileOperation.hpp"
#include <QScrollArea>
#include <QVBoxLayout>
#include <QTimer>
#include <QLocale>
#include <algorithm>
#include <chrono>
#include <QStorageInfo>
#include <QDirIterator>
#include <QJsonArray>
#include <QtConcurrent/QtConcurrentRun>
#include <Windows.h>
#include "../../UniqueLibs/settings/appSettings.hpp"

namespace hc::ui {
DialogShell::DialogShell(const QString& Title, QWidget* Parent)
    : QDialog(Parent)
{
    setObjectName(QStringLiteral("hc.dialog.shell"));
    setWindowTitle(Title);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setSizeGripEnabled(true);
    setMinimumSize(420, 260);
    resize(620, 330);
    auto* Layout = new QVBoxLayout(this);
    Layout->setContentsMargins(0, 0, 0, 0);
    Layout->setSpacing(0);
    header_ = new fm::ui::DialogHeader(this);
    header_->setTitle(Title);
    header_->setGlyph(fm::ui::glyph::Folder);
    header_->setSubtitleElide(Qt::ElideMiddle);
    Layout->addWidget(header_);
    auto* Scroll = new QScrollArea(this);
    Scroll->setWidgetResizable(true);
    Scroll->setFrameShape(QFrame::NoFrame);
    auto* Body = new QWidget(Scroll);
    bodyLayout_ = new QVBoxLayout(Body);
    bodyLayout_->setContentsMargins(20, 12, 20, 12);
    bodyLayout_->setSpacing(12);
    message_ = new fm::ui::Banner(Body);
    message_->setObjectName(QStringLiteral("hc.dialog.message"));
    message_->hide();
    bodyLayout_->addWidget(message_);
    Scroll->setWidget(Body);
    Layout->addWidget(Scroll, 1);
    auto* Footer = new fm::ui::DialogFooter(this);
    auto* Buttons = new QHBoxLayout(Footer);
    Buttons->setContentsMargins(20, 12, 20, 12);
    Buttons->setSpacing(8);
    Buttons->addStretch();
    accept_ = new fm::ui::Button(tr("확인(&O)"), Footer);
    accept_->setObjectName(QStringLiteral("hc.dialog.accept"));
    accept_->setRole(fm::ui::Button::Primary);
    accept_->setDefault(true);
    cancel_ = new fm::ui::Button(tr("취소(&C)"), Footer);
    cancel_->setObjectName(QStringLiteral("hc.dialog.cancel"));
    cancel_->setAutoDefault(false);
    Buttons->addWidget(accept_);
    Buttons->addWidget(cancel_);
    Layout->addWidget(Footer);
    connect(accept_, &QPushButton::clicked, this, &DialogShell::accept);
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
}

void DialogShell::SetTitle(const QString& Text) { setWindowTitle(Text); header_->setTitle(Text); }
void DialogShell::SetSummary(const QString& Text) { header_->setSubtitle(Text); }
void DialogShell::SetMessage(const QString& Text, bool Error)
{
    message_->setTone(Error ? fm::ui::Banner::Danger : fm::ui::Banner::Info);
    // Filenames and provider messages are data, never rich-text markup.
    message_->setText(Text.toHtmlEscaped());
    message_->setVisible(!Text.isEmpty());
}
void DialogShell::SetAcceptEnabled(bool Enabled) { accept_->setEnabled(Enabled); if (auto* Queue = findChild<QPushButton*>("hc.operation.queue")) Queue->setEnabled(Enabled); }
void DialogShell::SetInitialInput(QLineEdit* Input)
{
    if (initialInput_) initialInput_->removeEventFilter(this);
    initialInput_ = Input;
    if (Input) { Input->installEventFilter(this); Input->setFocus(Qt::OtherFocusReason); }
}
void DialogShell::showEvent(QShowEvent* Event)
{
    QDialog::showEvent(Event);
    if (initialInput_) initialInput_->setFocus(Qt::OtherFocusReason);
}
bool DialogShell::eventFilter(QObject* Watched, QEvent* Event)
{
    if (Watched == initialInput_ && Event->type() == QEvent::KeyPress) {
        const auto* Key = static_cast<QKeyEvent*>(Event);
        if ((Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter)
            && (Key->modifiers() == Qt::NoModifier || Key->modifiers() == Qt::KeypadModifier)) {
            // Click the validated primary action, even if another button was
            // previously focused. Consume the key to avoid a second default click.
            if (accept_->isEnabled()) accept_->click();
            return true;
        }
    }
    return QDialog::eventFilter(Watched, Event);
}
void DialogShell::accept()
{
    // Enter and direct accept must obey the same validation gate as a mouse click.
    if (accept_->isEnabled()) QDialog::accept();
}

FileInputDialog::FileInputDialog(const FileDialogRequest& Request, QWidget* Parent)
    : DialogShell(Request.title_, Parent)
{
    setObjectName(QStringLiteral("hc.dialog.fileInput"));
    SetSummary(Request.summary_);
    auto* Card = new fm::ui::Card(this);
    auto* Fields = new QVBoxLayout(Card);
    Fields->setContentsMargins(12, 12, 12, 12);
    Fields->setSpacing(8);
    auto* Label = new QLabel(Request.label_, Card);
    Label->setTextFormat(Qt::PlainText);
    Label->setWordWrap(true);
    input_ = new fm::ui::PathEdit(Card);
    input_->setObjectName(QStringLiteral("hc.dialog.input"));
    input_->setHistoryVisible(false);
    // A native folder chooser cannot represent archive or plugin destinations.
    input_->setBrowseVisible(Request.localFolder_);
    input_->setPath(Request.initialValue_);
    input_->lineEdit()->setAccessibleName(Request.label_);
    input_->lineEdit()->setObjectName(QStringLiteral("hc.dialog.input.editor"));
    Label->setBuddy(input_->lineEdit());
    Fields->addWidget(Label);
    Fields->addWidget(input_);
    BodyLayout()->addWidget(Card);
    BodyLayout()->addStretch();
    connect(input_, &fm::ui::PathEdit::pathChanged, this, [this] { validateInput(); });
    QWidget* Previous = input_->lineEdit();
    for (auto* Button : input_->findChildren<fm::ui::Button*>())
        if (!Button->isHidden()) {
            setTabOrder(Previous, Button);
            Previous = Button;
        }
    setTabOrder(Previous, AcceptButton());
    setTabOrder(AcceptButton(), CancelButton());
    input_->lineEdit()->selectAll();
    SetInitialInput(input_->lineEdit());
    validateInput();
}
QString FileInputDialog::Input() const { return input_->path(); }
void FileInputDialog::validateInput()
{
    const auto Value = Input();
    const bool Valid = !Value.trimmed().isEmpty() && !Value.contains(QChar::Null);
    SetAcceptEnabled(Valid);
    SetMessage(Valid ? QString{} : tr("이름 또는 경로를 입력하세요."));
}
std::optional<QString> FileInputDialog::Prompt(const FileDialogRequest& Request, QWidget* Parent)
{
    FileInputDialog Dialog(Request, Parent);
    if (Dialog.exec() != QDialog::Accepted) return std::nullopt;
    return Dialog.Input();
}

namespace {
QString OperationTitle(const QString& Command)
{
    return Command.endsWith("copy") ? QObject::tr("복사") : Command.endsWith("move") ? QObject::tr("이동")
        : Command.endsWith("rename") ? QObject::tr("이름 변경") : Command.endsWith("delete") ? QObject::tr("삭제") : QObject::tr("새 폴더");
}
bool ValidLeaf(const QString& Name);
QStringList PathParts(const QString& Value)
{
    return QDir::fromNativeSeparators(Value).split('/', Qt::SkipEmptyParts);
}
bool ValidRelativePath(const QString& Value)
{
    if (QDir::isAbsolutePath(Value)) return false;
    const auto Parts = PathParts(Value);
    return !Parts.isEmpty() && std::all_of(Parts.begin(), Parts.end(), [](const QString& Part) { return Part == "." || Part == ".." || ValidLeaf(Part); });
}
bool ValidLeaf(const QString& Name)
{
    static const QRegularExpression Invalid(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
    static const QRegularExpression Reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"), QRegularExpression::CaseInsensitiveOption);
    return !Name.isEmpty() && Name != "." && Name != ".." && !Name.endsWith('.') && !Name.endsWith(' ')
        && !Invalid.match(Name).hasMatch() && !Reserved.match(Name).hasMatch();
}
}
QString ResolveNativeDestination(const QString& Input, const QString& BaseDirectory)
{
    auto Path = QDir::fromNativeSeparators(Input);
    if (Path.trimmed().isEmpty() || Path.contains(QChar::Null)) return {};
    const auto SplitRoot = [](const QString& Value, QString& Root, QString& Rest) {
        static const QRegularExpression Drive("^(?:[A-Za-z]:/|//[?]/[A-Za-z]:/)");
        const auto Match = Drive.match(Value);
        if (Match.hasMatch()) { Root = Match.captured(); Rest = Value.mid(Root.size()); return true; }
        if (!Value.startsWith("//")) return false;
        const auto Prefix = Value.startsWith("//?/UNC/", Qt::CaseInsensitive) ? Value.left(8) : QString("//");
        const auto Parts = Value.mid(Prefix.size()).split('/', Qt::SkipEmptyParts);
        if (Parts.size() < 2 || !ValidLeaf(Parts[0]) || !ValidLeaf(Parts[1])) return false;
        Root = Prefix + Parts[0] + '/' + Parts[1]; Rest = Parts.mid(2).join('/'); return true;
    };
    QString Root, Rest;
    if (!SplitRoot(Path, Root, Rest)) {
        // Reject drive-relative and root-relative inputs: their meaning otherwise
        // depends on global per-drive/process state rather than the captured tab.
        if (Path.startsWith('/') || Path.contains(':')) return {};
        Path = QDir::fromNativeSeparators(BaseDirectory) + '/' + Path;
        if (!SplitRoot(Path, Root, Rest)) return {};
    }
    QStringList Parts;
    for (const auto& Part : Rest.split('/', Qt::SkipEmptyParts)) {
        if (Part == ".") continue;
        if (Part == "..") { if (!Parts.isEmpty()) Parts.removeLast(); continue; }
        if (!ValidLeaf(Part)) return {};
        Parts.append(Part);
    }
    // Parent traversal stops at the volume or share root, including UNC paths.
    return Root + (Root.endsWith('/') || Parts.isEmpty() ? QString() : QString("/")) + Parts.join('/');
}
FileOperationDialog::FileOperationDialog(const FileOperationDialogRequest& Request, QWidget* Parent)
    : DialogShell(OperationTitle(Request.command_), Parent), request_(Request)
{
    setObjectName(QStringLiteral("hc.dialog.fileOperation"));
    resize(640, Request.command_.endsWith("copy") || Request.command_.endsWith("move") ? 760 : 560);
    CancelButton()->setText(tr("취소"));
    const bool Delete = Request.command_.endsWith("delete");
    const bool Mkdir = Request.command_.endsWith("mkdir");
    const bool Rename = Request.command_.endsWith("rename");
    const bool Copy = Request.command_.endsWith("copy");
    const bool Transfer = Copy || Request.command_.endsWith("move");
    const auto Defaults = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject();
    SetSummary((Mkdir ? tr("위치: ") : Request.manifestSelection_ ? tr("선택 목록 전체 · ")
        : tr("%1개 항목 · ").arg(Request.names_.size())) + Request.source_);
    const auto Label = [this](const QString& Text) {
        auto* Item = new QLabel(Text, this);
        Item->setTextFormat(Qt::PlainText); Item->setWordWrap(true);
        BodyLayout()->addWidget(Item); return Item;
    };
    if (!Mkdir) {
        auto* Summary = new fm::ui::FileSummaryList(this);
        QList<fm::ui::FileSummaryList::Item> Items;
        for (const auto& Name : Request.names_) Items.append({Name, {}});
        Summary->setItems(Items); Summary->setMaxVisibleRows(3);
        Summary->setAccessibleName(Request.names_.join(QStringLiteral(", ")));
        BodyLayout()->addWidget(Summary);
    }
    sourceSummary_ = Label(Request.nativeSource_ ? tr("선택 요약 조회 중…") : tr("공급자의 재귀 용량 요약은 후속 구현 예정입니다."));
    sourceSummary_->setObjectName("hc.operation.summary");
    summarizeSource();
    if (Delete) {
        auto* Recycle = new QRadioButton(tr("휴지통으로 이동(&R)"), this);
        recycle_ = Recycle; Recycle->setObjectName("hc.operation.recycle");
        Recycle->setChecked(hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["deleteToRecycle"].toBool(true)); Recycle->setEnabled(false);
        Recycle->setToolTip(tr("고정 로컬 볼륨 여부를 확인하고 있습니다."));
        BodyLayout()->addWidget(Recycle);
        auto* Permanent = new QRadioButton(tr("영구 삭제(&P)"), this);
        Permanent->setObjectName(QStringLiteral("hc.operation.permanent")); Permanent->setChecked(!Recycle->isChecked());
        BodyLayout()->addWidget(Permanent);
        readonly_ = new fm::ui::WrapCheckBox(tr("읽기 전용 파일도 묻지 않고 삭제(&O)"), this);
        readonly_->setObjectName("hc.operation.readonly"); BodyLayout()->addWidget(readonly_);
        readonly_->setEnabled(Request.nativeSource_);
        Label(tr("휴지통 이동이 실패해도 영구 삭제로 전환하지 않습니다. 복원은 Windows 휴지통에서 가능합니다."));
        AcceptButton()->setText(tr("삭제 방식 선택"));
        SetAcceptEnabled(false);
        SetMessage(tr("영구 삭제를 선택하면 복원할 수 없습니다. 휴지통으로 자동 대체하지 않습니다."));
        const auto Update = [this, Permanent] {
            const bool PermanentDelete = Permanent->isChecked();
            SetAcceptEnabled(PermanentDelete || (recycle_->isChecked() && recycle_->isEnabled()));
            SetTitle(PermanentDelete ? tr("영구 삭제") : tr("삭제"));
            AcceptButton()->setText(PermanentDelete ? tr("영구 삭제(&D)") : tr("휴지통으로(&D)"));
            AcceptButton()->setRole(PermanentDelete ? fm::ui::Button::Danger : fm::ui::Button::Primary);
            SetMessage(PermanentDelete ? tr("선택한 항목과 하위 항목을 영구 삭제합니다. 복원할 수 없습니다.")
                : recycle_->isEnabled() ? tr("선택한 항목을 Windows 휴지통으로 이동합니다.") : tr("이 위치에서는 휴지통을 지원하지 않습니다. 영구 삭제는 직접 선택해야 합니다."), PermanentDelete);
        };
        connect(Permanent, &QRadioButton::toggled, this, Update);
        connect(Recycle, &QRadioButton::toggled, this, Update);
        auto* Eligibility = new QFutureWatcher<bool>(this);
        connect(Eligibility, &QFutureWatcher<bool>::finished, this, [this, Eligibility, Update] {
            recycle_->setEnabled(Eligibility->result());
            recycle_->setToolTip(tr("고정 로컬 볼륨에서만 지원합니다. 실행 시 다시 확인합니다."));
            Update(); Eligibility->deleteLater();
        });
        Eligibility->setFuture(QtConcurrent::run([Path = Request.source_, Native = Request.nativeSource_] {
            wchar_t Root[MAX_PATH]{};
            return Native && !Path.startsWith("\\\\") && GetVolumePathNameW(QDir::toNativeSeparators(Path).toStdWString().c_str(), Root, MAX_PATH) && GetDriveTypeW(Root) == DRIVE_FIXED;
        }));
        BodyLayout()->addStretch();
        CancelButton()->setFocus();
        return;
    }
    auto* InputLabel = Label(Mkdir ? tr("폴더 이름(&N)") : Rename ? tr("새 이름(&N)") : tr("대상 폴더(&D)"));
    input_ = new fm::ui::PathEdit(this);
    input_->lineEdit()->setObjectName(QStringLiteral("hc.operation.editor"));
    input_->lineEdit()->setAccessibleName(InputLabel->text()); InputLabel->setBuddy(input_->lineEdit());
    input_->setBrowseVisible(Request.nativeDestination_ && !Mkdir && !Rename);
    input_->setHistoryVisible(!Mkdir && !Rename && Request.nativeDestination_);
    input_->setHistory(Request.nativeDestination_ ? RecentTargets(Request.providerKey_) : QStringList{});
    input_->setPath(Request.initialValue_);
    BodyLayout()->addWidget(input_);
    preview_ = Label({});
    preview_->setObjectName(QStringLiteral("hc.operation.preview"));
    if (!Mkdir && !Rename) {
        Label(tr("최근 대상 — 이 공급자에서 사용한 경로 · Alt+1~3"));
        auto* Recent = new fm::ui::RecentTargetsBar(this);
        Recent->setTargets(Request.nativeDestination_ ? RecentTargets(Request.providerKey_).mid(0, 3) : QStringList{});
        BodyLayout()->addWidget(Recent);
        connect(Recent, &fm::ui::RecentTargetsBar::targetActivated, this, [this](int, const QString& Path) { input_->setPath(Path); });
        targetMode_ = new QComboBox(this); targetMode_->setObjectName("hc.operation.targetMode");
        targetMode_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); targetMode_->setMinimumContentsLength(12);
        targetMode_->setAccessibleName(tr("대상 경로 해석 방식"));
        targetMode_->addItem(tr("대상 폴더")); targetMode_->addItem(tr("단일 항목의 새 이름 또는 전체 경로"));
        targetMode_->setEnabled(Request.nativeDestination_ && Request.names_.size() == 1 && !Request.manifestSelection_);
        BodyLayout()->addWidget(targetMode_);
        connect(targetMode_, &QComboBox::currentIndexChanged, this, [this] { validateInput(); });
        auto* Fields = new fm::ui::WrapLayout;
        auto* ConflictGroup = new QWidget(this); auto* ConflictFields = new QVBoxLayout(ConflictGroup);
        ConflictFields->setContentsMargins(0, 0, 0, 0);
        auto* FilterGroup = new QWidget(this); auto* FilterFields = new QVBoxLayout(FilterGroup);
        FilterFields->setContentsMargins(0, 0, 0, 0);
        auto* ConflictLabel = new QLabel(tr("파일이 이미 있을 때(&E)"), this);
        conflict_ = new QComboBox(this); conflict_->setObjectName(QStringLiteral("hc.operation.conflict"));
        conflict_->setAccessibleName(tr("파일이 이미 있을 때"));
        conflict_->addItem(tr("매번 묻기"), QStringLiteral("ask"));
        conflict_->addItem(tr("덮어쓰기"), QStringLiteral("replace"));
        conflict_->addItem(tr("건너뛰기"), QStringLiteral("skip"));
        ConflictLabel->setBuddy(conflict_);
        ConflictFields->addWidget(ConflictLabel); ConflictFields->addWidget(conflict_); Fields->addWidget(ConflictGroup);
        auto* FilterLabel = new QLabel(tr("포함할 파일(&F)"), this);
        auto* Filter = new QLineEdit(Request.copyEngine_ ? Defaults["copyPattern"].toString("*") : "*", this); filter_ = Filter;
        Filter->setObjectName("hc.operation.filter"); Filter->setAccessibleName(tr("포함할 파일")); Filter->setEnabled(Request.copyEngine_);
        Filter->setToolTip(tr("파일명에 *와 ?를 사용합니다. 여러 패턴은 ;로 구분합니다. 폴더는 모두 탐색하며 빈 폴더도 포함합니다."));
        connect(Filter, &QLineEdit::textChanged, this, [this] { validateInput(); });
        FilterLabel->setBuddy(Filter); FilterFields->addWidget(FilterLabel); FilterFields->addWidget(Filter); Fields->addWidget(FilterGroup);
        BodyLayout()->addLayout(Fields);
    }
    if (Transfer) {
        Label(tr("복사 옵션"));
        auto* Options = new fm::ui::WrapLayout;
        const QStringList Names{tr("파일 속성 및 수정 시각 유지(&A)"), tr("SHA-256으로 검증(&V)"),
            tr("파일 접근 권한(DACL) 복사(&N)"), tr("파일 대체 데이터 스트림(ADS) 복사(&S)"), tr("심볼릭 링크는 링크로(&L)"), tr("빈 폴더도 복사(&M)")};
        const QStringList Keys{"copyMetadata", "copyVerify", "copyAcl", "copyStreams"};
        const QStringList Objects{"metadata", "verify", "acl", "streams"};
        for (int I = 0; I < Names.size(); ++I) {
            auto* Check = new fm::ui::WrapCheckBox(Names[I], this);
            const bool Supported = I < 4 && Request.copyEngine_ && Request.nativeDestination_ && (I < 2 || Request.nativeSource_);
            Check->setEnabled(Supported);
            Check->setChecked(I == 5 || (Supported && Defaults[Keys[I]].toBool(I == 0)));
            if (I < 4) { Check->setObjectName("hc.operation." + Objects[I]);
                if (I == 0) metadata_ = Check; else if (I == 1) verify_ = Check; else if (I == 2) acl_ = Check; else streams_ = Check; }
            Check->setToolTip(I == 4 ? tr("TODO: 링크 보존과 링크 대상 복사 정책은 미지원입니다. 링크가 있으면 실행 시 오류로 표시합니다.")
                : I == 5 ? tr("폴더는 필터와 무관하게 탐색하며 빈 폴더도 포함합니다.")
                : Supported ? tr("실행 시 파일 시스템·권한을 다시 확인합니다. 실패하면 기존 대상과 원본을 보존합니다. DACL·ADS는 파일에 적용합니다.")
                : tr("이 공급자 조합에서는 사용할 수 없습니다. 저장한 기본값은 유지됩니다."));
            Options->addWidget(Check);
        }
        BodyLayout()->addLayout(Options);
        auto* Rate = new QHBoxLayout; auto* RateLabel = new QLabel(tr("전송 속도 제한(&T)"), this);
        speed_ = new QSpinBox(this); speed_->setObjectName("hc.operation.speed"); speed_->setRange(0, 1048576); speed_->setSuffix(" MiB/s"); speed_->setSpecialValueText(tr("제한 없음"));
        speed_->setValue(Request.copyEngine_ ? Defaults["copySpeedMiB"].toInt(0) : 0); speed_->setEnabled(Request.copyEngine_); RateLabel->setBuddy(speed_);
        Rate->addWidget(RateLabel); Rate->addWidget(speed_); Rate->addStretch(); BodyLayout()->addLayout(Rate);
        auto* Completion = Label({}); Completion->setObjectName("hc.operation.completion");
        const auto UpdateCompletion = [this, Completion, Engine = Request.copyEngine_] {
            Completion->setText(!Engine ? tr("이 압축 공급자는 필터·속도 제한·추가 복사 옵션을 지원하지 않습니다.")
                : (VerifyCopy() ? tr("SHA-256 검증을 통과하면 대상 파일 저장을 완료합니다.")
                    : tr("해시 검증 없이 대상 파일 저장을 완료합니다."))
                    + tr(" 제외된 파일이 있는 이동은 원본 폴더를 남깁니다. 폴더 권한·링크 보존은 지원하지 않습니다."));
        };
        connect(verify_, &QCheckBox::toggled, this, UpdateCompletion);
        UpdateCompletion();
    }
    if (Copy) {
        auto* Queue = new fm::ui::Button(tr("대기열에 추가(&Q)"), this);
        Queue->setObjectName("hc.operation.queue");
        Queue->setToolTip(tr("실행하지 않고 접수합니다. 작업관리자에서 순서를 정한 뒤 시작하세요."));
        connect(Queue, &QPushButton::clicked, this, [this] {
            if (AcceptButton()->isEnabled()) { queued_ = true; accept(); }
        });
        qobject_cast<QHBoxLayout*>(AcceptButton()->parentWidget()->layout())->insertWidget(0, Queue);
    } else if (Mkdir) {
        Label(tr("만들어질 폴더"));
        plan_ = new fm::ui::FolderPlanView(this); BodyLayout()->addWidget(plan_);
        openCreated_ = new fm::ui::WrapCheckBox(tr("만든 후 마지막 폴더로 이동(&G)"), this);
        openCreated_->setObjectName("hc.operation.openCreated"); openCreated_->setEnabled(Request.nativeDestination_); BodyLayout()->addWidget(openCreated_);
        Label(tr("경로 구분자로 중첩 폴더를 만듭니다. 실패·취소 전에 만들어진 중간 폴더는 유지됩니다."));
    } else {
        if (Rename) {
            extension_ = new fm::ui::WrapCheckBox(tr("확장자 변경 허용(&X)"), this); BodyLayout()->addWidget(extension_);
            connect(extension_, &QCheckBox::toggled, this, [this] { validateInput(); });
        }

        Label(Rename ? tr("새 이름 또는 상대 경로를 입력합니다. 다른 폴더로의 변경은 이동 작업으로 처리합니다.")
            : tr("선택 항목을 대상 폴더로 이동합니다. 볼륨 간 이동은 복사 후 원본 제거로 처리될 수 있습니다."));
    }
    if (!Mkdir) {
        parents_ = new fm::ui::WrapCheckBox(tr("대상 폴더가 없으면 만들기(&K)"), this);
        parents_->setObjectName("hc.operation.parents"); parents_->setEnabled(Request.nativeDestination_); BodyLayout()->addWidget(parents_);
    }
    targetSummary_ = Label(Request.nativeDestination_ ? tr("대상 볼륨 조회 중…") : tr("공급자 대상은 실행 시 검사합니다."));
    targetSummary_->setObjectName("hc.operation.targetSummary");
    targetWatcher_ = new QFutureWatcher<QJsonObject>(this);
    probeTimer_ = new QTimer(this); probeTimer_->setSingleShot(true); probeTimer_->setInterval(150);
    connect(probeTimer_, &QTimer::timeout, this, [this] { probeTarget(); });
    connect(targetWatcher_, &QFutureWatcher<QJsonObject>::finished, this, [this] {
        const auto Result = targetWatcher_->result();
        if (Result["generation"].toVariant().toULongLong() != generation_) { probeTarget(); return; }
        targetSummary_->setText(Result["text"].toString());
        targetSummary_->setProperty("target", Result["target"].toString());
        if (plan_) {
            QList<fm::ui::FolderPlanView::Node> Nodes{{request_.source_, fm::ui::FolderPlanView::Node::Current}};
            for (const auto& Part : Result["plan"].toArray()) {
                const auto Node = Part.toObject();
                Nodes.append({Node["name"].toString(), Node["state"].toInt() == 0 ? fm::ui::FolderPlanView::Node::Existing : Node["state"].toInt() == 1 ? fm::ui::FolderPlanView::Node::New : fm::ui::FolderPlanView::Node::Invalid});
            }
            plan_->setPlan(Nodes);
        }
    });
    AcceptButton()->setText(Copy ? tr("복사(&C)") : Mkdir ? tr("만들기(&O)") : tr("확인(&O)"));
    BodyLayout()->addStretch();
    connect(input_, &fm::ui::PathEdit::pathChanged, this, [this] { validateInput(); });
    input_->lineEdit()->selectAll();
    if (Rename) {
        hc::ui::SelectRenameText(input_->lineEdit(), hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["renameSelectExtension"].toBool(false));
    }
    SetInitialInput(input_->lineEdit());
    validateInput();
}
QString FileOperationDialog::Input() const { return input_ ? input_->path() : QString{}; }
QString FileOperationDialog::Conflict() const { return conflict_ ? conflict_->currentData().toString() : QStringLiteral("ask"); }

FileOperationDialog::~FileOperationDialog() { stop_.request_stop(); }
bool FileOperationDialog::TargetIsName() const { return targetMode_ && targetMode_->currentIndex() == 1; }
bool FileOperationDialog::PreserveMetadata() const { return metadata_ && metadata_->isEnabled() && metadata_->isChecked(); }
bool FileOperationDialog::VerifyCopy() const { return verify_ && verify_->isEnabled() && verify_->isChecked(); }
bool FileOperationDialog::CopyAcl() const { return acl_ && acl_->isEnabled() && acl_->isChecked(); }
bool FileOperationDialog::CopyStreams() const { return streams_ && streams_->isEnabled() && streams_->isChecked(); }
QString FileOperationDialog::IncludePattern() const { return filter_ && filter_->isEnabled() ? filter_->text().trimmed() : "*"; }
quint64 FileOperationDialog::SpeedLimit() const { return speed_ && speed_->isEnabled() ? quint64(speed_->value()) * 1024 * 1024 : 0; }
bool FileOperationDialog::CreateParents() const { return request_.nativeDestination_ && (request_.command_.endsWith("mkdir") || (parents_ && parents_->isChecked())); }
bool FileOperationDialog::OpenCreatedFolder() const { return openCreated_ && openCreated_->isEnabled() && openCreated_->isChecked(); }
bool FileOperationDialog::Recycle() const { return recycle_ && recycle_->isEnabled() && recycle_->isChecked(); }
bool FileOperationDialog::AllowReadOnly() const { return readonly_ && readonly_->isEnabled() && readonly_->isChecked(); }
QString FileOperationDialog::ResolvedTarget() const
{
    const auto Value = QDir::fromNativeSeparators(Input());
    if (!request_.nativeDestination_) return Value;
    return QDir::toNativeSeparators(ResolveNativeDestination(Value, request_.relativeBase_.isEmpty() ? request_.source_ : request_.relativeBase_));
}
QStringList FileOperationDialog::RecentTargets(const QString& Provider)
{
    QStringList Targets;
    const auto Values = hc::settings::AppSettings().Read(hc::settings::Section::Options).toObject()["recentOperationTargets"].toObject()[Provider].toArray();
    for (const auto& Value : Values) if (Value.isString() && !Value.toString().isEmpty() && !Targets.contains(Value.toString()) && Targets.size() < 10) Targets.append(Value.toString());
    return Targets;
}
bool FileOperationDialog::RememberTarget(const QString& Provider, const QString& Target)
{
    if (Provider.isEmpty() || Target.isEmpty()) return false;
    auto& Settings = hc::settings::AppSettings();
    if (Settings.Editing()) return false;
    auto Targets = RecentTargets(Provider); Targets.removeAll(Target); Targets.prepend(Target);
    while (Targets.size() > 10) Targets.removeLast();
    auto Options = Settings.Read(hc::settings::Section::Options).toObject();
    auto History = Options["recentOperationTargets"].toObject(); History[Provider] = QJsonArray::fromStringList(Targets); Options["recentOperationTargets"] = History;
    const auto Before = Settings.Object();
    if (Settings.Put(hc::settings::Section::Options, Options)) return true;
    // A failed optional history save must not leave an unsaved settings draft.
    Settings.BeginEdit(); Settings.Replace(Before); Settings.DiscardEdit(); return false;
}
void FileOperationDialog::validateInput()
{
    const auto Value = Input();
    const bool Mkdir = request_.command_.endsWith("mkdir");
    const bool Rename = request_.command_.endsWith("rename");
    const bool NameOnly = Rename || Mkdir;
    bool Valid = !Value.trimmed().isEmpty() && !Value.contains(QChar::Null);
    QString Error;
    if (NameOnly && !(request_.nativeDestination_ ? (Mkdir ? ValidRelativePath(Value) : ValidRelativePath(Value) || QDir::isAbsolutePath(Value)) : ValidLeaf(Value))) {
        Valid = false; Error = tr("유효한 이름 또는 경로를 입력하세요. 예약 이름·잘못된 문자·끝 공백은 사용할 수 없습니다.");
    }
    if (request_.nativeDestination_ && Valid) {
        const auto Target = ResolvedTarget();
        if (Target.isEmpty()) { Valid = false; Error = tr("유효한 대상 경로를 입력하세요. 상대경로는 현재 작업 폴더 기준입니다."); }
        // QFileInfo path parsing performs no filesystem access here.
        if (TargetIsName() || Rename || Mkdir) {
            if (!ValidLeaf(QFileInfo(Target).fileName())) { Valid = false; Error = tr("대상의 마지막 이름이 유효하지 않습니다."); }
        }
    }
    if (extension_ && !extension_->isChecked() && QFileInfo(Value).suffix().compare(QFileInfo(request_.initialValue_).suffix(), Qt::CaseInsensitive) != 0) {
        Valid = false; Error = tr("확장자를 바꾸려면 확장자 변경 허용을 선택하세요.");
    }
    if (Rename && QDir::cleanPath(Value) == QDir::cleanPath(request_.initialValue_)) { Valid = false; Error = tr("원본과 다른 이름을 입력하세요."); }
    preview_->setText(tr("대상: %1\n존재 여부·권한·충돌은 실행 시 다시 확인합니다.").arg(ResolvedTarget()));
    if (plan_) {
        QList<fm::ui::FolderPlanView::Node> Nodes{{request_.source_, fm::ui::FolderPlanView::Node::Current}};
        for (const auto& Part : PathParts(Value)) Nodes.append({Part, Valid ? fm::ui::FolderPlanView::Node::New : fm::ui::FolderPlanView::Node::Invalid});
        plan_->setPlan(Nodes);
    }
    if (filter_ && filter_->isEnabled() && !hc::operations::IsValidCopyPattern(IncludePattern().toStdString())) {
        Valid = false; Error = tr("파일명 패턴을 확인하세요. * · ?와 ; 구분을 사용하며 경로와 빈 패턴은 허용하지 않습니다.");
    }
    SetAcceptEnabled(Valid);
    SetMessage(Valid ? QString{} : Error.isEmpty() ? tr("이름 또는 경로를 입력하세요.") : Error, !Valid);
    ++generation_;
    if (probeTimer_ && request_.nativeDestination_) { targetSummary_->setText(tr("대상 볼륨 조회 중…")); probeTimer_->start(); }
}
void FileOperationDialog::probeTarget()
{
    if (targetWatcher_->isRunning() || stop_.stop_requested()) return;
    const auto Target = ResolvedTarget();
    const bool Folder = !TargetIsName() && !request_.command_.endsWith("rename") && !request_.command_.endsWith("mkdir");
    targetWatcher_->setFuture(QtConcurrent::run([Target, Folder, Source = request_.source_, Parts = PathParts(Input()), Mkdir = bool(plan_), Generation = generation_, Stop = stop_.get_token()] {
        QJsonObject Result{{"target", Target}, {"generation", qint64(Generation)}};
        if (Stop.stop_requested()) return Result;
        auto Existing = Folder ? Target : QFileInfo(Target).absolutePath();
        for (int Depth = 0; Depth < 128 && !QFileInfo::exists(Existing); ++Depth) {
            if (Stop.stop_requested()) return Result;
            const auto Parent = QFileInfo(Existing).absolutePath(); if (Parent == Existing) break; Existing = Parent;
        }
        const QStorageInfo Volume(Existing), SourceVolume(Source);
        if (Volume.isValid() && Volume.isReady()) {
            Result["text"] = QObject::tr("볼륨: %1 · %2\n사용 가능: %3 · %4\n조회 시점의 참고 정보입니다. 쓰기 시 공간을 다시 확인합니다.")
                .arg(Volume.rootPath(), QString::fromUtf8(Volume.fileSystemType()), QLocale().formattedDataSize(Volume.bytesAvailable()),
                    SourceVolume.isValid() && SourceVolume.device() == Volume.device() ? QObject::tr("같은 볼륨") : QObject::tr("다른 볼륨 또는 원본 볼륨 미확인"));
        } else Result["text"] = QObject::tr("대상 볼륨·사용 가능 공간을 조회하지 못했습니다. 실행 시 검사합니다.");
        if (Mkdir) {
            QJsonArray Plan; auto Path = Source;
            for (const auto& Part : Parts) {
                if (Stop.stop_requested()) return Result;
                Path = QDir(Path).filePath(Part); const QFileInfo Info(Path);
                Plan.append(QJsonObject{{"name", Part}, {"state", Info.exists() ? Info.isDir() ? 0 : 2 : 1}});
            }
            Result["plan"] = Plan;
        }
        return Result;
    }));
}
void FileOperationDialog::summarizeSource()
{
    if (request_.command_.endsWith("mkdir")) { sourceSummary_->hide(); return; }
    if (!request_.nativeSource_) return;
    if (request_.manifestSelection_) { sourceSummary_->setText(tr("선택 목록 전체 · 재귀 요약은 실행 계획에서 확인합니다.")); return; }
    auto* Watcher = new QFutureWatcher<QJsonObject>(this);
    connect(Watcher, &QFutureWatcher<QJsonObject>::finished, this, [this, Watcher] { sourceSummary_->setText(Watcher->result()["text"].toString()); Watcher->deleteLater(); });
    Watcher->setFuture(QtConcurrent::run([Source = request_.source_, Names = request_.names_, Stop = stop_.get_token()] {
        quint64 Files = 0, Folders = 0, Bytes = 0; bool Partial = false;
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        const auto Count = [&](const QFileInfo& Info) {
            if (!Info.exists() || !Info.isReadable()) { Partial = true; return; }
            if (Info.isDir()) ++Folders; else { ++Files; Bytes += quint64(qMax<qint64>(0, Info.size())); }
        };
        for (const auto& Name : Names) {
            if (Stop.stop_requested()) return QJsonObject{};
            const QFileInfo Top(QDir(Source).filePath(Name)); Count(Top);
            if (Top.isDir() && !Top.isSymLink()) {
                QDirIterator Items(Top.filePath(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
                while (Items.hasNext()) {
                    if (Stop.stop_requested()) return QJsonObject{};
                    if (Files + Folders >= 50000 || std::chrono::steady_clock::now() >= Deadline) { Partial = true; break; }
                    Items.next(); Count(Items.fileInfo());
                }
            }
            if (Partial && std::chrono::steady_clock::now() >= Deadline) break;
        }
        return QJsonObject{{"text", QObject::tr("파일 %1개 · 폴더 %2개 · %3%4\n조회 시점의 요약입니다. 링크 대상은 재귀 탐색하지 않습니다.").arg(Files).arg(Folders).arg(QLocale().formattedDataSize(qint64(Bytes)), Partial ? QObject::tr(" 이상 · 일부 조회") : QString{})}};
    }));
}
} // namespace hc::ui


namespace hc::ui {
ArchiveExtractDialog::ArchiveExtractDialog(const QStringList& Names, const QString& Destination, bool InsideArchive, QWidget* Parent, const QString& BaseDirectory)
    : DialogShell(tr("파일 압축 해제"), Parent), baseDirectory_(BaseDirectory.isEmpty() ? Destination : BaseDirectory) {
    setObjectName("hc.archive.extract"); resize(720, 500);
    SetSummary(InsideArchive ? tr("압축 파일 안에서 선택한 항목을 꺼냅니다.") : tr("선택한 압축 파일을 대상 폴더에 풉니다."));
    auto* Card = new fm::ui::Card(this); auto* Form = new QFormLayout(Card); Form->setRowWrapPolicy(QFormLayout::WrapLongRows); BodyLayout()->addWidget(Card);
    auto* Sources = new QLabel(Names.join("\n"), Card); Sources->setTextFormat(Qt::PlainText); Sources->setWordWrap(true); Sources->setObjectName("hc.archive.extract.sources"); Form->addRow(tr("원본"), Sources);
    auto* PathRow = new QHBoxLayout; destination_ = new QLineEdit(Destination, Card); destination_->setObjectName("hc.archive.extract.path"); destination_->setAccessibleName(tr("압축 해제 대상 폴더")); PathRow->addWidget(destination_, 1);
    auto* Browse = new fm::ui::Button(tr("찾아보기(&B)"), Card); Browse->setAutoDefault(false); PathRow->addWidget(Browse);
    auto* DestinationLabel = new QLabel(tr("대상 폴더(&D)"), Card); DestinationLabel->setBuddy(destination_); Form->addRow(DestinationLabel, PathRow);
    connect(Browse, &QPushButton::clicked, this, [this] { const auto Path = QFileDialog::getExistingDirectory(this, tr("압축 해제 대상 폴더"), this->Destination()); if (!Path.isEmpty()) destination_->setText(Path); });
    separate_ = new QCheckBox(tr("압축 파일 이름으로 각각 하위 폴더 만들기(&F)"), Card); separate_->setObjectName("hc.archive.extract.separate"); separate_->setChecked(!InsideArchive); separate_->setEnabled(!InsideArchive); Form->addRow(separate_);
    conflict_ = new QComboBox(Card); conflict_->setObjectName("hc.archive.extract.conflict"); conflict_->addItem(tr("묻기"), "ask"); conflict_->addItem(tr("덮어쓰기"), "replace"); conflict_->addItem(tr("건너뛰기"), "skip"); Form->addRow(tr("같은 이름의 파일"), conflict_);
    queued_ = new QCheckBox(tr("대기열에 추가(&Q)"), Card); queued_->setObjectName("hc.archive.extract.queued"); Form->addRow(queued_);
    auto* Resolved = new QLabel(this); Resolved->setObjectName("hc.archive.extract.resolved"); Resolved->setWordWrap(true); Resolved->setTextFormat(Qt::PlainText); BodyLayout()->addWidget(Resolved);
    const auto UpdateResolved = [this, Resolved] { Resolved->setText(tr("상대경로 기준: %1\n해석된 대상: %2").arg(baseDirectory_, this->Destination())); };
    connect(destination_, &QLineEdit::textChanged, this, UpdateResolved); UpdateResolved();
    auto* Note = new QLabel(tr("원본 압축 파일은 유지합니다. 하위 폴더 구조를 보존하며 암호가 필요하면 작업 관리자에서 입력합니다. 백그라운드 작업의 일시 정지·취소도 작업 관리자에서 처리합니다."), this); Note->setWordWrap(true); BodyLayout()->addWidget(Note); BodyLayout()->addStretch();
    AcceptButton()->setText(tr("압축 해제(&S)")); connect(destination_, &QLineEdit::textChanged, this, [this] { validate(); });
    destination_->selectAll(); SetInitialInput(destination_); validate();
}
QString ArchiveExtractDialog::Destination() const { return ResolveNativeDestination(destination_->text(), baseDirectory_); }
QString ArchiveExtractDialog::Conflict() const { return conflict_->currentData().toString(); }
bool ArchiveExtractDialog::SeparateFolders() const { return separate_->isChecked(); }
bool ArchiveExtractDialog::Queued() const { return queued_->isChecked(); }
void ArchiveExtractDialog::validate() {
    const bool Valid = !Destination().isEmpty();
    SetAcceptEnabled(Valid); SetMessage(Valid ? QString() : tr("대상 경로를 입력하세요. . · .. 상대경로는 현재 작업 폴더 기준입니다."), !Valid);
}
}
