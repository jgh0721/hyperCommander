#include "stdafx.h"
#include "fileJobInteraction.hpp"

/// UIs
#include "qtDialogFrame.hpp"

/// 고유 라이브러리
#include "fileIconService.hpp"
#include "fileBatchOperations.hpp"

/// 플랫폼/프레임워크 헤더파일
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QLabel>
#include <QKeyEvent>
#include <QShowEvent>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QScreen>
#include <QResizeEvent>
#include <QShortcut>
#include <QThreadPool>
#include <QtConcurrent>
#include <limits>
#include <fmwidgets/Button.h>
#include <fmwidgets/DialogFooter.h>

namespace hc::ui
{
    using namespace operations;
    namespace
    {
        // Like the shared wrap layout, but every visual row ends at the right edge.
        class ActionLayout final : public QLayout
        {
        public:
            explicit ActionLayout(QWidget* Parent) : QLayout(Parent) { setContentsMargins(0, 0, 0, 0); setSpacing(8); }
            ~ActionLayout() override { while (auto* Item = takeAt(0)) delete Item; }
            void addItem(QLayoutItem* Item) override { items_.append(Item); invalidate(); }
            int count() const override { return int(items_.size()); }
            QLayoutItem* itemAt(int Index) const override { return items_.value(Index); }
            QLayoutItem* takeAt(int Index) override { if (Index < 0 || Index >= count()) return nullptr; auto* Item = items_.takeAt(Index); invalidate(); return Item; }
            Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }
            bool hasHeightForWidth() const override { return true; }
            int heightForWidth(int Width) const override { return arrange(QRect(0, 0, Width, 0), false); }
            QSize minimumSize() const override { QSize Size; for (auto* Item : items_) if (!Item->isEmpty()) Size = Size.expandedTo(Item->minimumSize()); return Size; }
            QSize sizeHint() const override { return minimumSize(); }
            void setGeometry(const QRect& Rect) override { QLayout::setGeometry(Rect); arrange(Rect, true); }
        private:
            int arrange(const QRect& Rect, bool Apply) const
            {
                int Top = Rect.top(), Width = 0, Height = 0;
                QList<QLayoutItem*> Row;
                const auto Flush = [&] {
                    int Left = Rect.right() + 1 - Width;
                    for (auto* Item : Row) {
                        const auto Size = Item->sizeHint().expandedTo(Item->minimumSize()).boundedTo(Item->maximumSize());
                        if (Apply) Item->setGeometry(QRect(Left, Top, Size.width(), Height));
                        Left += Size.width() + spacing();
                    }
                };
                for (auto* Item : items_) {
                    if (Item->isEmpty()) continue;
                    const auto Size = Item->sizeHint().expandedTo(Item->minimumSize()).boundedTo(Item->maximumSize());
                    if (Width && Width + spacing() + Size.width() > Rect.width()) { Flush(); Top += Height + spacing(); Row.clear(); Width = Height = 0; }
                    if (Width) Width += spacing(); Width += Size.width(); Height = qMax(Height, Size.height()); Row.append(Item);
                }
                Flush(); return Top - Rect.top() + Height;
            }
            QList<QLayoutItem*> items_;
        };
        QString Display(const std::string& Value)
        {
            const auto At = storage::ParseLocator(Value);
            if (!At.HasValue()) return QString::fromStdString(Value);
            auto Result = QString::fromStdString(At.Value().root_);
            for (const auto& Hop : At.Value().nesting_) Result += " > " + QString::fromStdString(Hop.entry_);
            return Result;
        }
        QString Native(const std::string& Value)
        {
            const auto At = storage::ParseLocator(Value);
            if (At.HasValue()) {
                if (At.Value().kind_ != storage::LocatorKind::Local && At.Value().kind_ != storage::LocatorKind::Smb) return {};
                return QString::fromStdString(At.Value().root_);
            }
            const auto Path = QString::fromStdString(Value);
            return Path.startsWith("\\\\") || (Path.size() > 2 && Path[1] == ':') ? Path : QString{};
        }
        struct Preview { QString facts_; QImage image_; };
        QThreadPool* PreviewPool()
        {
            static QPointer<QThreadPool> Pool;
            if (!Pool) { Pool = new QThreadPool(qApp); Pool->setMaxThreadCount(2); Pool->setObjectName("hc.jobs.previewPool"); }
            return Pool;
        }
    }

    FileJobInteraction::FileJobInteraction(JobSnapshot Snapshot, QString Operation, Reply Answer,
        std::function<void()> SavedPasswords, std::function<void()> Details, QWidget* Parent)
        : QDialog(Parent), snapshot_(std::move(Snapshot)), answer_(std::move(Answer)), savedPasswords_(std::move(SavedPasswords)), details_(std::move(Details))
    {
        setObjectName("hc.jobs.interaction"); setProperty("hc.jobs.id", QString::fromStdString(snapshot_.id_));
        setProperty("hc.jobs.requestId", QString::fromStdString(snapshot_.interaction_->id_));
        setWindowModality(Qt::NonModal); setAttribute(Qt::WA_DeleteOnClose);
        auto Font = font(); Font.setPointSizeF(12); setFont(Font);
        const auto& Request = *snapshot_.interaction_;
        if (Operation.isEmpty()) Operation = tr("파일 작업");
        const auto Kind = Request.kind_;
        const bool Password = Kind == InteractionKind::Password;
        const auto Source = !Request.source_.empty() ? Request.source_ : !snapshot_.progress_.source_.empty() ? snapshot_.progress_.source_ : snapshot_.metadata_.source_;
        const auto Destination = !Request.destination_.empty() ? Request.destination_ : !snapshot_.progress_.destination_.empty() ? snapshot_.progress_.destination_ : snapshot_.metadata_.destination_;
        originalName_ = QFileInfo(Display(Destination)).fileName();
        headingText_ = Kind == InteractionKind::Overwrite ? tr("대상에 같은 이름의 항목이 있습니다.")
            : Kind == InteractionKind::Rename ? tr("사용할 이름이 이미 있습니다. 다른 이름을 지정하세요.")
            : Password ? tr("압축 파일의 암호를 입력하세요.")
            : Kind == InteractionKind::DeleteConfirmation ? Request.reason_ == "confirm-recycle-delete" ? tr("선택한 항목을 휴지통으로 이동하시겠습니까?") : tr("선택한 항목을 삭제하시겠습니까?")
            : Kind == InteractionKind::ReadOnlyRemoval ? tr("읽기 전용 항목의 삭제를 허용하시겠습니까?")
            : Kind == InteractionKind::Elevation ? has(InteractionChoice::CopyOnly) ? tr("대상에 저장했지만 원본을 제거하지 못했습니다.") : tr("작업에 필요한 권한이 없습니다.")
            : tr("작업을 계속할 수 없습니다.");
        messageText_ = Kind == InteractionKind::Overwrite ? tr("원본과 기존 대상을 확인하고 처리 방법을 선택하세요.")
            : Kind == InteractionKind::DeleteConfirmation ? Request.reason_ == "confirm-recycle-delete" ? tr("Windows 휴지통으로 이동합니다.") : tr("휴지통으로 이동하지 않습니다. 이미 삭제한 항목은 취소로 복원되지 않습니다.")
            : Kind == InteractionKind::ReadOnlyRemoval ? tr("모두 허용은 현재 작업 안에서만 적용됩니다.")
            : Password ? Request.passwordAttempt_ > 1 ? tr("암호가 맞지 않습니다. 다시 입력하거나 저장된 암호를 선택하세요.") : tr("입력한 암호는 작업 기록에 표시하지 않습니다.")
            : Kind == InteractionKind::RetryFailure && Request.reason_.starts_with("file-in-use:") ? tr("다른 프로그램에서 항목을 사용하고 있습니다. 파일을 닫은 뒤 다시 시도하세요.")
            : Kind == InteractionKind::Elevation ? tr("허용된 방법으로 다시 시도하거나 작업을 취소하세요.") : QString::fromStdString(Request.reason_);
        setWindowTitle(Operation + (Kind == InteractionKind::Overwrite || Kind == InteractionKind::Rename ? tr(" · 이름 충돌") : Password ? tr(" · 암호 입력") : tr(" · 응답 필요")));
        auto* Layout = new QVBoxLayout(this); Layout->setSpacing(16); Layout->setAlignment(Qt::AlignTop);
        heading_ = new QLabel(headingText_, this); heading_->setObjectName("hc.jobs.interaction.heading"); heading_->setWordWrap(true); heading_->setTextFormat(Qt::PlainText);
        auto HeadingFont = Font; HeadingFont.setWeight(QFont::DemiBold); heading_->setFont(HeadingFont); Layout->addWidget(heading_);
        message_ = new QLabel(messageText_, this); message_->setWordWrap(true); message_->setTextFormat(Qt::PlainText); Layout->addWidget(message_);
        content_ = new QWidget(this); body_ = new QVBoxLayout(content_); body_->setContentsMargins(0, 0, 0, 0); body_->setSpacing(14); Layout->addWidget(content_);
        if (!Source.empty()) endpoint(tr("원본"), Source, "source");
        if (!Destination.empty() && Source != Destination) endpoint(tr("대상"), Destination, "destination");
        if (snapshot_.progress_.plannedItems_ && Kind == InteractionKind::DeleteConfirmation) {
            auto* Count = new QLabel(tr("선택한 항목 %1개").arg(*snapshot_.progress_.plannedItems_), content_); body_->addWidget(Count);
        }
        auto* NameLabel = new QLabel(Password ? tr("암호(&P)") : tr("새 이름(&N)"), content_);
        editor_ = new QLineEdit(content_); editor_->setObjectName("hc.jobs.newName"); editor_->setAccessibleName(Password ? tr("압축 파일 암호") : tr("충돌한 항목의 새 이름")); NameLabel->setBuddy(editor_);
        editor_->setEchoMode(Password ? QLineEdit::Password : QLineEdit::Normal); editor_->setMaxLength(Password ? 4096 : 255);
        NameLabel->setVisible(Password); editor_->setVisible(Password); body_->addWidget(NameLabel); body_->addWidget(editor_);
        NameLabel->setObjectName("hc.jobs.newNameLabel");
        if (Password) {
            auto* Show = new QCheckBox(tr("암호 표시(&V)"), content_); Show->setObjectName("hc.jobs.showPassword"); body_->addWidget(Show);
            connect(Show, &QCheckBox::toggled, this, [this](bool Checked) { editor_->setEchoMode(Checked ? QLineEdit::Normal : QLineEdit::Password); });
        }
        validation_ = new QLabel(content_); validation_->setObjectName("hc.jobs.interaction.error"); validation_->setTextFormat(Qt::PlainText); validation_->setWordWrap(true); validation_->hide(); body_->addWidget(validation_);
        connect(editor_, &QLineEdit::textChanged, this, [this] { validate(); });
        connect(editor_, &QLineEdit::returnPressed, this, [this, Password] { if (apply_ && apply_->isEnabled()) choose(Password ? InteractionChoice::Confirm : InteractionChoice::Rename); });
        DialogFrameOptions Frame; Frame.title_ = windowTitle(); Frame.outline_ = true; auto* Footer = ApplyDialogFrame(this, Frame);
        delete Footer->takeAt(0);
        auto* FooterWidget = new QWidget(Footer->parentWidget()); footer_ = new QVBoxLayout(FooterWidget); footer_->setContentsMargins(0, 0, 0, 0); footer_->setSpacing(8); Footer->addWidget(FooterWidget);
        rebuildActions();
        if (Kind == InteractionKind::Rename) editName();
        setMinimumWidth(420); resize(screen() ? qMin(760, screen()->availableGeometry().width() - 32) : 760, height()); fitToContents();
    }

    bool FileJobInteraction::Matches(const JobSnapshot& Snapshot) const
    { return !replied_ && !dismissed_ && Snapshot.id_ == snapshot_.id_ && Snapshot.interaction_ && Snapshot.interaction_->id_ == snapshot_.interaction_->id_ && !Snapshot.cancelRequested_ && !IsTerminal(Snapshot.state_); }
    bool FileJobInteraction::has(InteractionChoice Choice) const
    { return std::ranges::find(snapshot_.interaction_->choices_, Choice) != snapshot_.interaction_->choices_.end(); }
    void FileJobInteraction::Dismiss() { dismissed_ = true; if (editor_) editor_->clear(); close(); }
    void FileJobInteraction::SetError(const QString& Message) { replied_ = false; validation_->setText(Message); validation_->show(); }

    void FileJobInteraction::endpoint(const QString& Role, const std::string& Value, const char* Name)
    {
        const auto Path = Display(Value), NativePath = Native(Value);
        auto* Block = new QWidget(content_); Block->setObjectName("hc.jobs.interaction." + QString::fromLatin1(Name));
        auto* Row = new QHBoxLayout(Block); Row->setContentsMargins(0, 0, 0, 0); Row->setSpacing(16);
        auto* Text = new QWidget(Block); auto* Lines = new QVBoxLayout(Text); Lines->setContentsMargins(0, 0, 0, 0); Lines->setSpacing(3);
        const auto Add = [&](const QString& Value) { auto* Label = new QLabel(Value, Text); Label->setTextFormat(Qt::PlainText); Label->setWordWrap(true); Label->setMinimumWidth(0); Label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); Label->setToolTip(Path); Lines->addWidget(Label); return Label; };
        auto* RoleLabel = new QLabel(Role, Block); RoleLabel->setFixedWidth(64); RoleLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft); RoleLabel->setWordWrap(true); Row->addWidget(RoleLabel);
        auto* File = Add(QFileInfo(Path).fileName().isEmpty() ? Path : QFileInfo(Path).fileName()); auto Font = File->font(); Font.setWeight(QFont::DemiBold); File->setFont(Font);
        Add(QDir::toNativeSeparators(QFileInfo(Path).path())); auto* Facts = Add(NativePath.isEmpty() ? tr("가상 경로 · 크기와 수정 시각 확인 불가") : tr("크기와 수정 시각 확인 중..."));
        Row->addWidget(Text, 1);
        auto* Icon = new QLabel(Block); Icon->setObjectName(Block->objectName() + ".preview"); Icon->setFixedSize(104, 78); Icon->setAlignment(Qt::AlignCenter); Row->addWidget(Icon);
        auto& Icons = icons::FileIconService::Instance();
        icons::Request Query; Query.kind_ = icons::Kind::Associated; Query.path_ = NativePath; Query.extension_ = QFileInfo(Path).suffix(); Query.pixels_ = qRound(48 * devicePixelRatioF()); Query.virtual_ = NativePath.isEmpty();
        const auto UpdateIcon = [Icon, Query, Ratio = devicePixelRatioF()] {
            auto& Service = icons::FileIconService::Instance(); auto Value = Service.Read(Query, Icon);
            auto Pixmap = Value.pixmap_.isNull() ? Service.DefaultIcon(false, Query.pixels_, true, Icon) : Value.pixmap_;
            Pixmap.setDevicePixelRatio(Ratio); if (!Icon->property("hc.jobs.thumbnail").toBool()) Icon->setPixmap(Pixmap);
        };
        Icons.Subscribe(Icon, UpdateIcon); UpdateIcon();
        body_->addWidget(Block);
        if (NativePath.isEmpty()) return;
        const auto Pixels = QSize(qRound(104 * devicePixelRatioF()), qRound(78 * devicePixelRatioF()));
        auto* Watcher = new QFutureWatcher<Preview>(Block);
        connect(Watcher, &QFutureWatcher<Preview>::finished, Block, [this, Watcher, Facts, Icon, Ratio = devicePixelRatioF()] {
            const auto Value = Watcher->result(); Watcher->deleteLater(); Facts->setText(Value.facts_);
            if (!Value.image_.isNull()) { auto Pixmap = QPixmap::fromImage(Value.image_); Pixmap.setDevicePixelRatio(Ratio); Icon->setProperty("hc.jobs.thumbnail", true); Icon->setPixmap(Pixmap); }
            fitToContents();
        });
        Watcher->setFuture(QtConcurrent::run(PreviewPool(), [NativePath, Pixels] {
            Preview Result; QFileInfo Info(NativePath);
            if (!Info.exists()) { Result.facts_ = QObject::tr("파일 정보를 확인할 수 없습니다."); return Result; }
            Result.facts_ = Info.isDir() ? QObject::tr("폴더 · 수정 %1").arg(QLocale().toString(Info.lastModified(), QLocale::ShortFormat))
                : QObject::tr("크기 %1 (%2바이트) · 수정 %3").arg(QLocale().formattedDataSize(Info.size())).arg(Info.size()).arg(QLocale().toString(Info.lastModified(), QLocale::ShortFormat));
            if (!Info.isFile() || Info.size() > 64 * 1024 * 1024 || !QImageReader::supportedImageFormats().contains(Info.suffix().toLatin1().toLower())) return Result;
            QImageReader Reader(NativePath); Reader.setAutoTransform(true); const auto Size = Reader.size();
            if (!Size.isValid() || qint64(Size.width()) * Size.height() > 100000000) return Result;
            Reader.setScaledSize(Size.scaled(Pixels, Qt::KeepAspectRatio)); Result.image_ = Reader.read(); return Result;
        }));
    }

    QPushButton* FileJobInteraction::addButton(QLayout* Row, const QString& Text, const char* Name, std::function<void()> Clicked)
    {
        auto* Button = new fm::ui::Button(Text, actions_); Button->setObjectName(QString::fromLatin1(Name));
        Button->setAutoDefault(false); Button->setDefault(false); Button->setFont(font());
        Button->installEventFilter(this); Button->setFocusPolicy(Qt::StrongFocus);
        Button->setFixedHeight(qMax(44, QFontMetrics(font()).height() + 16));
        Button->setMinimumWidth(qMax(88, Button->sizeHint().width()));
        Row->addWidget(Button); connect(Button, &QPushButton::clicked, this, std::move(Clicked)); return Button;
    }

    void FileJobInteraction::rebuildActions()
    {
        delete actions_; actions_ = new QWidget(this); actions_->setObjectName("hc.jobs.interaction.actions"); apply_ = nullptr;
        auto* Rows = new QVBoxLayout(actions_); Rows->setContentsMargins(0, 0, 0, 0); Rows->setSpacing(8); footer_->addWidget(actions_);
        const auto NewRow = [&]() -> QLayout* { auto* Widget = new QWidget(actions_); auto* Flow = new ActionLayout(Widget); Rows->addWidget(Widget); return Flow; };
        auto* Primary = NewRow();
        const auto Add = [&](QLayout* Row, InteractionChoice Choice, const QString& Text) {
            if (!has(Choice)) return static_cast<QPushButton*>(nullptr);
            const auto Name = "hc.jobs.choice." + std::to_string(int(Choice));
            return addButton(Row, Text, Name.c_str(), [this, Choice] { choose(Choice); });
        };
        const auto Kind = snapshot_.interaction_->kind_;
        if (rename_) {
            const auto Label = snapshot_.metadata_.kind_ == JobKind::Copy ? tr("이 이름으로 복사(&O)")
                : snapshot_.metadata_.kind_ == JobKind::Move ? tr("이 이름으로 이동(&O)")
                : snapshot_.metadata_.kind_ == JobKind::MakeFile || snapshot_.metadata_.kind_ == JobKind::MakeDirectory ? tr("이 이름으로 만들기(&O)") : tr("이 이름으로 계속(&O)");
            apply_ = addButton(Primary, Label, "hc.jobs.renameApply", [this] { choose(InteractionChoice::Rename); });
            Add(Primary, InteractionChoice::Skip, tr("건너뛰기(&S)"));
            addButton(Primary, tr("이전으로(&B)"), "hc.jobs.renameBack", [this] { rename_ = false; editor_->hide(); findChild<QLabel*>("hc.jobs.newNameLabel")->hide(); rebuildActions(); });
        }
        else {
            const auto Confirm = Kind == InteractionKind::Password ? tr("확인(&O)") : Kind == InteractionKind::ReadOnlyRemoval ? tr("삭제 허용(&Y)")
                : snapshot_.interaction_->reason_ == "confirm-recycle-delete" ? tr("휴지통으로 이동(&Y)")
                : snapshot_.metadata_.kind_ == JobKind::ArchiveDelete ? tr("압축에서 삭제(&Y)") : tr("영구 삭제(&Y)");
            auto* ConfirmButton = Add(Primary, InteractionChoice::Confirm, Confirm);
            if (Kind == InteractionKind::Password) {
                apply_ = ConfirmButton;
                if (savedPasswords_) addButton(Primary, tr("저장된 암호(&D)..."), "hc.jobs.savedPasswords", savedPasswords_);
            }
            Add(Primary, InteractionChoice::Replace, tr("덮어쓰기(&O)"));
            Add(Primary, InteractionChoice::Elevate, tr("관리자 권한으로(&E)..."));
            Add(Primary, InteractionChoice::Retry, has(InteractionChoice::CopyOnly) ? tr("원본 제거 다시 시도(&T)") : tr("다시 시도(&T)"));
            Add(Primary, InteractionChoice::CopyOnly, tr("복사만 완료(&O)"));
            Add(Primary, InteractionChoice::Skip, tr("건너뛰기(&S)"));
            if (has(InteractionChoice::Rename)) {
                const auto Name = "hc.jobs.choice." + std::to_string(int(InteractionChoice::Rename));
                addButton(Primary, tr("이름 변경(&R)..."), Name.c_str(), [this] { if (!editor_->text().isEmpty() && !editor_->isHidden()) choose(InteractionChoice::Rename); else editName(); });
            }
            Add(Primary, InteractionChoice::ConfirmAll, tr("모두 허용(&A)"));
            Add(Primary, InteractionChoice::ElevateAll, tr("모두 관리자 권한으로(&A)..."));
            if (details_) addButton(Primary, Kind == InteractionKind::Elevation ? tr("권한 오류 자세히(&P)...") : tr("파일 사용 정보(&L)..."),
                Kind == InteractionKind::Elevation ? "hc.jobs.permission" : "hc.jobs.fileLocks", details_);
        }
        auto* Secondary = !rename_ && (has(InteractionChoice::ReplaceAll) || has(InteractionChoice::RenameIncoming)) ? NewRow() : Primary;
        if (!rename_) {
            Add(Secondary, InteractionChoice::ReplaceAll, tr("모두 덮어쓰기(&A)"));
            Add(Secondary, InteractionChoice::SkipAll, tr("모두 건너뛰기(&K)"));
            if (has(InteractionChoice::RenameIncoming) || has(InteractionChoice::RenameExisting)) {
                auto* Options = addButton(Secondary, tr("옵션(&M)"), "hc.jobs.conflictOptions", [] {});
                auto* Menu = new QMenu(Options);
                if (has(InteractionChoice::RenameIncoming)) {
                    auto* Action = Menu->addAction(tr("자동 이름 변경: 복사할 파일(&F)"));
                    connect(Action, &QAction::triggered, this, [this] { choose(InteractionChoice::RenameIncoming); });
                    new QShortcut(QKeySequence(Qt::ALT | Qt::Key_F), actions_, [this] { choose(InteractionChoice::RenameIncoming); });
                }
                if (has(InteractionChoice::RenameExisting)) {
                    auto* Action = Menu->addAction(tr("자동 이름 변경: 기존 대상 파일(&G)"));
                    connect(Action, &QAction::triggered, this, [this] { choose(InteractionChoice::RenameExisting); });
                    new QShortcut(QKeySequence(Qt::ALT | Qt::Key_G), actions_, [this] { choose(InteractionChoice::RenameExisting); });
                }
                connect(Options, &QPushButton::clicked, this, [Options, Menu] {
                    const auto Size = Menu->sizeHint();
                    Menu->popup(Options->mapToGlobal(QPoint(Options->width() - Size.width(), -Size.height())));
                });
            }
        }
        addButton(Secondary, tr("취소(&C)"), "hc.jobs.interaction.cancel", [this] { choose(InteractionChoice::Cancel); });
        validate();
        if (layout()) fitToContents();
        if (isVisible()) QTimer::singleShot(0, this, [this] { focusAction(); });
    }

    void FileJobInteraction::editName()
    {
        rename_ = true; editor_->show(); findChild<QLabel*>("hc.jobs.newNameLabel")->show();
        if (editor_->text().isEmpty()) {
            const auto Dot = originalName_.lastIndexOf('.'); const auto Split = Dot > 0 ? Dot : originalName_.size();
            editor_->setText(originalName_.left(Split) + " (2)" + originalName_.mid(Split));
        }
        rebuildActions(); editor_->setFocus(); const auto Dot = editor_->text().lastIndexOf('.'); editor_->setSelection(0, Dot > 0 ? Dot : editor_->text().size());
    }
    void FileJobInteraction::validate()
    {
        if (!apply_) return;
        const bool Password = snapshot_.interaction_->kind_ == InteractionKind::Password;
        const auto Error = Password ? QString{} : filetools::NameError(editor_->text());
        const bool Same = !Password && editor_->text().compare(originalName_, Qt::CaseInsensitive) == 0;
        apply_->setEnabled(!editor_->text().isEmpty() && Error.isEmpty() && !Same);
        validation_->setText(Same ? tr("이미 사용 중인 이름입니다. 다른 이름을 입력하세요.") : Error);
        validation_->setVisible(!Password && (!Error.isEmpty() || Same));
    }
    void FileJobInteraction::choose(InteractionChoice Choice)
    {
        if (replied_ || dismissed_) return;
        if (Choice != InteractionChoice::Cancel && !has(Choice)) return;
        if ((rename_ && Choice == InteractionChoice::Rename) || (Choice == InteractionChoice::Confirm && snapshot_.interaction_->kind_ == InteractionKind::Password)) {
            validate(); if (apply_ && !apply_->isEnabled()) return;
        }
        const auto Value = Choice == InteractionChoice::Rename || (Choice == InteractionChoice::Confirm && snapshot_.interaction_->kind_ == InteractionKind::Password) ? editor_->text() : QString{};
        // The controller rechecks the job and request ID before accepting an answer.
        replied_ = true; answer_(Choice, Value);
    }
    void FileJobInteraction::focusAction()
    {
        if (dismissed_ || replied_ || !isVisible()) return;
        if (rename_ || snapshot_.interaction_->kind_ == InteractionKind::Password) { editor_->setFocus(Qt::OtherFocusReason); return; }
        auto* Target = findChild<QPushButton*>("hc.jobs.choice." + QString::number(int(InteractionChoice::Skip)));
        if (!Target) Target = findChild<QPushButton*>("hc.jobs.interaction.cancel");
        if (Target) Target->setFocus(Qt::TabFocusReason);
    }
    void FileJobInteraction::showEvent(QShowEvent* Event)
    {
        QDialog::showEvent(Event); focusAction();
        QTimer::singleShot(0, this, [this] { focusAction(); });
    }
    bool FileJobInteraction::eventFilter(QObject* Object, QEvent* Event)
    {
        auto* Button = qobject_cast<QPushButton*>(Object);
        if (Button && Event->type() == QEvent::KeyPress) {
            const auto* Key = static_cast<QKeyEvent*>(Event);
            const auto Code = Key->key();
            if (Key->modifiers() == Qt::NoModifier && (Code == Qt::Key_Return || Code == Qt::Key_Enter)) { Button->click(); return true; }
            if (Key->modifiers() == Qt::NoModifier && (Code == Qt::Key_Left || Code == Qt::Key_Right || Code == Qt::Key_Up || Code == Qt::Key_Down)) {
                auto Buttons = actions_->findChildren<QPushButton*>();
                Buttons.removeIf([this](auto* Candidate) { return !Candidate->isVisibleTo(this) || !Candidate->isEnabled(); });
                std::ranges::sort(Buttons, [this](auto* A, auto* B) {
                    const auto At = A->mapTo(this, QPoint{}), Bt = B->mapTo(this, QPoint{});
                    return At.y() == Bt.y() ? At.x() < Bt.x() : At.y() < Bt.y();
                });
                const auto Index = Buttons.indexOf(Button);
                if (Index >= 0 && !Buttons.isEmpty()) {
                    const auto Forward = Code == Qt::Key_Right || Code == Qt::Key_Down;
                    int Next = int((Index + (Forward ? 1 : Buttons.size() - 1)) % Buttons.size());
                    if (Code == Qt::Key_Up || Code == Qt::Key_Down) {
                        const auto Origin = Button->mapTo(this, Button->rect().center());
                        int Distance = std::numeric_limits<int>::max();
                        for (int I = 0; I < Buttons.size(); ++I) {
                            const auto Point = Buttons[I]->mapTo(this, Buttons[I]->rect().center());
                            const auto Dy = Point.y() - Origin.y();
                            if ((Forward && Dy > 0) || (!Forward && Dy < 0)) {
                                const auto Score = qAbs(Dy) * 10000 + qAbs(Point.x() - Origin.x());
                                if (Score < Distance) { Distance = Score; Next = I; }
                            }
                        }
                    }
                    Buttons[Next]->setFocus(Qt::TabFocusReason); return true;
                }
            }
        }
        return QDialog::eventFilter(Object, Event);
    }
    void FileJobInteraction::reject() { if (dismissed_) QDialog::reject(); else choose(InteractionChoice::Cancel); }
    void FileJobInteraction::closeEvent(QCloseEvent* Event)
    {
        // QWidget::close is already in progress when the caption X invokes this.
        // A nested controller Dismiss cannot close it again; accept this event.
        if (!dismissed_ && !replied_) choose(InteractionChoice::Cancel);
        if (dismissed_ || replied_) Event->accept(); else Event->ignore();
    }
    void FileJobInteraction::fitToContents()
    {
        if (fitting_ || !layout()) return;
        fitting_ = true; const auto Reset = qScopeGuard([this] { fitting_ = false; });
        if (auto* Body = findChild<QWidget*>("hc.dialog.body")) { Body->layout()->invalidate(); Body->layout()->activate(); }
        layout()->invalidate(); layout()->activate();
        const auto Height = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width()) : layout()->totalSizeHint().height();
        const auto Maximum = screen() ? screen()->availableGeometry().height() - 40 : 1000;
        // totalMinimumSize evaluates wrapped children at their minimum width;
        // using that height here keeps the old narrow-window height after widening.
        const auto Target = qMin(Height, Maximum);
        setMinimumHeight(Target); resize(width(), Target);
    }
    void FileJobInteraction::resizeEvent(QResizeEvent* Event)
    {
        QDialog::resizeEvent(Event);
        if (Event->oldSize().width() != Event->size().width()) fitToContents();
    }
}
