#include "stdafx.h"
#include "linkCreationDialog.hpp"

/// 모듈
/// UIs
#include "nativePathCompletion.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/qtDialogMnemonics.hpp"
/// 공통 라이브러리
#include "cmnIncludeLibrary.hpp"
#include "commonLibs/Qt/elidedLabel.hpp"
/// 외부 라이브러리
#include <fmwidgets/DialogWidgets.h>
#include <fmwidgets/Button.h>
#include <fmwidgets/WrapCheckBox.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QTimer>
#include <QShortcut>
#include <QLoggingCategory>
#include <QtConcurrent/QtConcurrentRun>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(linkDialogLog, "hc.ui.links")

namespace hc::ui {
LinkCreationDialog::LinkCreationDialog(links::Request Request, int DirectoryCount, const QString& RelativeBase, QWidget* Parent)
    : DialogShell(tr("링크 만들기"), Parent), request_(std::move(Request)), base_(RelativeBase) {
    setObjectName("hc.mklink.dialog"); EnableCompactLayout(720);
    SetSummary(tr("선택한 항목의 링크를 대상 위치에 만듭니다."));
    auto* Summary = new QLabel(tr("파일 %1개 · 폴더 %2개").arg(request_.sources_.size() - DirectoryCount).arg(DirectoryCount), this);
    BodyLayout()->addWidget(Summary);
    auto* Files = new hc::qt::ElidedLabel(this);
    Files->SetLineCount(qMin(2, request_.sources_.size()));
    Files->setText(tr("원본: %1").arg(QDir::toNativeSeparators(request_.sources_.mid(0, 2).join('\n')))); Files->setToolTip(request_.sources_.join('\n'));
    BodyLayout()->addWidget(Files);
    auto* Form = new QGridLayout; Form->setColumnMinimumWidth(0, 96); Form->setColumnStretch(1, 1);
    Form->setHorizontalSpacing(12); Form->setVerticalSpacing(8); BodyLayout()->addLayout(Form);
    auto* Types = new QWidget(this); auto* TypeLayout = new QHBoxLayout(Types); TypeLayout->setContentsMargins(0, 0, 0, 0);
    auto* Group = new QButtonGroup(this);
    const auto AddType = [&](const QString& Text, const char* Name) {
        auto* Button = new QRadioButton(Text, Types); Button->setObjectName(Name); TypeLayout->addWidget(Button); Group->addButton(Button);
        connect(Button, &QRadioButton::toggled, this, [this] { changed(); }); return Button;
    };
    if (!DirectoryCount) hard_ = AddType(tr("하드링크(&H)"), "hc.mklink.hard");
    symbolic_ = AddType(tr("소프트링크(&S)"), "hc.mklink.symbolic");
    if (DirectoryCount == request_.sources_.size()) junction_ = AddType(tr("정션(&J)"), "hc.mklink.junction");
    TypeLayout->addStretch(); Form->addWidget(new QLabel(tr("링크 종류"), this), 0, 0); Form->addWidget(Types, 0, 1, 1, 2);
    description_ = new QLabel(this); description_->setWordWrap(true); Form->addWidget(description_, 1, 1, 1, 2);
    path_ = new fm::ui::PathEdit(this); path_->setObjectName("hc.mklink.path");
    path_->lineEdit()->setObjectName("hc.mklink.editor"); path_->lineEdit()->setAccessibleName(tr("대상 경로"));
    path_->setHistoryVisible(false); path_->setBrowseVisible(true); path_->setPath(QDir::toNativeSeparators(request_.destination_));
    for (auto* Button : path_->findChildren<fm::ui::Button*>()) if (Button->isVisibleTo(path_)) {
        Button->setGlyph(fm::ui::glyph::None); Button->setIcon(QIcon{}); Button->setText(tr("찾기(&B)…")); Button->setToolTip(tr("폴더 찾아보기 · Alt+B"));
    }
    auto* Label = new QLabel(tr("대상 경로(&D)"), this); Label->setBuddy(path_->lineEdit()); Form->addWidget(Label, 2, 0); Form->addWidget(path_, 2, 1);
    mode_ = new QComboBox(this); mode_->setObjectName("hc.mklink.mode"); mode_->setAccessibleName(tr("대상 경로 해석 방식"));
    // QComboBox item text does not process '&'; use a buddy label for its mnemonic.
    auto* ModeBox = new QWidget(this); auto* ModeLayout = new QVBoxLayout(ModeBox); ModeLayout->setContentsMargins(0, 0, 0, 0);
    auto* ModeLabel = new QLabel(tr("해석 방식(&M)"), ModeBox); ModeLabel->setBuddy(mode_); ModeLayout->addWidget(ModeLabel); ModeLayout->addWidget(mode_);
    mode_->addItem(tr("자동"), "auto"); mode_->addItem(tr("폴더로"), "folder");
    connect(path_, &fm::ui::PathEdit::browseRequested, this, [this] {
        const auto Folder = QFileDialog::getExistingDirectory(this, tr("링크를 만들 폴더"), this->Request().destination_);
        if (!Folder.isEmpty()) { mode_->setCurrentIndex(1); path_->setPath(QDir::toNativeSeparators(Folder) + '\\'); }
    });
    if (request_.sources_.size() == 1) mode_->addItem(tr("새 이름으로"), "name");
    Form->addWidget(ModeBox, 2, 2);
    preview_ = new QLabel(this); preview_->setObjectName("hc.mklink.preview"); preview_->setWordWrap(true); preview_->setTextFormat(Qt::PlainText); Form->addWidget(preview_, 3, 1, 1, 2);
    parents_ = new fm::ui::WrapCheckBox(tr("대상 폴더가 없으면 만들기(&K)"), this); parents_->setObjectName("hc.mklink.parents"); parents_->setChecked(true); Form->addWidget(parents_, 4, 1, 1, 2);
    reference_ = new QWidget(this); auto* ReferenceLayout = new QVBoxLayout(reference_); ReferenceLayout->setContentsMargins(0, 0, 0, 0);
    auto* RefRow = new QHBoxLayout; RefRow->addWidget(new QLabel(tr("원본 참조"), reference_));
    absolute_ = new QRadioButton(tr("절대경로(&A)"), reference_); relative_ = new QRadioButton(tr("상대경로(&R)"), reference_);
    absolute_->setObjectName("hc.mklink.absolute"); relative_->setObjectName("hc.mklink.relative"); absolute_->setChecked(true);
    RefRow->addWidget(absolute_); RefRow->addWidget(relative_); RefRow->addStretch(); ReferenceLayout->addLayout(RefRow);
    auto* Hint = new QLabel(tr("상대경로는 새 링크의 부모 폴더를 기준으로 원본을 가리킵니다."), reference_); Hint->setWordWrap(true); ReferenceLayout->addWidget(Hint);
    Form->addWidget(reference_, 5, 1, 1, 2);
    conflict_ = new QComboBox(this); conflict_->setObjectName("hc.mklink.conflict");
    conflict_->addItem(tr("매번 묻기"), "ask"); conflict_->addItem(tr("건너뛰기"), "skip"); conflict_->addItem(tr("자동 이름 변경"), "rename");
    auto* PolicyLabel = new QLabel(tr("같은 이름이 있으면(&E)"), this); PolicyLabel->setBuddy(conflict_); PolicyLabel->setWordWrap(true);
    Form->addWidget(PolicyLabel, 6, 0); Form->addWidget(conflict_, 6, 1, 1, 2);
    summary_ = new QLabel(this); summary_->setObjectName("hc.mklink.summary"); summary_->setWordWrap(true); BodyLayout()->addWidget(summary_);
    AcceptButton()->setText(tr("만들기(&O)")); CancelButton()->setText(tr("취소(&C)")); SetInitialInput(path_->lineEdit());
    InstallNativePathCompletion(path_->lineEdit(), base_, true);
    const auto InitialPath = QDir::fromNativeSeparators(request_.destination_);
    const auto Recent = FileOperationDialog::RecentTargets(InitialPath.startsWith("//") ? "smb" : "local");
    for (int Index = 0; Index < qMin(3, Recent.size()); ++Index) {
        auto* Shortcut = new QShortcut(QKeySequence(QStringLiteral("Alt+%1").arg(Index + 1)), this);
        connect(Shortcut, &QShortcut::activated, this, [this, Target = Recent[Index]] { path_->setPath(Target); });
    }
    timer_ = new QTimer(this); timer_->setSingleShot(true); timer_->setInterval(180);
    watcher_ = new QFutureWatcher<QJsonObject>(this);
    connect(timer_, &QTimer::timeout, this, [this] { probe(); });
    connect(watcher_, &QFutureWatcher<QJsonObject>::finished, this, [this] {
        const auto Result = watcher_->result();
        if (Result["generation"].toVariant().toULongLong() != generation_) { timer_->start(); return; }
        targetIsName_ = Result["name"].toBool();
        const auto Error = Result["error"].toString(); preview_->setText(Result["preview"].toString());
        SetMessage(Error, !Error.isEmpty()); SetAcceptEnabled(Error.isEmpty()); FitToContents();
    });
    connect(path_->lineEdit(), &QLineEdit::textChanged, this, [this] { changed(); });
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    connect(parents_, &QCheckBox::toggled, this, [this] { changed(); });
    connect(relative_, &QRadioButton::toggled, this, [this] { changed(); });
    if (hard_) hard_->setChecked(true); else if (junction_) junction_->setChecked(true); else symbolic_->setChecked(true);
    changed(); FitToContents();
}
links::Request LinkCreationDialog::Request() const {
    auto Result = request_; Result.destination_ = ResolveNativeDestination(path_->lineEdit()->text(), base_);
    Result.kind_ = hard_ && hard_->isChecked() ? links::Kind::Hard : junction_ && junction_->isChecked() ? links::Kind::Junction : links::Kind::Symbolic;
    Result.relative_ = Result.kind_ == links::Kind::Symbolic && relative_->isChecked(); Result.createParents_ = parents_->isChecked();
    Result.targetIsName_ = targetIsName_; Result.conflict_ = conflict_->currentData().toString(); return Result;
}
LinkCreationDialog::~LinkCreationDialog() { stop_.request_stop(); }
void LinkCreationDialog::changed() {
    if (!timer_) return;
    stop_.request_stop(); stop_ = std::stop_source{};
    ++generation_; SetAcceptEnabled(false); SetMessage(tr("경로 확인 중…"));
    reference_->setVisible(symbolic_->isChecked());
    summary_->setText(tr("링크 %1개 · %2 · 원본 유지").arg(request_.sources_.size()).arg(links::KindName(Request().kind_)));
    description_->setText(hard_ && hard_->isChecked() ? tr("같은 파일 데이터를 공유합니다. 한쪽에서 내용을 수정하면 함께 바뀝니다.")
        : junction_ && junction_->isChecked() ? tr("로컬 폴더를 연결합니다. 다른 로컬 볼륨의 폴더도 연결할 수 있습니다.")
        : tr("원본 경로를 가리킵니다. 원본을 옮기면 연결이 끊어질 수 있습니다."));
    timer_->start();
}
void LinkCreationDialog::probe() {
    if (watcher_->isRunning()) return;
    const auto Value = Request(); const auto Input = path_->lineEdit()->text(); const auto Mode = mode_->currentData().toString();
    const auto Generation = generation_; const auto Base = base_; const auto Stop = stop_.get_token();
    watcher_->setFuture(QtConcurrent::run([Value, Input, Mode, Generation, Base, Stop] {
        QJsonObject Result{{"generation", qint64(Generation)}}; QString Error, Preview;
        const QFileInfo Target(Value.destination_);
        const bool Name = Value.sources_.size() == 1 && !Input.endsWith('/') && !Input.endsWith('\\')
            && (Mode == "name" || (Mode == "auto" && !Target.isDir()));
        Result["name"] = Name;
        if (Input.trimmed().isEmpty() || Value.destination_.isEmpty() || (Base.isEmpty() && QDir::isRelativePath(Input))) Error = QObject::tr("링크를 만들 실제 대상 경로를 입력하세요.");
        else if (!Name && Target.exists() && !Target.isDir()) Error = QObject::tr("대상 폴더 경로에 파일이 있습니다.");
        else if (!Value.createParents_ && !(Name ? QFileInfo(Target.absolutePath()).isDir() : Target.isDir())) Error = QObject::tr("대상 폴더가 없습니다. 폴더 생성 옵션을 선택하세요.");
        for (const auto& Source : Value.sources_) {
            if (!Error.isEmpty() || Stop.stop_requested()) break;
            const auto Destination = Name ? Value.destination_ : QDir(Value.destination_).filePath(QFileInfo(Source).fileName());
            const auto Code = links::Validate(Source, Destination, Value.kind_, Value.relative_);
            if (Code == ERROR_NOT_SAME_DEVICE) Error = Value.kind_ == links::Kind::Hard
                ? QObject::tr("하드링크는 같은 볼륨에만 만들 수 있습니다. 소프트링크를 선택하세요.") : QObject::tr("이 위치에서는 상대경로로 원본을 참조할 수 없습니다.");
            else if (Code) Error = hc::ui::DescribeWin32Error(Code, QObject::tr("링크 경로 확인"), Source).Text();
            if (Preview.count('\n') < 3) {
                if (!Preview.isEmpty()) Preview += '\n';
                Preview += QObject::tr("링크 생성: %1").arg(QDir::toNativeSeparators(Destination));
                if (Value.relative_) Preview += QObject::tr(" · 원본 참조: %1").arg(QDir::toNativeSeparators(QDir(QFileInfo(Destination).absolutePath()).relativeFilePath(Source)));
            }
        }
        if (!Input.isEmpty() && QDir::isRelativePath(Input)) Preview += QObject::tr("\n상대경로 기준: %1").arg(QDir::toNativeSeparators(Base));
        Result["error"] = Error; Result["preview"] = Preview; return Result;
    }));
}
}
