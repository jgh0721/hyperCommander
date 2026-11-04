#include "stdafx.h"
#include "documentViewPane.hpp"

/// 모듈

/// UIs
#include "text/QTextView.h"
#include "text/TextLexerRegistry.h"
#include "markdownDocumentView.hpp"
#include "pdf/QPDFView.h"
#include "image/QImageView.h"
#include "imageReadingView.hpp"
#include "imageFramePlayer.hpp"
#include "imageResampler.hpp"
#include "epubProcessView.hpp"
#include "epubSourceEditor.hpp"
#include "core/ToolbarIcons.h"
#include <QComboBox>
#include <QToolBar>
#include <QToolButton>

/// 기타 헤더파일
#include "documentTextCodec.hpp"
#include "textDocumentMetrics.hpp"
#include "documentShortcutRegistry.hpp"

/// 고유 라이브러리
#include "UniqueLibs/ProductDefaults_Edit.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "core/AppSettings.h"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QActionGroup>
#include <QKeySequence>
#include <QMenu>
#include <QSignalBlocker>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        // A disabled row acts as a small section heading (prototype ".hd").
        QAction* Heading(QMenu* Menu, const QString& Text)
        {
            auto* Action = Menu->addAction(Text); Action->setEnabled(false); return Action;
        }
        QAction* Check(QMenu* Menu, const QString& Text, bool Checked)
        {
            auto* Action = Menu->addAction(Text); Action->setCheckable(true); Action->setChecked(Checked); return Action;
        }
        bool SameEncoding(const QString& A, const QString& B)
        {
            const auto Left = CanonicalEncodingName(A);
            return !Left.isEmpty() && Left == CanonicalEncodingName(B);
        }
    }
    QAction* DocumentViewPane::chromeAction(const QString& Text, const QString& Name, std::function<void()> Run, const QString& Key)
    {
        // Keys are shown as text: the window-level command owns the binding.
        auto* Action = new QAction(Key.isEmpty() ? Text : Text + QLatin1Char('\t') + Key, chromeOwner_);
        if (!Name.isEmpty()) Action->setObjectName(Name);
        if (Run) connect(Action, &QAction::triggered, this, [Run = std::move(Run)] { Run(); });
        return Action;
    }
    namespace
    {
        QString KeyText(const QJsonObject& Stored, const QString& Id)
        {
            const auto Bindings = DocumentShortcutRegistry::Bindings(Stored);
            return QKeySequence::fromString(Bindings[Id].toString(), QKeySequence::PortableText).toString(QKeySequence::NativeText);
        }
    }
    bool DocumentViewPane::SetSaveEncoding(const QString& Encoding, bool Bom, QString* ErrorCode)
    {
        if (!text_) return false;
        const bool BomAllowed = EncodingInfo(Encoding).bomCapable_;
        // Validate before changing output settings; the user keeps editing if a legacy encoding cannot represent a scalar.
        auto Bytes = EncodeDocumentText(text_->Content(), Encoding, Bom && BomAllowed);
        if (!Bytes.HasValue()) { if (ErrorCode) *ErrorCode = QString::fromStdString(Bytes.Failure().code_); return false; }
        if (encoding_ == Encoding && bom_ == (Bom && BomAllowed)) return true;
        encoding_ = Encoding; bom_ = Bom && BomAllowed;
        emit ContentChanged(); emit ViewStateChanged();
        return true;
    }
    void DocumentViewPane::buildTextChrome(QMenu* Menu, bool Language)
    {
        auto* Text = text_;
        if (Language) {
            auto* Languages = Menu->addMenu(tr("언어"));
            connect(Languages, &QMenu::aboutToShow, Languages, [Languages, Text] {
                Languages->clear(); auto* Group = new QActionGroup(Languages);
                const auto Current = Text->currentLanguage();
                for (const auto& Name : TextLexerRegistry::instance().displayNames()) {
                    auto* Item = Check(Languages, Name, Name.compare(Current, Qt::CaseInsensitive) == 0); Group->addAction(Item);
                    QObject::connect(Item, &QAction::triggered, Text, [Text, Name] { Text->setLanguage(Name); });
                }
            });
            Menu->addSeparator();
        }
        auto* Encoding = Menu->addMenu(tr("인코딩"));
        connect(Encoding, &QMenu::aboutToShow, this, [this, Encoding] {
            Encoding->clear();
            Heading(Encoding, tr("다른 인코딩으로 다시 읽기 (내용 유지 안 함)"));
            auto* Read = new QActionGroup(Encoding);
            for (const auto& Info : ReadEncodings()) {
                auto* Item = Check(Encoding, Info.label_, SameEncoding(Info.name_, readEncoding_)); Read->addAction(Item);
                connect(Item, &QAction::triggered, this, [this, Name = Info.name_] { emit ReinterpretRequested(Name); });
            }
            Encoding->addSeparator();
            Heading(Encoding, tr("변환하여 저장 (다음 저장부터)"));
            auto Saves = SaveEncodings();
            if (std::none_of(Saves.begin(), Saves.end(), [this](const auto& Info) { return SameEncoding(Info.name_, encoding_); }))
                for (const auto& Info : ReadEncodings()) if (SameEncoding(Info.name_, encoding_)) Saves.prepend(Info);
            auto* Save = new QActionGroup(Encoding);
            for (const auto& Info : Saves) {
                auto* Item = Check(Encoding, tr("%1로 변환").arg(Info.label_), SameEncoding(Info.name_, encoding_)); Save->addAction(Item);
                connect(Item, &QAction::triggered, this, [this, Name = Info.name_] {
                    QString Code;
                    if (SetSaveEncoding(Name, bom_, &Code)) return;
                    const auto Content = text_->Content(); const auto Missing = FindUnrepresentable(Content, Name);
                    const auto Where = PositionOfOffset(QStringView(Content), qMax<qsizetype>(0, Missing.firstOffset_));
                    if (Missing.firstOffset_ >= 0) text_->goToPosition(Where.line_, Where.column_);
                    hc::ui::MessageDialog::warning(this, tr("변환하여 저장"), tr("%1(으)로 표현할 수 없는 문자 %2개 — 첫 위치 줄 %3:%4. 인코딩을 바꾸지 않았습니다.")
                        .arg(EncodingInfo(Name).label_).arg(Missing.count_).arg(Where.line_).arg(Where.column_));
                });
            }
            Encoding->addSeparator();
            auto* Bom = Check(Encoding, tr("BOM 포함"), bom_);
            Bom->setEnabled(EncodingInfo(encoding_).bomCapable_);
            connect(Bom, &QAction::triggered, this, [this](bool On) { SetSaveEncoding(encoding_, On); });
        });
        auto* Eol = Menu->addMenu(tr("줄 끝"));
        connect(Eol, &QMenu::aboutToShow, this, [this, Eol, Text] {
            Eol->clear();
            const auto Stats = CountLineEndings(QStringView(Text->Content()));
            const auto Dominant = Stats.Dominant();
            Heading(Eol, Dominant == "MIXED" ? tr("현재: 혼합 (CRLF %1 · LF %2 · CR %3)").arg(Stats.crlf_).arg(Stats.lf_).arg(Stats.cr_)
                : tr("현재: %1 · %2줄").arg(Dominant.isEmpty() ? eol_ : Dominant).arg(Stats.Lines()));
            for (const auto& Item : {std::pair{QString("CRLF"), QTextView::CRLF}, std::pair{QString("LF"), QTextView::LF}, std::pair{QString("CR"), QTextView::CR}}) {
                auto* Action = Check(Eol, tr("%1로 변환").arg(Item.first), Dominant == Item.first);
                Action->setEnabled(!Text->isReadOnly());
                connect(Action, &QAction::triggered, this, [this, Text, Ending = Item.second, Name = Item.first] {
                    Text->ConvertLineEnding(Ending); eol_ = Name; statusEol_ = Name; emit ViewStateChanged();
                });
            }
        });
        auto* Indent = Menu->addMenu(tr("들여쓰기"));
        connect(Indent, &QMenu::aboutToShow, this, [this, Indent, Text] {
            Indent->clear();
            const bool Tabs = Text->useTabs(); const int Width = Text->tabWidth();
            auto* Style = new QActionGroup(Indent);
            for (const bool UseTabs : {true, false}) {
                auto* Item = Check(Indent, UseTabs ? tr("탭") : tr("공백"), Tabs == UseTabs); Style->addAction(Item);
                connect(Item, &QAction::triggered, Text, [Text, UseTabs] { Text->SetDocumentIndentation(UseTabs, Text->tabWidth()); });
            }
            Indent->addSeparator();
            auto* Size = new QActionGroup(Indent);
            for (const int Value : {2, 4, 8}) {
                auto* Item = Check(Indent, tr("크기 %1").arg(Value), Width == Value); Size->addAction(Item);
                connect(Item, &QAction::triggered, Text, [Text, Value] { Text->SetDocumentIndentation(Text->useTabs(), Value); });
            }
            Indent->addSeparator();
            for (const bool ToTabs : {false, true}) {
                auto* Item = Indent->addAction(ToTabs ? tr("들여쓰기를 탭으로 변환") : tr("들여쓰기를 공백으로 변환"));
                Item->setEnabled(!Text->isReadOnly());
                connect(Item, &QAction::triggered, Text, [Text, ToTabs] { Text->ConvertIndentation(ToTabs); });
            }
            AppSettings Settings;
            auto* Detect = Check(Indent, tr("파일 내용에서 자동 감지"), Settings.value("textView/detectIndent").toBool());
            connect(Detect, &QAction::triggered, this, [](bool On) { AppSettings Settings; Settings.setValue("textView/detectIndent", On); });
        });
        Menu->addSeparator();
        auto* ReadOnly = Menu->addAction(tr("읽기 전용")); ReadOnly->setCheckable(true); ReadOnly->setProperty("documentIconName", "read-only");
        connect(Menu, &QMenu::aboutToShow, ReadOnly, [ReadOnly, Text] { const QSignalBlocker Block(ReadOnly); ReadOnly->setChecked(Text->isReadOnly()); });
        connect(ReadOnly, &QAction::triggered, Text, [Text](bool On) { Text->setReadOnly(On); });
        // The toolbar lock and this row are one state; follow the view, not only when the menu opens.
        ReadOnly->setChecked(Text->isReadOnly());
        connect(Text, &QTextView::statusChanged, ReadOnly, [ReadOnly, Text] { const QSignalBlocker Block(ReadOnly); ReadOnly->setChecked(Text->isReadOnly()); });
        auto* Font = Menu->addAction(tr("편집기 글꼴…")); Font->setProperty("documentIconName", "font");
        connect(Font, &QAction::triggered, Text, &QTextView::ChooseEditorFont);
    }
    namespace
    {
        // 보기 menu items shared by plain text and Markdown source editing.
        void AppendTextViewItems(QMenu* Owner, QList<QAction*>& Items, QTextView* Text, std::function<QAction*(const QString&, const QString&, std::function<void()>, const QString&)> Make, bool Zoom, const QJsonObject& Bindings)
        {
            if (Zoom) {
                // The editor zooms with Ctrl+wheel and its own keys; these rows have no window shortcut.
                Items << Make(QObject::tr("확대"), "textView.zoomIn", {}, {})
                      << Make(QObject::tr("축소"), "textView.zoomOut", {}, {})
                      << Make(QObject::tr("원래 크기"), "textView.zoomReset", {}, {});
                auto* Gap = new QAction(Owner); Gap->setSeparator(true); Items << Gap;
            }
            auto* Wrap = new QMenu(QObject::tr("자동 줄바꿈"), Owner); Wrap->menuAction()->setProperty("documentIconName", "word-wrap");
            QObject::connect(Wrap, &QMenu::aboutToShow, Wrap, [Wrap, Text] {
                Wrap->clear(); auto* Group = new QActionGroup(Wrap);
                const QStringList Modes{QObject::tr("없음"), QObject::tr("단어 단위"), QObject::tr("문자 단위"), QObject::tr("공백 단위")};
                for (int Mode = 0; Mode < Modes.size(); ++Mode) {
                    auto* Item = Check(Wrap, Modes[Mode], Text->isWordWrapEnabled() ? Text->WordWrapMode() == Mode : Mode == 0); Group->addAction(Item);
                    QObject::connect(Item, &QAction::triggered, Text, [Text, Mode] { Text->SetWordWrapMode(Mode); });
                }
            });
            Items << Wrap->menuAction();
            auto* Whitespace = new QAction(QObject::tr("탭 · 공백 · 줄 끝 표시"), Owner); Whitespace->setCheckable(true);
            Whitespace->setProperty("documentIconName", "whitespace"); Whitespace->setChecked(Text->isWhitespaceVisible());
            QObject::connect(Whitespace, &QAction::triggered, Text, &QTextView::setWhitespaceVisible);
            QObject::connect(Text, &QTextView::statusChanged, Whitespace, [Whitespace, Text] { const QSignalBlocker Block(Whitespace); Whitespace->setChecked(Text->isWhitespaceVisible()); });
            Items << Whitespace;
            auto* Fold = new QMenu(QObject::tr("코드 접기"), Owner); Fold->menuAction()->setProperty("documentIconName", "code-folding");
            auto* Margin = Check(Fold, QObject::tr("접기 표시"), Text->isCodeFoldingEnabled());
            QObject::connect(Margin, &QAction::triggered, Text, &QTextView::setCodeFoldingEnabled);
            QObject::connect(Fold, &QMenu::aboutToShow, Margin, [Margin, Text] { const QSignalBlocker Block(Margin); Margin->setChecked(Text->isCodeFoldingEnabled()); });
            Fold->addSeparator();
            QObject::connect(Fold->addAction(QObject::tr("전체 접기") + QLatin1Char('\t') + KeyText(Bindings, "text.foldAll")), &QAction::triggered, Text, [Text] { Text->FoldAll(true); });
            QObject::connect(Fold->addAction(QObject::tr("전체 펼치기") + QLatin1Char('\t') + KeyText(Bindings, "text.unfoldAll")), &QAction::triggered, Text, [Text] { Text->FoldAll(false); });
            Items << Fold->menuAction();
            auto* Ruler = new QAction(QObject::tr("눈금자"), Owner); Ruler->setCheckable(true); Ruler->setChecked(Text->isRulerVisible());
            QObject::connect(Ruler, &QAction::triggered, Text, [Text](bool On) {
                Text->setRulerVisible(On); AppSettings Settings; Settings.setValue("textView/showRuler", On);
            });
            Items << Ruler;
        }
    }
    void DocumentViewPane::refreshChrome()
    {
        contribution_.menu_ = modeMenu_;
    }
    void DocumentViewPane::resetChrome()
    {
        // Menus and items belong to one load; the workspace drops them when the contribution changes.
        delete modeMenu_; modeMenu_ = nullptr;
        delete chromeOwner_; chromeOwner_ = new QObject(this);
        contribution_.menu_ = nullptr; contribution_.editActions_.clear(); contribution_.viewActions_.clear();
    }
    namespace
    {
        // A submenu whose radio rows are rebuilt from the view state each time it opens.
        QMenu* RadioMenu(QMenu* Parent, const QString& Title, std::function<void(QMenu*)> Fill)
        {
            auto* Menu = new QMenu(Title, Parent);
            QObject::connect(Menu, &QMenu::aboutToShow, Menu, [Menu, Fill = std::move(Fill)] { Menu->clear(); Fill(Menu); });
            // An empty menu would not open; one placeholder row keeps it openable until the first fill.
            Menu->addAction(QString());
            return Menu;
        }
    }
    void DocumentViewPane::buildPdfChrome()
    {
        auto* Pdf = pdf_;
        const auto Key = [this](const char* Id) { return KeyText(shortcutBindings_, QString::fromLatin1(Id)); };
        modeMenu_ = new QMenu(tr("PDF(&P)"), this); modeMenu_->setObjectName("pdfModeMenu");
        auto* Mode = modeMenu_;
        Mode->addAction(chromeAction(tr("첫 쪽"), "pdfMenu.first", [Pdf] { Pdf->goToPage(0); }, Key("pdf.first")));
        Mode->addAction(chromeAction(tr("이전 쪽"), "pdfMenu.previous", [Pdf] { Pdf->previousPage(); }, Key("pdf.pagePrevious")));
        Mode->addAction(chromeAction(tr("다음 쪽"), "pdfMenu.next", [Pdf] { Pdf->nextPage(); }, Key("pdf.pageNext")));
        Mode->addAction(chromeAction(tr("마지막 쪽"), "pdfMenu.last", [Pdf] { Pdf->goToPage(Pdf->totalPageCount() - 1); }, Key("pdf.last")));
        Mode->addAction(chromeAction(tr("쪽으로 이동…"), "pdfMenu.goToPage", [this] { emit StatusPickerRequested("page"); }, Key("pdf.goToPage")));
        Mode->addSeparator();
        Mode->addAction(chromeAction(tr("다음 검색 결과"), "pdfMenu.findNext", [Pdf] { Pdf->searchNext(); }, Key("pdf.findNext")));
        Mode->addAction(chromeAction(tr("이전 검색 결과"), "pdfMenu.findPrev", [Pdf] { Pdf->searchPrev(); }, Key("pdf.findPrev")));
        Mode->addSeparator();
        // D1: the stored /Rotate of the current page (an edit), apart from the view rotation in 보기.
        Mode->addMenu(RadioMenu(Mode, tr("현재 쪽 회전 (편집)"), [Pdf](QMenu* Menu) {
            auto* Group = new QActionGroup(Menu);
            for (const int Angle : {0, 90, 180, 270}) {
                auto* Item = Check(Menu, QStringLiteral("%1°").arg(Angle), int(Pdf->currentPageRotation()) == Angle); Group->addAction(Item);
                QObject::connect(Item, &QAction::triggered, Pdf, [Pdf, Angle] { Pdf->RotateCurrentPage(static_cast<QPDFView::RotationAngle>(Angle)); });
            }
        }));
        Mode->addMenu(RadioMenu(Mode, tr("주석"), [Pdf](QMenu* Menu) {
            using T = QPDFView::AnnotationTool;
            auto* Group = new QActionGroup(Menu);
            for (const auto& [Tool, Label] : {std::pair{T::AnnotationNone, tr("선택 도구")}, std::pair{T::AnnotationHighlight, tr("하이라이트")},
                     std::pair{T::AnnotationInk, tr("형광펜 · 잉크")}, std::pair{T::AnnotationFreeText, tr("자유 텍스트")}, std::pair{T::AnnotationUnderline, tr("밑줄")},
                     std::pair{T::AnnotationStrikeOut, tr("취소선")}, std::pair{T::AnnotationNote, tr("팝업 노트")}}) {
                auto* Item = Check(Menu, Label, Pdf->CurrentAnnotationTool() == Tool); Group->addAction(Item);
                QObject::connect(Item, &QAction::triggered, Pdf, [Pdf, Tool] { if (Tool != T::AnnotationNone) Pdf->ShowAnnotationToolBar(true); Pdf->SelectAnnotationTool(Tool); });
            }
        }));
        // 편집: 찾기 · 다음 · 이전 (the toolbar search field).
        contribution_.editActions_ = {chromeAction(tr("찾기…"), "pdfEdit.find", [this] { RunViewCommand("pdf.find"); }, Key("pdf.find")),
            chromeAction(tr("다음 찾기"), "pdfEdit.findNext", [Pdf] { Pdf->searchNext(); }, Key("pdf.findNext")),
            chromeAction(tr("이전 찾기"), "pdfEdit.findPrev", [Pdf] { Pdf->searchPrev(); }, Key("pdf.findPrev"))};
        // 보기
        QList<QAction*> Items;
        for (const auto& [Label, Id] : {std::pair{tr("확대"), "view.zoomIn"}, std::pair{tr("축소"), "view.zoomOut"}, std::pair{tr("원래 크기"), "view.zoomReset"}})
            Items << chromeAction(Label, QStringLiteral("pdfView.") + QString::fromLatin1(Id).mid(5), [this, Id = QString::fromLatin1(Id)] { RunViewCommand(Id); }, Key((QByteArray("pdf.") + QByteArray(Id).mid(5)).constData()));
        Items << RadioMenu(modeMenu_, tr("맞춤"), [Pdf](QMenu* Menu) {
            auto* Group = new QActionGroup(Menu);
            auto* Width = Check(Menu, tr("너비 맞춤"), Pdf->zoomMode() == QPDFView::FitWidth); Group->addAction(Width);
            QObject::connect(Width, &QAction::triggered, Pdf, [Pdf] { Pdf->setZoomMode(QPDFView::FitWidth); });
            auto* Page = Check(Menu, tr("쪽 맞춤"), Pdf->zoomMode() == QPDFView::FitPage); Group->addAction(Page);
            QObject::connect(Page, &QAction::triggered, Pdf, [Pdf] { Pdf->setZoomMode(QPDFView::FitPage); });
            auto* Actual = Check(Menu, tr("실제 크기"), Pdf->zoomMode() == QPDFView::CustomZoom && qRound(Pdf->zoomLevel()) == 100); Group->addAction(Actual);
            QObject::connect(Actual, &QAction::triggered, Pdf, [Pdf] { Pdf->setZoomLevel(100); });
        })->menuAction();
        auto* Gap = new QAction(chromeOwner_); Gap->setSeparator(true); Items << Gap;
        Items << RadioMenu(modeMenu_, tr("쪽 배치"), [Pdf](QMenu* Menu) {
            auto* Group = new QActionGroup(Menu);
            const QStringList Labels{tr("한 쪽"), tr("연속 스크롤"), tr("두 쪽")};
            for (int Mode = 0; Mode < Labels.size(); ++Mode) {
                auto* Item = Check(Menu, Labels[Mode], int(Pdf->viewMode()) == Mode); Group->addAction(Item);
                QObject::connect(Item, &QAction::triggered, Pdf, [Pdf, Mode] { Pdf->setViewMode(static_cast<QPDFView::ViewMode>(Mode)); });
            }
            Menu->addSeparator();
            auto* Rtl = Check(Menu, tr("오른쪽 → 왼쪽 넘김"), Pdf->spreadReadingDirection() == QPDFView::RightToLeft);
            Rtl->setEnabled(Pdf->viewMode() == QPDFView::TwoPageSpread);
            QObject::connect(Rtl, &QAction::triggered, Pdf, [Pdf](bool On) { Pdf->setSpreadReadingDirection(On ? QPDFView::RightToLeft : QPDFView::LeftToRight); });
        })->menuAction();
        Items << RadioMenu(modeMenu_, tr("회전"), [Pdf](QMenu* Menu) {
            QObject::connect(Menu->addAction(tr("왼쪽으로 90°")), &QAction::triggered, Pdf, [Pdf] { Pdf->setViewRotation(Pdf->viewRotation() - 90); });
            QObject::connect(Menu->addAction(tr("오른쪽으로 90°")), &QAction::triggered, Pdf, [Pdf] { Pdf->setViewRotation(Pdf->viewRotation() + 90); });
            Menu->addSeparator();
            auto* Group = new QActionGroup(Menu);
            for (const int Angle : {0, 90, 180, 270}) {
                auto* Item = Check(Menu, QStringLiteral("%1°").arg(Angle), Pdf->viewRotation() == Angle); Group->addAction(Item);
                QObject::connect(Item, &QAction::triggered, Pdf, [Pdf, Angle] { Pdf->setViewRotation(Angle); });
            }
        })->menuAction();
        auto* Gap2 = new QAction(chromeOwner_); Gap2->setSeparator(true); Items << Gap2;
        auto* Panel = chromeAction(tr("썸네일 · 목차 패널"), "pdfView.panel", {}); Panel->setCheckable(true); Panel->setProperty("documentIconName", "sidebar");
        connect(Panel, &QAction::triggered, Pdf, [Pdf](bool On) { Pdf->setSidebarPosition(On ? QPDFView::SidebarLeft : QPDFView::SidebarHidden); });
        auto* Annotations = chromeAction(tr("주석 도구"), "pdfView.annotations", {}); Annotations->setCheckable(true);
        connect(Annotations, &QAction::triggered, Pdf, [Pdf](bool On) { Pdf->ShowAnnotationToolBar(On); });
        const auto Sync = [Pdf, Panel, Annotations] {
            const QSignalBlocker A(Panel), B(Annotations);
            Panel->setChecked(Pdf->sidebarPosition() != QPDFView::SidebarHidden); Annotations->setChecked(Pdf->AnnotationToolBarShown());
        };
        connect(Pdf, &QPDFView::viewStateChanged, Panel, Sync); Sync();
        Items << Panel << Annotations;
        contribution_.viewActions_ = Items;
        contribution_.menu_ = modeMenu_;
    }
    void DocumentViewPane::buildImageChrome()
    {
        using K = DocumentViewKind;
        auto* Image = image_; auto* Reading = reading_;
        const quint32 Single = viewKinds::Of(K::Image), Comic = viewKinds::Of(K::Comic), Webtoon = viewKinds::Of(K::Webtoon);
        const quint32 Books = Comic | Webtoon, Every = Single | Books;
        const auto Key = [this](const char* Id) { return KeyText(shortcutBindings_, QString::fromLatin1(Id)); };
        const auto Scoped = [](QAction* Action, quint32 Kinds) { viewKinds::Set(Action, Kinds); return Action; };
        const auto Gap = [this, &Scoped](quint32 Kinds) { auto* Item = new QAction(chromeOwner_); Item->setSeparator(true); return Scoped(Item, Kinds); };
        const auto Book = [Reading] { return Reading->State(); };
        const auto Adjust = [this, Reading](const QString& Name, const QString& Label) {
            bool Ok = false;
            const double Value = hc::ui::InputDialog::getDouble(this, Label, Label, Name == "contrast" || Name == "gamma" ? 1 : 0,
                Name == "brightness" ? -1 : Name == "gamma" || Name == "contrast" ? .1 : 0, Name == "gamma" ? 5 : 3, 2, &Ok);
            if (Ok) Reading->SetAdjustment(Name, Value);
        };
        const auto SelectMode = [this](int Mode) {
            if (auto* Combo = findChild<QComboBox*>("imageReadingMode")) { Combo->setCurrentIndex(Mode); emit Combo->activated(Mode); }
        };

        // ── Mode menu: 이미지(I) · 만화(C) · 웹툰(W) ──
        modeMenu_ = new QMenu(tr("이미지(&I)"), this); modeMenu_->setObjectName("imageModeMenu");
        auto* Mode = modeMenu_;
        const auto Item = [&](const QString& Text, const char* Name, quint32 Kinds, std::function<void()> Run, const char* KeyId = nullptr) {
            auto* Action = Scoped(chromeAction(Text, QString::fromLatin1(Name), std::move(Run), KeyId ? Key(KeyId) : QString()), Kinds);
            Mode->addAction(Action); return Action;
        };
        const auto Sub = [&](QMenu* Menu, quint32 Kinds) { Scoped(Menu->menuAction(), Kinds); Mode->addMenu(Menu); return Menu; };
        const auto Separator = [&](quint32 Kinds) { Scoped(Mode->addSeparator(), Kinds); };
        // Single image
        Item(tr("이전 이미지"), "imageMenu.previous", Single, [this] { RunViewCommand("image.previous"); }, "image.previous");
        Item(tr("다음 이미지"), "imageMenu.next", Single, [this] { RunViewCommand("image.next"); }, "image.next");
        Separator(Single);
        auto* Transform = Sub(new QMenu(tr("회전 · 뒤집기"), Mode), Single);
        connect(Transform->addAction(ToolbarIcons::iconByName("rotate-cw"), tr("시계 방향 90°")), &QAction::triggered, Image, &QImageView::rotateClockwise);
        connect(Transform->addAction(ToolbarIcons::iconByName("rotate-ccw"), tr("반시계 방향 90°")), &QAction::triggered, Image, &QImageView::rotateCounterClockwise);
        Transform->addSeparator();
        connect(Transform->addAction(tr("좌우 뒤집기")), &QAction::triggered, Image, &QImageView::flipHorizontal);
        connect(Transform->addAction(tr("상하 뒤집기")), &QAction::triggered, Image, &QImageView::flipVertical);
        auto* Crop = Sub(new QMenu(tr("자르기"), Mode), Single);
        connect(Crop->addAction(tr("영역 선택")), &QAction::triggered, this, [this, Image] {
            if (auto* Edit = findChild<QAction*>("imageEditMode"); Edit && !Edit->isChecked()) Edit->trigger();
            Image->setEditTool(QImageView::Crop);
        });
        connect(Crop->addAction(tr("자르기 적용")), &QAction::triggered, Image, &QImageView::cropToSelection);
        Item(tr("출력 크기…"), "imageMenu.outputSize", Single, [this] { if (auto* Resize = findChild<QAction*>("imageOutputSize")) Resize->trigger(); });
        Separator(Single);
        Item(tr("슬라이드쇼 시작"), "imageMenu.slideshowStart", Single, [this] { SetSlideshow(true); });
        Item(tr("슬라이드쇼 정지"), "imageMenu.slideshowStop", Single, [this] { SetSlideshow(false); });
        Separator(Single);
        auto* Play = Item(tr("재생 / 일시정지"), "imageMenu.animation", Single, [this] { RunViewCommand("image.animation"); }, "image.animation");
        auto* PreviousFrame = Item(tr("이전 프레임"), "imageMenu.framePrevious", Single, [this] { animation_->Step(-1); });
        auto* NextFrame = Item(tr("다음 프레임"), "imageMenu.frameNext", Single, [this] { animation_->Step(1); });
        connect(Mode, &QMenu::aboutToShow, Play, [this, Play, PreviousFrame, NextFrame] {
            const bool Animated = animation_ && animation_->Supported();
            Play->setEnabled(Animated); PreviousFrame->setEnabled(Animated); NextFrame->setEnabled(Animated);
        });
        Separator(Single);
        Item(tr("다른 형식으로 저장…"), "imageMenu.saveAs", Single, [this] { emit SaveRequested(); });
        // Comic and webtoon
        Item(tr("첫 쪽"), "comicMenu.first", Comic, [Reading] { Reading->RunCommand("image.first"); }, "image.first");
        Item(tr("이전 쪽"), "comicMenu.previous", Comic, [Reading] { Reading->RunCommand("image.previous"); }, "image.previous");
        Item(tr("다음 쪽"), "comicMenu.next", Comic, [Reading] { Reading->RunCommand("image.next"); }, "image.next");
        Item(tr("마지막 쪽"), "comicMenu.last", Comic, [Reading] { Reading->RunCommand("image.last"); }, "image.last");
        Item(tr("쪽으로 이동…"), "comicMenu.goToPage", Comic, [this] { emit StatusPickerRequested("page"); }, "image.goToPage");
        // 페이지 목록: the navigator's page grid. The full screen capsule shares readingPageList.
        const auto OpenPages = [Reading] { if (auto* Pages = Reading->findChild<QToolButton*>("imageNavigatorPages")) Pages->click(); };
        Item(tr("페이지 목록…"), "comicMenu.pages", Comic, OpenPages);
        auto* PageList = Scoped(chromeAction(tr("페이지 목록"), "readingPageList", OpenPages), Books);
        PageList->setIcon(ToolbarIcons::iconByName("toc-toggle")); PageList->setProperty("documentIconName", "toc-toggle");
        Item(tr("다음 화면"), "webtoonMenu.next", Webtoon, [Reading] { Reading->RunCommand("image.next"); }, "image.next");
        Item(tr("이전 화면"), "webtoonMenu.previous", Webtoon, [Reading] { Reading->RunCommand("image.previousSpace"); }, "image.previousSpace");
        Item(tr("첫 이미지"), "webtoonMenu.first", Webtoon, [Reading] { Reading->RunCommand("image.first"); }, "image.first");
        Item(tr("마지막 이미지"), "webtoonMenu.last", Webtoon, [Reading] { Reading->RunCommand("image.last"); }, "image.last");
        Item(tr("이미지로 이동…"), "webtoonMenu.goToPage", Webtoon, [this] { emit StatusPickerRequested("page"); }, "image.goToPage");
        Separator(Books);
        Item(tr("이전 권"), "comicMenu.volumePrevious", Comic, [Reading] { Reading->OpenVolume(-1); }, "image.volumePrevious");
        Item(tr("다음 권"), "comicMenu.volumeNext", Comic, [Reading] { Reading->OpenVolume(1); }, "image.volumeNext");
        Item(tr("이전 회차"), "webtoonMenu.volumePrevious", Webtoon, [Reading] { Reading->OpenVolume(-1); }, "image.volumePrevious");
        Item(tr("다음 회차"), "webtoonMenu.volumeNext", Webtoon, [Reading] { Reading->OpenVolume(1); }, "image.volumeNext");
        auto* AutoComic = Item(tr("마지막 쪽에서 다음 권 자동"), "comicMenu.automaticNext", Comic, {});
        auto* AutoWebtoon = Item(tr("끝에서 다음 회차 자동"), "webtoonMenu.automaticNext", Webtoon, {});
        for (auto* Auto : {AutoComic, AutoWebtoon}) { Auto->setCheckable(true); connect(Auto, &QAction::triggered, Reading, &ImageReadingView::SetAutomaticNext); }
        Separator(Books);
        Item(tr("책갈피 저장"), "readingMenu.bookmark", Books, [Reading] { Reading->Bookmark(); }, "image.bookmarkSave");
        Item(tr("책갈피로 이동…"), "readingMenu.openBookmark", Books, [Reading] { Reading->OpenBookmark(); }, "image.bookmarkOpen");
        Separator(Books);
        auto* Private = Item(tr("시크릿 읽기 (기록 안 함)"), "readingMenu.private", Books, {}); Private->setCheckable(true);
        connect(Private, &QAction::triggered, Reading, &ImageReadingView::SetPrivate);
        Item(tr("이 쪽 다시 읽기"), "comicMenu.retry", Comic, [Reading] { Reading->RetryPage(); });
        Item(tr("이 이미지 다시 읽기"), "webtoonMenu.retry", Webtoon, [Reading] { Reading->RetryPage(); });
        connect(Mode, &QMenu::aboutToShow, Private, [Book, AutoComic, AutoWebtoon, Private] {
            const auto State = Book();
            for (auto* Auto : {AutoComic, AutoWebtoon}) { const QSignalBlocker Block(Auto); Auto->setChecked(State["automaticNext"].toBool()); }
            const QSignalBlocker Block(Private); Private->setChecked(State["private"].toBool());
        });

        // ── 편집 ──
        auto* EditMode = Scoped(chromeAction(tr("편집 모드"), "imageEdit.mode", {}), Single); EditMode->setCheckable(true); EditMode->setProperty("documentIconName", "edit-pencil");
        if (auto* Edit = findChild<QAction*>("imageEditMode")) {
            EditMode->setChecked(Edit->isChecked());
            connect(EditMode, &QAction::triggered, Edit, [Edit](bool On) { if (Edit->isChecked() != On) Edit->trigger(); });
            connect(Edit, &QAction::toggled, EditMode, [EditMode](bool On) { const QSignalBlocker Block(EditMode); EditMode->setChecked(On); });
        }
        auto* EditCopy = Scoped(chromeAction(tr("현재 이미지 사본 편집…"), "readingEdit.copy", [this, Reading] {
            const auto Current = Reading->CurrentImage(); if (!Current.isNull()) emit EditImageRequested(Current);
        }), Books);
        contribution_.editActions_ = {Scoped(Gap(Every), Single), EditMode, EditCopy};

        // ── 보기 ──
        QList<QAction*> View;
        for (const auto& [Label, Id] : {std::pair{tr("확대"), "view.zoomIn"}, std::pair{tr("축소"), "view.zoomOut"}, std::pair{tr("원래 크기"), "view.zoomReset"}})
            View << Scoped(chromeAction(Label, QStringLiteral("imageView.") + QString::fromLatin1(Id).mid(5), [this, Id = QString::fromLatin1(Id)] { RunViewCommand(Id); }, Key((QByteArray("image.") + QByteArray(Id).mid(5)).constData())), Every);
        View << Scoped(RadioMenu(modeMenu_, tr("맞춤"), [this, Image, Reading](QMenu* Menu) {
            auto* Group = new QActionGroup(Menu);
            const auto Kind = this->Kind();
            const bool Reader = Kind != DocumentViewKind::Image;
            const int Current = Reader ? Reading->State()["fit"].toInt() : int(Image->fitMode());
            const QStringList Names{tr("원본 크기"), tr("창에 맞춤"), tr("너비 맞춤"), tr("높이 맞춤"), tr("늘림")};
            const QList<int> Order = Kind == DocumentViewKind::Webtoon ? QList<int>{2, 0} : Reader ? QList<int>{1, 2, 3, 0} : QList<int>{1, 2, 3, 0, 4};
            for (const int Fit : Order) {
                auto* Row = Check(Menu, Names[Fit], Current == Fit); Group->addAction(Row);
                QObject::connect(Row, &QAction::triggered, Menu, [Image, Reading, Reader, Fit] {
                    if (Reader) Reading->SetFit(Fit); else Image->setFitMode(static_cast<QImageView::FitMode>(Fit));
                });
            }
        })->menuAction(), Every);
        const auto Quality = [this, Image, Reading]() {
            return RadioMenu(modeMenu_, tr("보기 품질"), [Image, Reading](QMenu* Menu) {
                auto* Group = new QActionGroup(Menu);
                QList<std::pair<QString, QString>> Filters{{QStringLiteral("auto"), tr("자동")}};
                for (const auto& Filter : ImageFilterRegistry::Filters()) Filters.append({Filter.id_, Filter.label_});
                const auto Current = Reading && !Reading->isHidden() ? Reading->State()["filter"].toString() : Image->DisplayFilter();
                for (const auto& [Id, Label] : Filters) {
                    auto* Row = Check(Menu, Label, Current == Id || (Current.isEmpty() && Id == QLatin1String("auto"))); Group->addAction(Row);
                    QObject::connect(Row, &QAction::triggered, Menu, [Image, Reading, Id] { Image->SetDisplayFilter(Id); if (Reading) Reading->SetFilter(Id); });
                }
            });
        };
        View << Scoped(Quality()->menuAction(), Single);
        View << Scoped(chromeAction(tr("이미지 간격…"), "webtoonView.gap", [this, Reading] {
            bool Ok = false;
            const int Value = hc::ui::InputDialog::getInt(this, tr("이미지 간격"), tr("간격 (DIP)"), Reading->State()["gap"].toInt(), 0, 200, 1, &Ok);
            if (Ok) Reading->SetGap(Value);
        }), Webtoon);
        View << Gap(Comic);
        View << Scoped(RadioMenu(modeMenu_, tr("펼침"), [Reading](QMenu* Menu) {
            const auto State = Reading->State();
            auto* Group = new QActionGroup(Menu);
            auto* One = Check(Menu, tr("한 쪽"), State["mode"].toInt() == ImageReadingView::Single); Group->addAction(One);
            QObject::connect(One, &QAction::triggered, Reading, [Reading] { Reading->SetMode(ImageReadingView::Single); });
            auto* Two = Check(Menu, tr("두 쪽"), State["mode"].toInt() == ImageReadingView::Comic); Group->addAction(Two);
            QObject::connect(Two, &QAction::triggered, Reading, [Reading] { Reading->SetMode(ImageReadingView::Comic); });
            Menu->addSeparator();
            auto* Cover = Check(Menu, tr("표지 단독"), State["cover"].toBool());
            QObject::connect(Cover, &QAction::triggered, Reading, [Reading](bool On) { Reading->SetCover(On); AppSettings Settings; Settings.setValue("imageView/comic/coverAlone", On); });
            auto* Split = Check(Menu, tr("펼침면 분할"), State["split"].toBool());
            QObject::connect(Split, &QAction::triggered, Reading, &ImageReadingView::SetSplit);
        })->menuAction(), Comic);
        View << Scoped(RadioMenu(modeMenu_, tr("읽기 방향"), [Reading](QMenu* Menu) {
            const bool Rtl = Reading->State()["rtl"].toBool();
            auto* Group = new QActionGroup(Menu);
            auto* Ltr = Check(Menu, tr("왼쪽 → 오른쪽"), !Rtl); Group->addAction(Ltr);
            QObject::connect(Ltr, &QAction::triggered, Reading, [Reading] { Reading->SetRightToLeft(false); });
            auto* RtlRow = Check(Menu, tr("오른쪽 → 왼쪽"), Rtl); Group->addAction(RtlRow);
            QObject::connect(RtlRow, &QAction::triggered, Reading, [Reading] { Reading->SetRightToLeft(true); });
        })->menuAction(), Comic);
        View << Scoped(chromeAction(tr("쪽 간격…"), "comicView.gap", [this, Reading] {
            bool Ok = false;
            const int Value = hc::ui::InputDialog::getInt(this, tr("쪽 간격"), tr("간격 (DIP)"), Reading->State()["gap"].toInt(), 0, 200, 1, &Ok);
            if (Ok) Reading->SetGap(Value);
        }), Comic);
        View << Scoped(RadioMenu(modeMenu_, tr("보기 방식"), [this, SelectMode](QMenu* Menu) {
            auto* Group = new QActionGroup(Menu);
            const auto Kind = this->Kind();
            const int Current = Kind == DocumentViewKind::Webtoon ? 2 : Kind == DocumentViewKind::Comic ? 1 : 0;
            const QStringList Names{tr("일반 이미지"), tr("만화"), tr("웹툰")};
            for (int Value = 0; Value < Names.size(); ++Value) {
                auto* Row = Check(Menu, Names[Value], Current == Value); Group->addAction(Row);
                QObject::connect(Row, &QAction::triggered, Menu, [SelectMode, Value] { SelectMode(Value); });
            }
        })->menuAction(), Every);
        View << Gap(Books);
        auto* Correct = new QMenu(tr("화질 보정"), modeMenu_); Correct->menuAction()->setProperty("documentIconName", "adjust");
        for (const auto& [Name, Label] : {std::pair{QString("grayscale"), tr("회색조")}, std::pair{QString("invert"), tr("색 반전")}}) {
            auto* Row = Correct->addAction(Label); Row->setCheckable(true);
            connect(Row, &QAction::triggered, Reading, [Reading, Name](bool On) { Reading->SetAdjustment(Name, On ? 1 : 0); });
        }
        Correct->addSeparator();
        for (const auto& [Name, Label] : {std::pair{QString("brightness"), tr("밝기…")}, std::pair{QString("contrast"), tr("대비…")}, std::pair{QString("gamma"), tr("감마…")}, std::pair{QString("sharpness"), tr("선명화…")}})
            connect(Correct->addAction(Label), &QAction::triggered, this, [Adjust, Name, Label] { Adjust(Name, QString(Label).remove(QStringLiteral("…"))); });
        Correct->addSeparator();
        connect(Correct->addAction(tr("보정 초기화")), &QAction::triggered, Reading, [Reading, Correct] {
            for (const auto* Name : {"grayscale", "invert", "sharpness", "brightness"}) Reading->SetAdjustment(QString::fromLatin1(Name), 0);
            for (const auto* Name : {"contrast", "gamma"}) Reading->SetAdjustment(QString::fromLatin1(Name), 1);
            for (auto* Row : Correct->actions()) if (Row->isCheckable()) { const QSignalBlocker Block(Row); Row->setChecked(false); }
        });
        View << Scoped(Correct->menuAction(), Books);
        View << Scoped(Quality()->menuAction(), Books);
        contribution_.viewActions_ = View;
        contribution_.menu_ = modeMenu_;

        // ── Toolbar (prototype rows per kind) ──
        auto* Previous = toolbar_->findChild<QAction*>("image.previous"); auto* Next = toolbar_->findChild<QAction*>("image.next");
        auto* Edit = findChild<QAction*>("imageEditMode");
        for (auto* Action : toolbar_->actions()) toolbar_->removeAction(Action);
        const auto Tool = [&](const QString& IconName, const QString& Tip, quint32 Kinds, std::function<void()> Run, const QString& Text = {}) {
            auto* Action = Scoped(new QAction(ToolbarIcons::iconByName(IconName), Text.isEmpty() ? Tip : Text, chromeOwner_), Kinds);
            Action->setToolTip(Tip); Action->setProperty("documentIconName", IconName);
            if (Run) connect(Action, &QAction::triggered, this, [Run = std::move(Run)] { Run(); });
            toolbar_->addAction(Action);
            if (!Text.isEmpty()) if (auto* Button = qobject_cast<QToolButton*>(toolbar_->widgetForAction(Action))) Button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            return Action;
        };
        const auto Line = [&](quint32 Kinds) { Scoped(toolbar_->addSeparator(), Kinds); };
        Tool("first-page", tr("첫 쪽"), Comic, [Reading] { Reading->RunCommand("image.first"); });
        if (Previous) toolbar_->addAction(Scoped(Previous, Every));
        if (Next) toolbar_->addAction(Scoped(Next, Every));
        Tool("last-page", tr("마지막 쪽"), Comic, [Reading] { Reading->RunCommand("image.last"); });
        Line(Every);
        Tool("zoom-out", tr("축소"), Every, [this] { RunViewCommand("view.zoomOut"); });
        Tool("zoom-in", tr("확대"), Every, [this] { RunViewCommand("view.zoomIn"); });
        auto* FitWindow = Tool("fit-page", tr("창에 맞춤"), Single, [Image] { Image->setFitMode(QImageView::FitToWindow); }); FitWindow->setCheckable(true);
        auto* FitBook = Tool("fit-page", tr("맞춤"), Books, {}, tr("창에 맞춤")); FitBook->setObjectName("readingFitButton");
        FitBook->setMenu(View[3]->menu());
        if (auto* Button = qobject_cast<QToolButton*>(toolbar_->widgetForAction(FitBook))) Button->setPopupMode(QToolButton::InstantPopup);
        Line(Every);
        Tool("rotate-ccw", tr("반시계 방향 90°"), Single, [Image] { Image->rotateCounterClockwise(); });
        Tool("rotate-cw", tr("시계 방향 90°"), Single, [Image] { Image->rotateClockwise(); });
        Line(Single);
        auto* Slideshow = Tool("play", tr("슬라이드쇼"), Single, {}, tr("슬라이드쇼")); Slideshow->setCheckable(true);
        Slideshow->setObjectName("imageSlideshowButton"); Slideshow->setChecked(SlideshowRunning());
        connect(Slideshow, &QAction::triggered, this, [this](bool On) { SetSlideshow(On); });
        auto* Spread = Tool("spread", tr("두 쪽 보기"), Comic, {}, tr("두 쪽")); Spread->setCheckable(true); Spread->setObjectName("readingSpreadButton");
        connect(Spread, &QAction::triggered, Reading, [Reading](bool On) { Reading->SetMode(On ? ImageReadingView::Comic : ImageReadingView::Single); });
        auto* Direction = Tool("reading-direction", tr("읽기 방향"), Comic, [Reading] { Reading->SetRightToLeft(!Reading->State()["rtl"].toBool()); }, tr("왼쪽→오른쪽"));
        Direction->setObjectName("readingDirectionButton");
        Line(Comic);
        auto* CorrectButton = Tool("adjust", tr("화질 보정"), Books, {}, tr("화질 보정")); CorrectButton->setMenu(Correct); CorrectButton->setObjectName("readingCorrectButton");
        if (auto* Button = qobject_cast<QToolButton*>(toolbar_->widgetForAction(CorrectButton))) Button->setPopupMode(QToolButton::InstantPopup);
        Tool("bookmark", tr("책갈피 저장"), Books, [Reading] { Reading->Bookmark(); })->setObjectName("readingBookmarkButton");
        // 이미지 간격 ▾ (webtoon): used by the full screen capsule; the status bar has the same chips.
        auto* GapMenu = new QMenu(modeMenu_);
        for (const int Value : {0, 8, 16}) connect(GapMenu->addAction(tr("%1 px").arg(Value)), &QAction::triggered, Reading, [Reading, Value] { Reading->SetGap(Value); });
        GapMenu->addSeparator();
        connect(GapMenu->addAction(tr("사용자 지정…")), &QAction::triggered, this, [this, Reading] {
            bool Ok = false;
            const int Value = hc::ui::InputDialog::getInt(this, tr("이미지 간격"), tr("간격 (DIP)"), Reading->State()["gap"].toInt(), 0, 200, 1, &Ok);
            if (Ok) Reading->SetGap(Value);
        });
        auto* GapButton = Scoped(new QAction(ToolbarIcons::iconByName("layout"), tr("간격 0 px"), chromeOwner_), Webtoon);
        GapButton->setObjectName("readingGapButton"); GapButton->setToolTip(tr("이미지 간격")); GapButton->setProperty("documentIconName", "layout"); GapButton->setMenu(GapMenu);
        auto* Spacer = new QWidget(toolbar_); Spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        Scoped(toolbar_->addWidget(Spacer), Every);
        if (Edit) {
            toolbar_->addAction(Scoped(Edit, Single));
            if (auto* Button = qobject_cast<QToolButton*>(toolbar_->widgetForAction(Edit))) Button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        }
        Tool("fullscreen", tr("전체 화면"), Books, [this] { emit FullscreenRequested(); });
        syncImageChrome();
    }
    void DocumentViewPane::syncImageChrome()
    {
        if (!image_ || !reading_ || !modeMenu_ || !toolbar_) return;
        const auto Kind = this->Kind();
        const auto Title = Kind == DocumentViewKind::Comic ? tr("만화(&C)") : Kind == DocumentViewKind::Webtoon ? tr("웹툰(&W)") : tr("이미지(&I)");
        contribution_.menuTitle_ = Title; modeMenu_->setTitle(Title);
        QList<QAction*> Scoped = modeMenu_->actions() + toolbar_->actions() + contribution_.viewActions_ + contribution_.editActions_;
        for (auto* Action : Scoped) if (viewKinds::Has(Action)) Action->setVisible(viewKinds::Applies(Action, Kind));
        const auto State = reading_->isHidden() ? QJsonObject{} : reading_->State();
        if (auto* Gap = chromeOwner_->findChild<QAction*>("readingGapButton")) Gap->setText(tr("간격 %1 px").arg(State["gap"].toInt()));
        for (auto* Action : toolbar_->actions()) {
            const QSignalBlocker Block(Action);
            if (Action->objectName() == QLatin1String("readingSpreadButton")) Action->setChecked(State["mode"].toInt() == ImageReadingView::Comic);
            else if (Action->objectName() == QLatin1String("readingDirectionButton")) Action->setText(State["rtl"].toBool() ? tr("오른쪽→왼쪽") : tr("왼쪽→오른쪽"));
            else if (Action->objectName() == QLatin1String("readingFitButton")) {
                const QStringList Fits{tr("원본 크기"), tr("창에 맞춤"), tr("너비 맞춤"), tr("높이 맞춤")};
                Action->setText(Fits.value(State["fit"].toInt(), tr("창에 맞춤")));
            } else if (Action->isCheckable() && Action->property("documentIconName").toString() == QLatin1String("fit-page")) Action->setChecked(image_->fitMode() == QImageView::FitToWindow);
        }
    }
    void DocumentViewPane::buildEpubChrome(QToolBar* Bar)
    {
        using K = DocumentViewKind;
        const quint32 Read = viewKinds::Of(K::EpubRead), Edit = viewKinds::Of(K::EpubEdit);
        const auto Key = [this](const char* Id) { return KeyText(shortcutBindings_, QString::fromLatin1(Id)); };
        const auto Reader = [this](const QString& Action, const QJsonValue& Value = {}) { if (auto* View = currentEpub()) View->ReaderAction(Action, Value); };
        const auto Editor = [this](const char* Name) -> QAction* { return epubEditor_ ? epubEditor_->findChild<QAction*>(QString::fromLatin1(Name)) : nullptr; };
        const auto EditorMenu = [this](const char* Name) -> QMenu* { return epubEditor_ ? epubEditor_->findChild<QMenu*>(QString::fromLatin1(Name)) : nullptr; };
        const auto Scoped = [](QAction* Action, quint32 Kinds) { viewKinds::Set(Action, Kinds); return Action; };
        // Profile edits go through the reader's own settings path (preview a patch, then apply), so the
        // settings panel and the saved profile agree. Change receives the current profile and fills the patch.
        const auto Profile = [this](const std::function<void(const QJsonObject&, QJsonObject&)>& Change) {
            auto* View = currentEpub(); if (!View) return;
            QJsonObject Patch; Change(View->ViewState()["profile"].toObject(), Patch); if (Patch.isEmpty()) return;
            if (View->ChangeReadingSettings(QStringLiteral("preview"), Patch, false)) View->ChangeReadingSettings(QStringLiteral("apply"), {}, false);
        };

        // ── Reader row of the EPUB toolbar (prototype: 이전 위치 · 이전 · 다음 | 목차 · 검색 · 책갈피 · 형광펜·메모 | 읽기 설정 > 전체 화면) ──
        if (Bar) {
            const auto Tool = [&](const QString& IconName, const QString& Text, std::function<void()> Run, bool Named = true) {
                auto* Action = new QAction(ToolbarIcons::iconByName(IconName), Text, Bar);
                Action->setToolTip(Text); Action->setProperty("documentIconName", IconName); Action->setProperty("epubReaderItem", true);
                connect(Action, &QAction::triggered, this, [Run = std::move(Run)] { Run(); });
                Bar->addAction(Action);
                if (auto* Button = qobject_cast<QToolButton*>(Bar->widgetForAction(Action))) Button->setToolButtonStyle(Named ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
                return Action;
            };
            const auto Line = [Bar] { Bar->addSeparator()->setProperty("epubReaderItem", true); };
            Line();
            Tool("undo", tr("이전 위치"), [Reader] { Reader("back"); });
            Tool("chevron-left", tr("이전 화면"), [Reader] { Reader("previous"); }, false);
            Tool("chevron-right", tr("다음 화면"), [Reader] { Reader("next"); }, false);
            Line();
            Tool("toc-toggle", tr("목차"), [Reader] { Reader("toggle-toc"); });
            Tool("search", tr("검색"), [Reader] { Reader("toggle-search"); });
            Tool("bookmark", tr("책갈피"), [Reader] { Reader("toggle-bookmarks"); });
            Tool("annotation-highlight", tr("형광펜·메모"), [Reader] { Reader("toggle-notes"); });
            Line();
            Tool("font", tr("읽기 설정"), [Reader] { Reader("toggle-settings"); });
            auto* Spacer = new QWidget(Bar); Spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            Bar->addWidget(Spacer)->setProperty("epubReaderItem", true);
            Tool("fullscreen", tr("전체 화면"), [this] { emit FullscreenRequested(); }, false);
            Bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        }

        // ── EPUB(B): reading and editing commands, rebuilt for the current mode ──
        modeMenu_ = new QMenu(tr("EPUB(&B)"), this); modeMenu_->setObjectName("epubModeMenu");
        auto* Mode = modeMenu_;
        connect(Mode, &QMenu::aboutToShow, this, [this, Mode, Reader, Editor, EditorMenu, Key] {
            Mode->clear();
            const auto Add = [Mode](QAction* Action) { if (Action) Mode->addAction(Action); };
            const auto Run = [Mode](const QString& Text, std::function<void()> Fn, const QString& Shortcut = {}) {
                auto* Action = Mode->addAction(Shortcut.isEmpty() ? Text : Text + QLatin1Char('\t') + Shortcut);
                QObject::connect(Action, &QAction::triggered, Mode, [Fn = std::move(Fn)] { Fn(); }); return Action;
            };
            if (!IsEpubEditing()) {
                Run(tr("다음 화면"), [Reader] { Reader("next"); }, Key("epub.next"));
                Run(tr("이전 화면"), [Reader] { Reader("previous"); }, Key("epub.previous"));
                Run(tr("다음 장"), [Reader] { Reader("next-chapter"); }, Key("epub.next-chapter"));
                Run(tr("이전 장"), [Reader] { Reader("previous-chapter"); }, Key("epub.previous-chapter"));
                Run(tr("이전 위치로"), [Reader] { Reader("back"); }, Key("epub.back"));
                Mode->addSeparator();
                Run(tr("목차"), [Reader] { Reader("toggle-toc"); }, Key("epub.focus-toc"));
                Run(tr("책갈피 추가"), [Reader] { Reader("bookmark-add"); }, Key("epub.bookmark-add"));
                Run(tr("형광펜 · 메모"), [Reader] { Reader("toggle-notes"); });
                Mode->addSeparator();
                Run(tr("영역 포커스 이동"), [Reader] { Reader("cycle-focus"); }, Key("epub.cycle-focus"));
                Mode->addSeparator();
                auto* Switch = Run(tr("편집 모드로 전환"), [this] { if (auto* Edit = findChild<QAction*>("epubEdit"); Edit && Edit->isEnabled()) { if (Edit->isCheckable()) Edit->setChecked(true); else Edit->trigger(); } });
                Switch->setEnabled(epubEditingAllowed_);
                return;
            }
            Add(Editor("epubApplyResource"));
            if (auto* Check = EditorMenu("epubValidateMenu")) Mode->addMenu(Check);
            Mode->addSeparator();
            if (auto* Structure = EditorMenu("epubStructureMenu")) Mode->addMenu(Structure);
            Add(Editor("epubEditNavigation"));
            Add(Editor("epubMetadataAction"));
            Mode->addSeparator();
            if (auto* Draft = EditorMenu("epubDraftMenu")) Mode->addMenu(Draft);
            Mode->addSeparator();
            Add(Editor("epubResourceFilter"));
            Mode->addSeparator();
            Run(tr("읽기 모드로 전환"), [this] { if (auto* Read = findChild<QAction*>("epubRead")) Read->trigger(); });
        });
        Mode->addAction(QString());

        // ── 보기: reading (font, layout, columns, panels) and editing (layout, panels, preview) ──
        QList<QAction*> View;
        const auto Item = [this, &Scoped](const QString& Text, const char* Name, quint32 Kinds, std::function<void()> Run, const QString& Shortcut = {}) {
            return Scoped(chromeAction(Text, QString::fromLatin1(Name), std::move(Run), Shortcut), Kinds);
        };
        View << Item(tr("글자 크게"), "epubView.larger", Read, [Profile] { Profile([](const QJsonObject& P, QJsonObject& Patch) { Patch["size"] = qMin(64.0, P["size"].toDouble(18) + 1); }); });
        View << Item(tr("글자 작게"), "epubView.smaller", Read, [Profile] { Profile([](const QJsonObject& P, QJsonObject& Patch) { Patch["size"] = qMax(8.0, P["size"].toDouble(18) - 1); }); });
        auto* Gap = new QAction(chromeOwner_); Gap->setSeparator(true); View << Scoped(Gap, Read);
        View << Scoped(RadioMenu(modeMenu_, tr("읽기 방식"), [this, Profile](QMenu* Menu) {
            auto* View = currentEpub(); const auto Layout = View ? View->ViewState()["profile"].toObject()["layout"].toString() : QString();
            auto* Group = new QActionGroup(Menu);
            auto* Scroll = Check(Menu, tr("세로 스크롤"), Layout != QLatin1String("paginated")); Group->addAction(Scroll);
            QObject::connect(Scroll, &QAction::triggered, Menu, [Profile] { Profile([](const QJsonObject&, QJsonObject& Patch) { Patch["layout"] = QStringLiteral("scroll"); }); });
            auto* Paged = Check(Menu, tr("가로 페이지"), Layout == QLatin1String("paginated")); Group->addAction(Paged);
            QObject::connect(Paged, &QAction::triggered, Menu, [Profile] { Profile([](const QJsonObject&, QJsonObject& Patch) { Patch["layout"] = QStringLiteral("paginated"); }); });
        })->menuAction(), Read);
        View << Scoped(RadioMenu(modeMenu_, tr("단 수"), [this, Profile](QMenu* Menu) {
            auto* View = currentEpub(); const int Columns = View ? View->ViewState()["profile"].toObject()["columns"].toInt() : 0;
            auto* Group = new QActionGroup(Menu);
            const QStringList Names{tr("자동"), tr("1단"), tr("2단")};
            for (int Value = 0; Value < Names.size(); ++Value) {
                auto* Row = Check(Menu, Names[Value], Columns == Value); Group->addAction(Row);
                QObject::connect(Row, &QAction::triggered, Menu, [Profile, Value] { Profile([Value](const QJsonObject&, QJsonObject& Patch) { Patch["columns"] = Value; }); });
            }
        })->menuAction(), Read);
        auto* Gap2 = new QAction(chromeOwner_); Gap2->setSeparator(true); View << Scoped(Gap2, Read);
        auto* Panels = new QMenu(tr("패널"), modeMenu_); Panels->menuAction()->setProperty("documentIconName", "sidebar");
        connect(Panels->addAction(tr("목차") + QLatin1Char('\t') + Key("epub.focus-toc")), &QAction::triggered, this, [Reader] { Reader("toggle-toc"); });
        connect(Panels->addAction(tr("검색")), &QAction::triggered, this, [Reader] { Reader("toggle-search"); });
        connect(Panels->addAction(tr("책갈피")), &QAction::triggered, this, [Reader] { Reader("toggle-bookmarks"); });
        connect(Panels->addAction(tr("형광펜 · 메모")), &QAction::triggered, this, [Reader] { Reader("toggle-notes"); });
        View << Scoped(Panels->menuAction(), Read);
        View << Item(tr("읽기 설정…"), "epubView.settings", Read, [Reader] { Reader("toggle-settings"); }, Key("epub.focus-settings"));
        // Editing: the editor's own actions, looked up when the menu opens (the editor starts with 편집).
        View << Scoped(RadioMenu(modeMenu_, tr("배치"), [Editor](QMenu* Menu) {
            for (const auto* Name : {"epubLayoutHorizontal", "epubLayoutVertical"}) if (auto* Action = Editor(Name)) Menu->addAction(Action);
        })->menuAction(), Edit);
        View << Scoped(RadioMenu(modeMenu_, tr("패널"), [Editor](QMenu* Menu) {
            for (const auto* Name : {"epubToggleTree", "epubTogglePreview", "epubToggleProblems"}) if (auto* Action = Editor(Name)) Menu->addAction(Action);
        })->menuAction(), Edit);
        View << Scoped(RadioMenu(modeMenu_, tr("미리보기"), [Editor](QMenu* Menu) {
            for (const auto* Name : {"epubAutoPreview", "epubPreviewScrollSync", "epubPreviewRefresh"}) if (auto* Action = Editor(Name)) Menu->addAction(Action);
        })->menuAction(), Edit);
        auto* Gap3 = new QAction(chromeOwner_); Gap3->setSeparator(true); View << Scoped(Gap3, Edit);
        auto* Wrap = Item(tr("자동 줄바꿈"), "epubView.wrap", Edit, {}); Wrap->setCheckable(true);
        connect(Wrap, &QAction::triggered, this, [this](bool On) { if (epubEditor_ && epubEditor_->SourceView()) epubEditor_->SourceView()->setWordWrap(On); });
        View << Wrap;
        for (auto* Action : View) if (viewKinds::Has(Action)) Action->setVisible(viewKinds::Applies(Action, K::EpubRead));
        contribution_.viewActions_ = View;
        contribution_.menu_ = modeMenu_;
    }
    void DocumentViewPane::buildMarkdownChrome()
    {
        using K = DocumentViewKind;
        const quint32 Preview = viewKinds::Of(K::MarkdownPreview), Source = viewKinds::Of(K::MarkdownEdit), Both = Preview | Source;
        const auto Key = [this](const char* Id) { return KeyText(shortcutBindings_, QString::fromLatin1(Id)); };
        const auto Scoped = [](QAction* Action, quint32 Kinds) { viewKinds::Set(Action, Kinds); return Action; };
        const auto Command = [this](const char* Id) { return [this, Id = QString::fromLatin1(Id)] { RunViewCommand(Id); }; };
        contribution_.menuTitle_ = tr("마크다운(&M)");
        modeMenu_ = new QMenu(contribution_.menuTitle_, this); modeMenu_->setObjectName("markdownModeMenu");
        auto* Mode = modeMenu_;
        Mode->addAction(Scoped(chromeAction(tr("미리보기 갱신"), "markdownMenu.refresh", Command("markdown.refresh"), Key("markdown.refresh")), Both));
        Mode->addAction(Scoped(chromeAction(tr("현재 개요 항목으로 이동"), "markdownMenu.heading", Command("markdown.currentHeading"), Key("markdown.currentHeading")), Both));
        Mode->addAction(Scoped(chromeAction(tr("렌더링 문제…"), "markdownMenu.problems", Command("markdown.problems"), Key("markdown.problems")), Both));
        // Source editing: 인코딩 · 줄 끝 · 들여쓰기 · 읽기 전용 · 편집기 글꼴 (the text menu without 언어).
        Scoped(Mode->addSeparator(), Source);
        const auto Before = Mode->actions().size();
        buildTextChrome(Mode, false);
        for (auto* Action : Mode->actions().mid(Before)) Scoped(Action, Source);
        Scoped(Mode->addSeparator(), Both);
        Mode->addAction(Scoped(chromeAction(tr("소스 · 미리보기 포커스 이동"), "markdownMenu.focus", Command("markdown.focusOtherPane"), Key("markdown.focusOtherPane")), Both));

        QList<QAction*> View;
        auto* Group = new QActionGroup(chromeOwner_);
        for (const auto& [Label, Id, Icon] : {std::tuple{tr("미리보기"), "markdown.preview", "view-preview"}, std::tuple{tr("소스 편집"), "markdown.source", "view-source"},
                 std::tuple{tr("나란히 보기"), "markdown.split", "view-split"}}) {
            auto* Item = Scoped(chromeAction(Label, QStringLiteral("markdownView.") + QString::fromLatin1(Id).mid(9), Command(Id)), Both);
            Item->setCheckable(true); Item->setProperty("markdownMode", QString::fromLatin1(Id).mid(9)); Item->setProperty("documentIconName", QString::fromLatin1(Icon));
            Group->addAction(Item); View << Item;
        }
        View << Scoped(chromeAction(tr("소스 · 미리보기 전환"), "markdownView.toggle", Command("markdown.toggleSource"), Key("markdown.toggleSource")), Both);
        auto* Gap = new QAction(chromeOwner_); Gap->setSeparator(true); View << Scoped(Gap, Both);
        auto* Outline = Scoped(chromeAction(tr("개요"), "markdownView.outline", Command("markdown.toggleToc"), Key("markdown.toggleToc")), Both); Outline->setCheckable(true);
        auto* Sync = Scoped(chromeAction(tr("스크롤 동기화"), "markdownView.sync", Command("markdown.syncScroll"), Key("markdown.syncScroll")), Both); Sync->setCheckable(true);
        View << Outline << Sync;
        auto* Gap2 = new QAction(chromeOwner_); Gap2->setSeparator(true); View << Scoped(Gap2, Both);
        // Source editing view items (자동 줄바꿈 · 탭 · 공백 · 줄 끝 표시 · 코드 접기 · 눈금자), without editor zoom.
        const auto Saved = contribution_.viewActions_;
        buildTextViewItems(false);
        for (auto* Action : contribution_.viewActions_) View << Scoped(Action, Source);
        contribution_.viewActions_ = Saved;
        View << Scoped(chromeAction(tr("미리보기 확대"), "markdownView.zoomIn", Command("markdown.zoomIn"), Key("markdown.zoomIn")), Both);
        View << Scoped(chromeAction(tr("미리보기 축소"), "markdownView.zoomOut", Command("markdown.zoomOut"), Key("markdown.zoomOut")), Both);
        View << Scoped(chromeAction(tr("미리보기 원래 크기"), "markdownView.zoomReset", Command("markdown.zoomReset"), {}), Both);
        contribution_.viewActions_ = View;
        contribution_.menu_ = modeMenu_;
        syncMarkdownChrome();
    }
    void DocumentViewPane::syncMarkdownChrome()
    {
        if (!markdown_ || !modeMenu_) return;
        const auto Kind = this->Kind();
        for (auto* Action : modeMenu_->actions() + contribution_.viewActions_) if (viewKinds::Has(Action)) Action->setVisible(viewKinds::Applies(Action, Kind));
        const auto Mode = markdown_->State()["mode"].toString();
        for (auto* Action : contribution_.viewActions_) {
            const QSignalBlocker Block(Action);
            if (Action->property("markdownMode").isValid()) Action->setChecked(Action->property("markdownMode").toString() == Mode);
            else if (Action->objectName() == QLatin1String("markdownView.outline")) { if (auto* Own = markdown_->findChild<QAction*>("markdown.toggleToc")) Action->setChecked(Own->isChecked()); }
            else if (Action->objectName() == QLatin1String("markdownView.sync")) { if (auto* Own = markdown_->findChild<QAction*>("markdown.syncScroll")) Action->setChecked(Own->isChecked()); }
        }
    }
    void DocumentViewPane::buildTextViewItems(bool Zoom)
    {
        QList<QAction*> Items;
        AppendTextViewItems(modeMenu_, Items, text_, [this](const QString& Label, const QString& Name, std::function<void()> Run, const QString& Key) {
            const auto Id = Name == "textView.zoomIn" ? QString("view.zoomIn") : Name == "textView.zoomOut" ? QString("view.zoomOut") : QString("view.zoomReset");
            return chromeAction(Label, Name, Run ? std::move(Run) : std::function<void()>([this, Id] { RunViewCommand(Id); }), Key);
        }, Zoom, shortcutBindings_);
        contribution_.viewActions_ = Items;
    }
}
