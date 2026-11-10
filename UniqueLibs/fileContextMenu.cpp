#include "stdafx.h"
#include "fileContextMenu.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "UniqueLibs/columnNativeFields.hpp"

/// 공통 라이브러리
#include "commonLibs/columns/columnQueryEngine.hpp"

/// 외부 라이브러리
#include <fmstyle/Glyphs.h>
#include <fmstyle/ThemeManager.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QEventLoop>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLoggingCategory>
#include <QScreen>
#include <QVBoxLayout>
#include <QWidgetAction>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::ui
{
    using namespace hc::filecontext;
    using fm::style::Glyph;
    using fm::style::Token;

    namespace
    {
        Q_LOGGING_CATEGORY( contextMenuLog, "hc.ui.fileContextMenu" )

        // "이름(&X)" 형식. 말줄임표는 가속 문자 뒤로 보낸다: "속성 변경(&A)…".
        QString MenuText(QString Label, QChar Mnemonic, const QString& Key)
        {
            Label.replace('&', QStringLiteral("&&"));
            if (!Mnemonic.isNull()) {
                const bool Ellipsis = Label.endsWith(QChar(0x2026));
                if (Ellipsis) Label.chop(1);
                Label += QStringLiteral("(&") + Mnemonic + ')' + (Ellipsis ? QString(QChar(0x2026)) : QString{});
            }
            return Key.isEmpty() ? Label : Label + '\t' + Key;
        }

        QString ChipStyle(const fm::style::ThemeColors& Colors, Tone Kind)
        {
            const auto Pair = [&](Token Text, Token Back) { return std::pair{Colors[Text], Colors[Back]}; };
            const auto [Text, Back] = Kind == Tone::Warn ? Pair(Token::Warn, Token::WarnBg)
                : Kind == Tone::Danger ? Pair(Token::Danger, Token::DangerBg)
                : Kind == Tone::Accent ? Pair(Token::AccentFg, Token::AccentSoft) : Pair(Token::Fg3, Token::Alt);
            return QStringLiteral("QLabel{color:%1;background:%2;border-radius:3px;padding:1px 6px;}").arg(Text.name(), Back.name());
        }

        filecontext::Probe ProbeOf(const hc::columns::CellValue& Value)
        {
            using hc::columns::ValueState;
            switch (Value.State()) {
            case ValueState::Ready: return Probe::Ready;
            case ValueState::Pending: case ValueState::OnDemand: return Probe::Pending;
            case ValueState::Unsupported: return Probe::Unsupported;
            default: return Probe::Empty;
            }
        }
    }

    FileContextMenu::FileContextMenu(ContextTarget Target, ProbeInputs Inputs, StateFor State, QWidget* Parent)
        : QMenu(Parent), target_(std::move(Target)), inputs_(std::move(Inputs)), state_(std::move(State))
    {
        setObjectName(target_.background_ ? QStringLiteral("hc.fileContextMenu.background") : QStringLiteral("hc.fileContextMenu"));
        setToolTipsVisible(true);
        result_ = EvaluateContext(target_, inputs_);
        expire_.setSingleShot(true); expire_.setInterval(3000);
        connect(&expire_, &QTimer::timeout, this, [this] { expireProbes(); });
    }

    FileContextMenu::~FileContextMenu() = default;

    QIcon FileContextMenu::icon(Icon Kind, Tone Shade) const
    {
        const auto& Colors = fm::style::themeColorsFor(this);
        const auto Color = Shade == Tone::Warn ? Colors[Token::Warn] : Shade == Tone::Danger ? Colors[Token::Danger]
            : Shade == Tone::Accent ? Colors[Token::AccentFg] : Colors[Token::Fg];
        switch (Kind) {
        case Icon::Folder: return fm::style::glyphIcon(Glyph::Folder, Colors[Token::Folder]);
        case Icon::Run: return fm::style::glyphIcon(Glyph::ArrowRight, Color);
        case Icon::Play: return fm::style::glyphIcon(Glyph::Play, Color);
        case Icon::Archive: case Icon::Open: return fm::style::glyphIcon(Glyph::File, Color);
        case Icon::Disc: return fm::style::glyphIcon(Glyph::Drive, Color);
        case Icon::Link: return fm::style::glyphIcon(Glyph::ReturnArrow, Color);
        case Icon::Shield: return fm::style::shieldIcon(Colors);
        case Icon::Warning: return fm::style::glyphIcon(Glyph::Warning, Colors[Token::Warn]);
        case Icon::Clock: return fm::style::glyphIcon(Glyph::Clock, Colors[Token::Fg3]);
        case Icon::Search: return fm::style::glyphIcon(Glyph::Search, Color);
        default: return {};
        }
    }

    QAction* FileContextMenu::addCommand(QMenu* Into, const QString& Id, const QString& Label, QChar Mnemonic, const QIcon& Picture,
        const QVariantMap& Parameters, const QString& Variant, QAction* Before, const QString& ToolTip)
    {
        const auto State = state_ ? state_(Id, Parameters) : CommandState{};
        if (!State.available_) return nullptr;
        auto* Action = new QAction(Picture, MenuText(Label, Mnemonic, State.key_), Into);
        Action->setObjectName(QStringLiteral("hc.fileContext.") + Id + (Variant.isEmpty() ? QString{} : '.' + Variant));
        Action->setEnabled(State.enabled_);
        if (State.checkable_) { Action->setCheckable(true); Action->setChecked(State.checked_); }
        // 쓸 수 없는 항목은 숨기지 않고 이유를 보인다(4.1절).
        const auto Tip = !State.enabled_ && !State.reason_.isEmpty() ? State.reason_ : ToolTip;
        if (!Tip.isEmpty()) { Action->setToolTip(Tip); Action->setStatusTip(Tip); }
        Action->setData(int(choices_.size())); choices_.push_back({Id, Parameters});
        if (Before) Into->insertAction(Before, Action); else Into->addAction(Action);
        return Action;
    }

    void FileContextMenu::build()
    {
        clear(); choices_.clear(); suggestionActions_.clear();
        defaultAction_ = section_ = placeholder_ = suggestionEnd_ = nullptr;
        // 머리글: 오른쪽 끌기 놓기 메뉴(cmpView.cpp)와 같은 QWidgetAction.
        auto* Header = new QWidget(this); Header->setObjectName("hc.fileContextHeader"); Header->setFocusPolicy(Qt::NoFocus);
        auto* Rows = new QVBoxLayout(Header); Rows->setContentsMargins(28, 8, 16, 8); Rows->setSpacing(3);
        title_ = new QLabel(Header); title_->setObjectName("hc.fileContextTitle");
        auto TitleFont = title_->font(); TitleFont.setBold(true); title_->setFont(TitleFont);
        detail_ = new QLabel(Header); detail_->setObjectName("hc.fileContextDetail");
        auto Palette = detail_->palette(); Palette.setColor(QPalette::WindowText, fm::style::themeColorsFor(this)[Token::Fg3]); detail_->setPalette(Palette);
        chips_ = new QWidget(Header); chipRow_ = new QHBoxLayout(chips_); chipRow_->setContentsMargins(0, 2, 0, 0); chipRow_->setSpacing(4);
        Rows->addWidget(title_); Rows->addWidget(detail_); Rows->addWidget(chips_);
        auto* HeaderAction = new QWidgetAction(this); HeaderAction->setDefaultWidget(Header); HeaderAction->setObjectName("hc.fileContext.header");
        addAction(HeaderAction); addSeparator();
        refreshHeader();
        if (target_.background_) buildBackgroundMenu(); else buildItemMenu();
    }

    void FileContextMenu::refreshHeader()
    {
        if (!title_) return;
        const auto Text = DescribeTarget(target_);
        const auto Screen = screen() ? screen()->availableGeometry().width() : 800;
        const int Width = qMin(420, Screen - 120);
        title_->setText(title_->fontMetrics().elidedText(Text.title_, Qt::ElideMiddle, Width)); title_->setToolTip(Text.title_);
        detail_->setText(Text.detail_); detail_->setVisible(!Text.detail_.isEmpty());
        while (auto* Item = chipRow_->takeAt(0)) { delete Item->widget(); delete Item; }
        QStringList Spoken{Text.title_, Text.detail_};
        const auto& Colors = fm::style::themeColorsFor(this);
        for (const auto& Badge : result_.badges_) {
            auto* Chip = new QLabel(chips_->fontMetrics().elidedText(Badge.text_, Qt::ElideMiddle, Width / 2), chips_);
            Chip->setObjectName("hc.fileContextChip"); Chip->setStyleSheet(ChipStyle(Colors, Badge.tone_)); Chip->setToolTip(Badge.toolTip_);
            chipRow_->addWidget(Chip); Spoken << Badge.text_;
        }
        chipRow_->addStretch();
        chips_->setVisible(!result_.badges_.empty());
        Spoken.removeAll(QString{});
        if (auto* Header = title_->parentWidget()) Header->setAccessibleName(Spoken.join(QStringLiteral(", ")));
    }

    void FileContextMenu::buildItemMenu()
    {
        const auto Plain = [this](Glyph Kind) { return fm::style::themedGlyphIcon(Kind, this); };
        const auto& Colors = fm::style::themeColorsFor(this);
        // 기본 동작: Enter가 실제로 하는 일(5.1절). 굵게 표시하며 차단 해제는 기본 동작이 되지 않는다.
        defaultAction_ = addCommand(this, "hc.navigate.open", result_.default_.label_, 'O', icon(result_.default_.icon_, Tone::Normal), {}, {}, nullptr,
            QObject::tr("Enter와 같습니다."));
        if (defaultAction_) setDefaultAction(defaultAction_);
        section_ = addSection(QObject::tr("제안")); section_->setObjectName("hc.fileContext.suggestions");
        placeholder_ = addAction(icon(Icon::Clock, Tone::Muted), QObject::tr("추가 제안 확인 중…"));
        placeholder_->setObjectName("hc.fileContext.pending"); placeholder_->setEnabled(false);
        suggestionEnd_ = addSeparator();
        refreshSuggestions();

        addCommand(this, "hc.viewer.open", QObject::tr("보기"), 'V', {});
        addCommand(this, "hc.file.edit", QObject::tr("편집"), 'E', {});
        addSeparator();
        addCommand(this, "hc.clipboard.cut", QObject::tr("잘라내기"), 'T', {});
        addCommand(this, "hc.clipboard.copy", QObject::tr("복사"), 'C', Plain(Glyph::Copy));
        const auto PasteLabel = target_.clipboardCount_ > 0 ? QObject::tr("붙여넣기 · %1개").arg(target_.clipboardCount_) : QObject::tr("붙여넣기");
        addCommand(this, "hc.clipboard.paste", PasteLabel, 'P', {}, {}, {}, nullptr, QObject::tr("현재 폴더에 붙여넣습니다: %1").arg(target_.folder_));
        // 폴더 하나를 대상으로 연 메뉴는 그 폴더 안에 붙여넣을 수 있다(10절 P2).
        if (target_.totalCount_ == 1 && target_.items_.size() == 1 && target_.items_.front().directory_ && target_.clipboardCount_ > 0)
            addCommand(this, "hc.clipboard.paste", QObject::tr("이 폴더에 붙여넣기"), {}, {}, {{"destination", target_.items_.front().path_}}, "folder",
                nullptr, QObject::tr("붙여넣을 위치: %1").arg(target_.items_.front().path_));
        addSeparator();
        addCommand(this, "hc.file.copy", QObject::tr("다른 패널로 복사…"), 'Y', Plain(Glyph::Copy));
        addCommand(this, "hc.file.move", QObject::tr("다른 패널로 이동…"), 'M', Plain(Glyph::Move));
        if (target_.totalCount_ > 1) addCommand(this, "hc.file.batchRename", QObject::tr("다중 이름 변경…"), 'R', Plain(Glyph::MultiRename));
        else addCommand(this, "hc.file.rename", QObject::tr("이름 변경"), 'R', Plain(Glyph::Rename));
        addCommand(this, "hc.file.mklink", QObject::tr("링크 만들기…"), 'L', icon(Icon::Link, Tone::Normal));
        auto* Archive = addMenu(QObject::tr("압축 · 이미지(&Z)")); Archive->setObjectName("hc.fileContext.archiveMenu"); Archive->setToolTipsVisible(true);
        addCommand(Archive, "hc.archive.create", QObject::tr("파일 압축…"), 'A', {});
        addCommand(Archive, "hc.archive.extract", QObject::tr("압축 풀기…"), 'X', {});
        addCommand(Archive, "hc.iso.create", QObject::tr("ISO 만들기…"), 'I', {});
        addCommand(Archive, "hc.iso.inspect", QObject::tr("ISO 구조 확인"), 'S', {});
        addCommand(this, "hc.file.delete", QObject::tr("삭제"), 'D', fm::style::glyphIcon(Glyph::Trash, Colors[Token::Danger]));
        addSeparator();
        auto* Paths = addMenu(QObject::tr("경로 복사(&H)")); Paths->setObjectName("hc.fileContext.pathMenu"); Paths->setToolTipsVisible(true);
        addCommand(Paths, "hc.clipboard.copyPath", QObject::tr("전체 경로"), 'F', {}, {{"format", "full"}}, "full", nullptr, QObject::tr("여러 항목은 한 줄에 하나씩 복사합니다."));
        addCommand(Paths, "hc.clipboard.copyPath", QObject::tr("이름"), 'N', {}, {{"format", "name"}}, "name");
        addCommand(Paths, "hc.clipboard.copyPath", QObject::tr("상위 폴더 경로"), 'P', {}, {{"format", "parent"}}, "parent");
        addCommand(this, "hc.file.attributes", QObject::tr("속성 변경…"), 'A', fm::style::themedGlyphIcon(Glyph::Info, this));
        auto* Tools = addMenu(QObject::tr("도구(&I)")); Tools->setObjectName("hc.fileContext.toolsMenu"); Tools->setToolTipsVisible(true);
        addCommand(Tools, "hc.file.openWith", QObject::tr("연결 프로그램 선택…"), 'O', {});
        addCommand(Tools, "hc.file.properties", QObject::tr("Windows 속성…"), 'R', {});
        addCommand(Tools, "hc.file.hash", QObject::tr("해시 계산…"), 'H', {});
        Tools->addSeparator();
        addCommand(Tools, "hc.file.compare", QObject::tr("파일 · 폴더 비교"), 'C', {});
        addCommand(Tools, "hc.file.permissions", QObject::tr("권한 확인…"), 'P', fm::style::themedGlyphIcon(Glyph::Lock, this));
        addCommand(Tools, "hc.file.folderSize", QObject::tr("하위 폴더 크기 계산"), 'S', {}, {}, {}, nullptr,
            QObject::tr("현재 목록의 모든 하위 폴더 크기를 계산합니다."));
        addCommand(Tools, "hc.plugin.wdx.query", QObject::tr("플러그인 정보…"), 'W', {});
        if (!userItems_.isEmpty()) { const auto Before = actions().size(); buildUserItems(this, userItems_, 0); if (actions().size() != Before) insertSeparator(actions().at(Before)); }
        addSeparator();
        addCommand(this, "hc.file.shellMenu", QObject::tr("탐색기 메뉴…"), 'X', fm::style::themedGlyphIcon(Glyph::More, this));
    }

    void FileContextMenu::buildBackgroundMenu()
    {
        const auto Plain = [this](Glyph Kind) { return fm::style::themedGlyphIcon(Kind, this); };
        const auto PasteLabel = target_.clipboardCount_ > 0 ? QObject::tr("붙여넣기 · %1개").arg(target_.clipboardCount_) : QObject::tr("붙여넣기");
        defaultAction_ = addCommand(this, "hc.clipboard.paste", PasteLabel, 'P', {}, {}, {}, nullptr, QObject::tr("현재 폴더에 붙여넣습니다: %1").arg(target_.folder_));
        addCommand(this, "hc.file.mkdir", QObject::tr("새 폴더"), 'N', Plain(Glyph::NewFolder));
        addCommand(this, "hc.file.new", QObject::tr("새 파일…"), 'W', Plain(Glyph::NewFile));
        addSeparator();
        addCommand(this, "hc.navigate.refresh", QObject::tr("새로 고침"), 'R', Plain(Glyph::Refresh));
        auto* Selection = addMenu(QObject::tr("선택(&S)")); Selection->setObjectName("hc.fileContext.selectionMenu"); Selection->setToolTipsVisible(true);
        addCommand(Selection, "hc.selection.all", QObject::tr("전체 선택"), 'A', {});
        addCommand(Selection, "hc.selection.select", QObject::tr("항목 선택…"), 'S', {});
        addCommand(Selection, "hc.selection.invertAll", QObject::tr("선택 반전"), 'I', {});
        addCommand(Selection, "hc.selection.restore", QObject::tr("이전 선택 복원"), 'R', {});
        auto* View = addMenu(QObject::tr("보기(&V)")); View->setObjectName("hc.fileContext.viewMenu"); View->setToolTipsVisible(true);
        addCommand(View, "hc.view.hidden", QObject::tr("숨김 파일 표시"), 'H', {});
        addCommand(View, "hc.navigate.forceRefresh", QObject::tr("강제 새로 고침"), 'F', {});
        addCommand(View, "hc.view.columns", QObject::tr("열 설정…"), 'C', {});
        addSeparator();
        addCommand(this, "hc.file.folderSize", QObject::tr("하위 폴더 크기 계산"), 'Z', {}, {}, {}, nullptr, QObject::tr("현재 목록의 모든 하위 폴더 크기를 계산합니다."));
        addCommand(this, "hc.clipboard.copyPath", QObject::tr("경로 복사"), 'H', {}, {{"format", "full"}, {"scope", "folder"}}, "folder", nullptr, target_.folder_);
        if (!userItems_.isEmpty()) { const auto Before = actions().size(); buildUserItems(this, userItems_, 0); if (actions().size() != Before) insertSeparator(actions().at(Before)); }
        addSeparator();
        addCommand(this, "hc.file.shellMenu", QObject::tr("탐색기 메뉴…"), 'X', fm::style::themedGlyphIcon(Glyph::More, this));
    }

    void FileContextMenu::buildUserItems(QMenu* Into, const QJsonArray& Items, int Depth)
    {
        if (Depth > 4) return;
        bool Separator = false;
        for (const auto& Value : Items) {
            const auto Item = Value.toObject();
            if (Item.value("type").toString() == "separator") { Separator = !Into->actions().isEmpty(); continue; }
            if (Item.contains("when") && !MatchesWhen(Item.value("when").toObject(), target_)) continue;
            const auto Label = Item.value("label").toString().trimmed();
            if (Label.isEmpty()) continue;
            const auto Id = Item.value("id").toString();
            if (Item.value("items").isArray()) {
                auto* Sub = new QMenu(Label, Into); Sub->setObjectName("hc.fileContext.user." + Id); Sub->setToolTipsVisible(true);
                buildUserItems(Sub, Item.value("items").toArray(), Depth + 1);
                if (Sub->actions().isEmpty()) { delete Sub; continue; }
                if (Separator) Into->addSeparator(); Separator = false;
                Into->addMenu(Sub); continue;
            }
            const auto Command = Item.value("command").toString().trimmed();
            if (Command.isEmpty()) continue;
            if (Separator) Into->addSeparator(); Separator = false;
            // 사용자 이름은 그대로 쓴다(가속 문자 &X 포함). 단축키 표시만 붙인다.
            auto* Action = addCommand(Into, Command, Label, {}, {}, Item.value("parameters").toObject().toVariantMap(), "user." + Id);
            if (Action) Action->setText(Label + (Action->text().contains('\t') ? '\t' + Action->text().section('\t', 1) : QString{}));
        }
    }

    void FileContextMenu::refreshSuggestions()
    {
        if (!suggestionEnd_) return;
        for (auto* Old : std::exchange(suggestionActions_, {})) { removeAction(Old); Old->deleteLater(); }
        for (const auto& Item : result_.suggestions_)
            if (auto* Action = addCommand(this, Item.commandId_, Item.label_, Item.mnemonic_, icon(Item.icon_, Item.tone_), Item.parameters_, "suggest", placeholder_, Item.toolTip_))
                suggestionActions_.push_back(Action);
        const bool Waiting = result_.waiting_ && !expired_;
        placeholder_->setVisible(Waiting);
        section_->setVisible(Waiting || !suggestionActions_.empty());
    }

    int FileContextMenu::pendingCount() const { return int(result_.pending_.size()); }

    void FileContextMenu::startProbes()
    {
        if (result_.pending_.empty() || engine_) return;
        // 메뉴 전용 엔진(itemSelectionController 선례). 네이티브 필드만 읽고, 메뉴가 닫히면 엔진과 함께 멈춘다.
        engine_ = std::make_unique<hc::columns::QueryEngine>(
            [](const QString& Path, const QString& Family, std::stop_token Stop) { return hc::columns::ReadNativeFields(Path, Family, Stop); }, nullptr);
        engine_->Changed = [this](const QSet<QString>& Paths) {
            // 엔진의 콜백 안에서 엔진을 지우지 않도록 다음 이벤트에서 반영한다.
            QTimer::singleShot(0, this, [this, Paths] { collect(Paths); });
        };
        const bool Slow = target_.place_ == Place::Smb;
        bool Changed = false;
        for (const auto& Request : result_.pending_) {
            const auto& Item = target_.items_[size_t(Request.item_)];
            const auto Path = Item.probePath_.isEmpty() ? Item.path_ : Item.probePath_;
            waiting_.insert(Path, {Request.item_, Request.field_});
            const auto Value = engine_->Read(Path, Request.field_, true, true, Slow);
            if (Value.State() != hc::columns::ValueState::Pending) Changed |= apply(Request.item_, Request.field_, Value);
        }
        if (Changed) { result_ = EvaluateContext(target_, inputs_); }
        qCDebug(contextMenuLog) << "context probes" << result_.pending_.size();
    }

    bool FileContextMenu::apply(int Index, const QString& Field, const hc::columns::CellValue& Value)
    {
        if (Index < 0 || Index >= int(target_.items_.size())) return false;
        auto& Item = target_.items_[size_t(Index)];
        const auto State = ProbeOf(Value);
        if (State == Probe::Pending) return false;
        if (Field == ZoneField) { Item.zone_ = State; Item.zoneValue_ = State == Probe::Ready ? quint32(Value.Raw().toULongLong()) : 0; }
        else if (Field == LinkField) { Item.link_ = State; Item.linkTarget_ = State == Probe::Ready ? Value.Raw().toString() : QString{}; }
        else return false;
        if (share_) share_(Item.probePath_.isEmpty() ? Item.path_ : Item.probePath_, Field, Value);
        return true;
    }

    void FileContextMenu::collect(const QSet<QString>& Paths)
    {
        if (!engine_) return;
        bool Changed = false;
        for (const auto& Path : Paths)
            for (const auto& [Index, Field] : waiting_.values(Path)) {
                const auto Value = engine_->Read(Path, Field, false);
                if (Value.State() == hc::columns::ValueState::Pending) continue;
                Changed |= apply(Index, Field, Value);
                waiting_.remove(Path, {Index, Field});
            }
        if (!Changed) return;
        const auto Before = geometry();
        result_ = EvaluateContext(target_, inputs_);
        if (waitLoop_ && result_.pending_.empty()) waitLoop_->quit();
        if (!suggestionEnd_) return;   // 아직 열리지 않았다: Exec가 만든다
        refreshHeader(); refreshSuggestions();
        if (result_.pending_.empty()) expire_.stop();
        keepAnchor(Before);
    }

    void FileContextMenu::expireProbes()
    {
        // 3초가 지나면 자리 표시를 지우고 더 기다리지 않는다(7.2절). 이미 받은 값은 apply가 모델에 넘겼다.
        expired_ = true;
        if (engine_) engine_->Cancel();
        const auto Before = geometry();
        refreshSuggestions(); keepAnchor(Before);
    }

    // 항목 수가 바뀌어 크기가 달라져도 메뉴가 열린 기준 모서리를 지킨다(7.2절).
    void FileContextMenu::keepAnchor(const QRect& Before)
    {
        if (!isVisible()) return;
        adjustSize();
        const bool Upward = Before.top() < anchor_.y() - 1 && Before.bottom() <= anchor_.y() + 1;
        auto Next = geometry();
        if (Upward) Next.moveBottom(Before.bottom()); else Next.moveTop(Before.top());
        if (const auto* At = QGuiApplication::screenAt(anchor_)) {
            const auto Area = At->availableGeometry();
            if (Next.bottom() > Area.bottom()) Next.moveBottom(Area.bottom());
            if (Next.top() < Area.top()) Next.moveTop(Area.top());
        }
        move(Next.topLeft());
    }

    std::optional<MenuChoice> FileContextMenu::Exec(const QPoint& Anchor, bool Wait)
    {
        anchor_ = Anchor;
        startProbes();
        // 로컬 고정 디스크에서만 잠깐 기다려 메뉴 높이가 열린 뒤 바뀌는 일을 줄인다. 이벤트 루프는 계속 돈다.
        if (Wait && pendingCount() > 0 && pendingCount() <= MaxWaitedProbes) {
            QEventLoop Loop; waitLoop_ = &Loop;
            QTimer::singleShot(40, &Loop, &QEventLoop::quit);
            Loop.exec(QEventLoop::ExcludeUserInputEvents);
        }
        build();
        if (result_.waiting_) expire_.start();
        QPointer<FileContextMenu> Alive(this);
        auto* Chosen = exec(Anchor);
        if (!Alive) return std::nullopt;
        expire_.stop();
        engine_.reset();
        if (!Chosen || !Chosen->data().isValid()) return std::nullopt;
        const auto Index = Chosen->data().toInt();
        if (Index < 0 || Index >= int(choices_.size())) return std::nullopt;
        return choices_[size_t(Index)];
    }
}
