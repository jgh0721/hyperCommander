#include "stdafx.h"
#include "documentViewPane.hpp"

/// 모듈

/// UIs
#include "documentStatusPopup.hpp"
#include "imageReadingView.hpp"
#include "imageResampler.hpp"
#include "text/QTextView.h"
#include "text/TextLexerRegistry.h"
#include "pdf/QPDFView.h"
#include "image/QImageView.h"

/// 기타 헤더파일
#include "documentTextCodec.hpp"
#include "textDocumentMetrics.hpp"
#include "documentShortcutRegistry.hpp"
#include "epubSourceEditor.hpp"
#include "epubProcessView.hpp"
#include "markdownDocumentView.hpp"
#include "rstDocumentView.hpp"
#include <QJsonArray>

/// 고유 라이브러리
#include "UniqueLibs/qtMessageDialog.hpp"
#include "core/AppSettings.h"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QKeySequence>
#include <QLineEdit>
#include <QPointer>
#include <QRegularExpression>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::document
{
    namespace
    {
        // "18", "18:5" or "18,5"; Column is 0 when omitted.
        bool ParseLineColumn(const QString& Text, int& Line, int& Column)
        {
            static const QRegularExpression Pattern(QStringLiteral(R"(^\s*(\d+)\s*(?:[:,]\s*(\d+))?\s*$)"));
            const auto Match = Pattern.match(Text); if (!Match.hasMatch()) return false;
            bool Ok = false; Line = Match.captured(1).toInt(&Ok); if (!Ok) return false;
            Column = Match.captured(2).isEmpty() ? 0 : Match.captured(2).toInt();
            return true;
        }
        QString Percent(double Value) { return QStringLiteral("%1 %").arg(qRound(Value)); }
        // Display text of a command's current key; the window-level action owns the binding.
        QString ShortcutText(const QJsonObject& Stored, const QString& Id)
        {
            const auto Bindings = DocumentShortcutRegistry::Bindings(Stored);
            return QKeySequence::fromString(Bindings[Id].toString(), QKeySequence::PortableText).toString(QKeySequence::NativeText);
        }
        void SelectImageMode(QWidget* Pane, int Mode)
        {
            // The mode combo owns the switch between the single image editor and the reading views.
            if (auto* Combo = Pane->findChild<QComboBox*>("imageReadingMode")) { Combo->setCurrentIndex(Mode); emit Combo->activated(Mode); }
        }
    }
    void DocumentViewPane::OpenStatusPicker(const QString& Id, QWidget* Anchor)
    {
        if (!Anchor) return;
        QPointer<DocumentViewPane> Self(this);
        const auto Done = [Self] { if (Self) { emit Self->ViewStateChanged(); Self->Focus(); } };
        // reST 의 칸( 프로젝트 · 미리보기 배율 · 빌드 상태 · 진단 · 환경 )은 그 보기가 연다.
        if (rst_ && (Id.startsWith("rst") )) { rst_->OpenStatusPicker(Id, Anchor, Done); return; }
        if (epub_ && !IsEpubEditing()) {
            auto* View = currentEpub(); if (!View) return;
            const auto State = View->ViewState(); const auto Profile = State["profile"].toObject();
            // Reading settings go through the reader's own path (preview a patch, then apply).
            const auto Apply = [View](const QJsonObject& Patch) { if (View->ChangeReadingSettings(QStringLiteral("preview"), Patch, false)) View->ChangeReadingSettings(QStringLiteral("apply"), {}, false); };
            if (Id == "chapter") {
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("장으로 이동"));
                QList<QPair<QString, QString>> Items; QList<int> Chapters;
                const auto Toc = State["toc"].toArray(); const int Count = State["chapterCount"].toInt();
                for (const auto& Entry : Toc) { const auto Item = Entry.toObject(); Items.append({Item["title"].toString(), tr("%1 / %2").arg(Item["chapter"].toInt() + 1).arg(Count)}); Chapters << Item["chapter"].toInt(); }
                if (Items.isEmpty()) for (int Chapter = 0; Chapter < Count; ++Chapter) { Items.append({tr("%1장").arg(Chapter + 1), {}}); Chapters << Chapter; }
                const int Current = Chapters.indexOf(State["chapterIndex"].toInt());
                auto* Search = Items.size() > 12 ? Popup->AddSearch(Popup->Root(), tr("장 검색")) : nullptr;
                Popup->AddList(Popup->Root(), Items, Current, [Popup, View, Chapters, Done](int Index) {
                    View->ReaderAction(QStringLiteral("chapter"), Chapters.value(Index)); Popup->close(); Done();
                }, Search);
                Popup->ShowAbove(); return;
            }
            if (Id == "epubLayout") {
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("읽기 방식"));
                const bool Paged = Profile["layout"] == QLatin1String("paginated");
                Popup->AddChips(Popup->Root(), {tr("세로 스크롤"), tr("가로 페이지")}, Paged ? 1 : 0, [Apply](int Index) {
                    Apply({{"layout", Index == 1 ? QStringLiteral("paginated") : QStringLiteral("scroll")}});
                });
                Popup->AddNote(Popup->Root(), tr("단 수"));
                Popup->AddChips(Popup->Root(), {tr("자동"), tr("1단"), tr("2단")}, Profile["columns"].toInt(), [Apply](int Index) { Apply({{"columns", Index}}); });
                Popup->AddNote(Popup->Root(), tr("단 수는 가로 페이지에서 쓰입니다. 화면이 좁으면 1단으로 보입니다."));
                Popup->ShowAbove(); return;
            }
            if (Id == "font") {
                const double Size = Profile["size"].toDouble(18);
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("글자 크기 — %1 px").arg(Size));
                Popup->AddChips(Popup->Root(), {tr("작게"), tr("%1 px").arg(Size), tr("크게")}, 1, [Apply, Size](int Index) {
                    if (Index != 1) Apply({{"size", qBound(8.0, Size + (Index == 0 ? -1 : 1), 64.0)}});
                });
                Popup->AddAction(Popup->Root(), tr("읽기 설정 열기…"), ShortcutText(shortcutBindings_, "epub.focus-settings"), [View] { View->ReaderAction(QStringLiteral("toggle-settings")); });
                Popup->ShowAbove(); return;
            }
        }
        if (markdown_) {
            auto* Markdown = markdown_;
            if (Id == "heading") {
                // Outline headings indented by depth; the current one is marked.
                const auto Headings = Markdown->Headings();
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("현재 제목"));
                QList<QPair<QString, QString>> Items;
                for (int Index = 0; Index < Headings.size(); ++Index)
                    Items.append({QString(Markdown->HeadingDepth(Index), QChar(0x3000)) + Headings[Index].first, tr("줄 %1").arg(Headings[Index].second)});
                if (Items.isEmpty()) Popup->AddNote(Popup->Root(), tr("이 문서에는 제목이 없습니다."));
                else {
                    auto* Search = Items.size() > 12 ? Popup->AddSearch(Popup->Root(), tr("제목 검색")) : nullptr;
                    Popup->AddList(Popup->Root(), Items, Markdown->CurrentHeadingIndex(), [Popup, Markdown, Done](int Index) {
                        Markdown->GoToHeading(Index); Popup->close(); Done();
                    }, Search);
                }
                Popup->ShowAbove(); return;
            }
            if (Id == "mdzoom") {
                const int Zoom = Markdown->PreviewZoomPercent();
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("미리보기 배율 — %1 %").arg(Zoom));
                const QList<int> Steps{75, 90, 100, 110, 125, 150};
                QStringList Labels; for (const int Step : Steps) Labels << tr("%1 %").arg(Step);
                Popup->AddChips(Popup->Root(), Labels, Steps.indexOf(Zoom), [Markdown, Steps, Done](int Index) { Markdown->SetPreviewZoom(Steps.value(Index, 100)); Done(); });
                Popup->AddAction(Popup->Root(), tr("원래 크기"), QStringLiteral("100 %"), [Self, Done] { if (Self) Self->RunViewCommand(QStringLiteral("markdown.zoomReset")); Done(); });
                Popup->AddAction(Popup->Root(), tr("배율 입력…"), tr("%1–%2 %").arg(MarkdownDocumentView::MinZoomPercent).arg(MarkdownDocumentView::MaxZoomPercent),
                    [Self] { if (Self) Self->RunViewCommand(QStringLiteral("markdown.zoomSet")); });
                Popup->ShowAbove(); return;
            }
            if (Id == "render") {
                const auto At = Markdown->RenderedAt();
                const auto Fields = Markdown->StatusFields();
                QString Text; for (const auto& Field : Fields) if (Field.id_ == "render") Text = Field.text_;
                auto* Popup = DocumentStatusPopup::Open(Anchor, At.isValid() ? tr("미리보기 렌더링 — %1 · %2").arg(Text, QLocale().toString(At.time(), QLocale::ShortFormat)) : tr("미리보기 렌더링 — %1").arg(Text));
                Popup->AddAction(Popup->Root(), tr("미리보기 갱신"), ShortcutText(shortcutBindings_, "markdown.refresh"), [Self, Done] { if (Self) Self->RunViewCommand(QStringLiteral("markdown.refresh")); Done(); });
                const auto Problems = Markdown->RenderProblems();
                if (Problems.isEmpty()) Popup->AddNote(Popup->Root(), tr("수식 · 다이어그램 오류 0건. 오류가 있으면 ‘렌더링 문제 n건’으로 바뀌고 여기서 문제 위치로 이동할 수 있습니다."));
                else {
                    Popup->AddWarning(Popup->Root(), tr("수식 · 다이어그램 오류 %1건 — 고를 때 소스 위치로 이동합니다.").arg(Markdown->RenderProblemCount()));
                    QList<QPair<QString, QString>> Items; QList<int> Lines;
                    for (const auto& Value : Problems) {
                        const auto Problem = Value.toObject();
                        Items.append({Problem["message"].toString().simplified().left(120), tr("줄 %1 · %2").arg(Problem["line"].toInt()).arg(Problem["kind"].toString())});
                        Lines << Problem["line"].toInt();
                    }
                    Popup->AddList(Popup->Root(), Items, -1, [Self, Popup, Lines, Done](int Index) {
                        Popup->close();
                        if (!Self || !Self->text_) return;
                        if (!Self->markdown_->SourceVisible()) Self->RunViewCommand(QStringLiteral("markdown.split"));
                        Self->text_->goToLine(Lines.value(Index, 1));
                        Done();
                    }, nullptr);
                }
                Popup->ShowAbove(); return;
            }
        }
        if (IsEpubEditing() && epubEditor_) {
            auto* Editor = epubEditor_; auto* Source = Editor->SourceView();
            const auto Run = [Editor](const char* Name) { if (auto* Item = Editor->findChild<QAction*>(QString::fromLatin1(Name)); Item && Item->isEnabled()) Item->trigger(); };
            if (Id == "position" && Source) {
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("줄로 이동"));
                const int Lines = Source->lineCount();
                Popup->AddInput(Popup->Root(), QStringLiteral("%1:%2").arg(Source->currentLine()).arg(Source->currentColumn()), tr("줄[:열] · 1–%1").arg(Lines),
                    [Source, Lines, Done](const QString& Value) {
                        int Line = 0, Column = 0;
                        if (!ParseLineColumn(Value, Line, Column) || Line < 1 || Line > Lines) return false;
                        if (Column > 0) Source->goToPosition(Line, Column); else Source->goToLine(Line);
                        Done(); return true;
                    });
                Popup->ShowAbove(); return;
            }
            if (Id == "encoding") {
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("인코딩 — UTF-8"));
                Popup->AddNote(Popup->Root(), tr("EPUB 리소스(XHTML · CSS · OPF)는 규격에 따라 UTF-8로 읽고 저장합니다. 다른 인코딩으로 바꿀 수 없습니다."));
                Popup->ShowAbove(); return;
            }
            if (Id == "eol" && Source) {
                const auto Stats = CountLineEndings(QStringView(Source->Content()));
                const auto Dominant = Stats.Dominant();
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("줄 끝 — %1").arg(Dominant == "MIXED" ? tr("혼합") : Dominant.isEmpty() ? QStringLiteral("LF") : Dominant));
                Popup->AddNote(Popup->Root(), tr("현재 리소스만 바꿉니다."));
                for (const auto& [Name, Ending] : {std::pair{QString("CRLF"), QTextView::CRLF}, std::pair{QString("LF"), QTextView::LF}, std::pair{QString("CR"), QTextView::CR}}) {
                    if (Dominant == Name) continue;
                    const auto Changes = Stats.Breaks() - (Name == "CRLF" ? Stats.crlf_ : Name == "LF" ? Stats.lf_ : Stats.cr_);
                    auto* Button = Popup->AddAction(Popup->Root(), tr("모든 줄을 %1로 변환").arg(Name), tr("%1줄 변경").arg(Changes), [Self, Source, Ending] {
                        Source->ConvertLineEnding(Ending); if (Self) emit Self->ViewStateChanged();
                    });
                    Button->setEnabled(!Source->isReadOnly() && Changes > 0);
                }
                Popup->ShowAbove(); return;
            }
            if (Id == "validation") {
                QString State; for (const auto& Field : Editor->StatusFields()) if (Field.id_ == "validation") State = Field.text_;
                auto* Popup = DocumentStatusPopup::Open(Anchor, State.isEmpty() ? tr("검사") : tr("검사 — %1").arg(State));
                Popup->AddAction(Popup->Root(), tr("기본 검사"), ShortcutText(shortcutBindings_, "epubEdit.validate"), [Run] { Run("epubBasicValidation"); });
                Popup->AddAction(Popup->Root(), tr("EPUBCheck 검사"), ShortcutText(shortcutBindings_, "epubEdit.epubcheck"), [Run] { Run("epubStandardValidation"); });
                Popup->AddNote(Popup->Root(), tr("문제 수를 누르면 하단 문제 패널로 이동합니다."));
                Popup->ShowAbove(); return;
            }
        }
        if (text_) {
            auto* Text = text_;
            if (Id == "position") {
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("줄로 이동"));
                const int Lines = Text->lineCount();
                Popup->AddInput(Popup->Root(), QStringLiteral("%1:%2").arg(Text->currentLine()).arg(Text->currentColumn()), tr("줄[:열] · 1–%1").arg(Lines),
                    [Text, Lines, Done](const QString& Value) {
                        int Line = 0, Column = 0;
                        if (!ParseLineColumn(Value, Line, Column) || Line < 1 || Line > Lines) return false;
                        if (Column > 0) Text->goToPosition(Line, Column); else Text->goToLine(Line);
                        Done(); return true;
                    });
                Popup->AddNote(Popup->Root(), tr("Enter로 이동, Esc로 본문 복귀. 편집 › 줄로 이동…(Ctrl+G)과 같은 동작."));
                Popup->ShowAbove(); return;
            }
            if (Id == "language") {
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("언어 (구문 강조)"));
                const auto Names = TextLexerRegistry::instance().displayNames();
                const auto Suffix = QFileInfo(Text->currentFilePath()).suffix();
                const auto ByExtension = Suffix.isEmpty() ? QString() : TextLexerRegistry::instance().languageForExtension(Suffix);
                QList<QPair<QString, QString>> Items; int Current = -1;
                for (int I = 0; I < Names.size(); ++I) {
                    Items.append({Names[I], Names[I].compare(ByExtension, Qt::CaseInsensitive) == 0 ? tr("확장자 .%1").arg(Suffix) : QString()});
                    if (Names[I].compare(Text->currentLanguage(), Qt::CaseInsensitive) == 0) Current = I;
                }
                auto* Search = Popup->AddSearch(Popup->Root(), tr("언어 검색"));
                Popup->AddList(Popup->Root(), Items, Current, [Popup, Text, Names, Done](int Index) {
                    Text->setLanguage(Names.value(Index)); Popup->close(); Done();
                }, Search);
                Popup->ShowAbove(); return;
            }
            if (Id == "encoding") {
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("인코딩 — %1%2").arg(EncodingInfo(encoding_).label_.isEmpty() ? encoding_ : EncodingInfo(encoding_).label_, bom_ ? tr(" (BOM)") : QString()));
                const auto ReadLabel = EncodingInfo(readEncoding_).label_.isEmpty() ? readEncoding_ : EncodingInfo(readEncoding_).label_;
                Popup->AddNote(Popup->Root(), readDetection_ == TextDetection::Explicit ? tr("직접 지정: %1").arg(ReadLabel)
                    : tr("자동 감지: %1 · %2").arg(ReadLabel, TextDetectionLabel(readDetection_)));
                if (CanonicalEncodingName(readEncoding_) != CanonicalEncodingName(encoding_))
                    Popup->AddNote(Popup->Root(), tr("다음 저장부터 %1(으)로 변환합니다.").arg(EncodingInfo(encoding_).label_));
                const auto Pages = Popup->AddTabs({tr("다시 읽기 (해석만)"), tr("변환하여 저장")}, 0);
                // Reinterpret: the bytes on disk are decoded again; edits are not carried over.
                Popup->AddNote(Pages[0], tr("파일 바이트는 그대로 두고 해석만 바꿉니다. 글자가 깨져 보일 때 사용합니다."));
                const auto Reads = ReadEncodings();
                QList<QPair<QString, QString>> ReadItems; int ReadCurrent = -1;
                for (int I = 0; I < Reads.size(); ++I) {
                    ReadItems.append({Reads[I].label_, Reads[I].note_});
                    if (CanonicalEncodingName(Reads[I].name_) == CanonicalEncodingName(readEncoding_)) ReadCurrent = I;
                }
                auto* Search = Popup->AddSearch(Pages[0], tr("인코딩 검색"));
                Popup->AddList(Pages[0], ReadItems, ReadCurrent, [Self, Popup, Reads](int Index) {
                    Popup->close(); if (Self) emit Self->ReinterpretRequested(Reads.value(Index).name_);
                }, Search);
                Popup->AddWarning(Pages[0], tr("편집 내용이 있으면 다시 읽기 전에 폐기 여부를 묻습니다."));
                // Convert: the current text is written in the chosen encoding from the next save on.
                Popup->AddNote(Pages[1], tr("현재 내용을 선택한 인코딩으로 변환해 다음 저장부터 씁니다. 저장 전까지 원본 파일은 바뀌지 않습니다."));
                auto Saves = SaveEncodings();
                if (std::none_of(Saves.begin(), Saves.end(), [this](const auto& Info) { return CanonicalEncodingName(Info.name_) == CanonicalEncodingName(encoding_); })) {
                    const auto Info = EncodingInfo(encoding_); if (!Info.name_.isEmpty()) Saves.prepend(Info);
                }
                QList<QPair<QString, QString>> SaveItems; int SaveCurrent = -1;
                for (int I = 0; I < Saves.size(); ++I) {
                    const auto Missing = FindUnrepresentable(Text->Content(), Saves[I].name_);
                    SaveItems.append({Saves[I].label_, Missing.count_ ? tr("표현 불가 %1자").arg(Missing.count_) : Saves[I].note_});
                    if (CanonicalEncodingName(Saves[I].name_) == CanonicalEncodingName(encoding_)) SaveCurrent = I;
                }
                auto* Bom = new QPointer<QCheckBox>;
                Popup->AddList(Pages[1], SaveItems, SaveCurrent, [Self, Text, Saves, Bom](int Index) {
                    if (!Self) return;
                    const auto Info = Saves.value(Index);
                    QString Code;
                    if (!Self->SetSaveEncoding(Info.name_, Self->bom_, &Code)) {
                        const auto Missing = FindUnrepresentable(Text->Content(), Info.name_);
                        if (Missing.firstOffset_ >= 0) {
                            const auto Where = PositionOfOffset(QStringView(Text->Content()), Missing.firstOffset_);
                            Text->goToPosition(Where.line_, Where.column_);
                        }
                        hc::ui::MessageDialog::warning(Self, tr("변환하여 저장"),
                            tr("현재 내용에 %1(으)로 표현할 수 없는 문자가 %2자 있어 바꾸지 않았습니다. 첫 위치로 이동했습니다.").arg(Info.label_).arg(Missing.count_));
                        return;
                    }
                    if (*Bom) { (*Bom)->setEnabled(Info.bomCapable_); const QSignalBlocker Block(*Bom); (*Bom)->setChecked(Self->bom_); }
                    emit Self->ViewStateChanged();
                });
                *Bom = Popup->AddCheck(Pages[1], tr("BOM 포함"), bom_, [Self](bool On) { if (Self) Self->SetSaveEncoding(Self->encoding_, On); });
                (*Bom)->setEnabled(EncodingInfo(encoding_).bomCapable_);
                connect(Popup, &QObject::destroyed, Popup, [Bom] { delete Bom; });
                Popup->AddWarning(Pages[1], tr("표현할 수 없는 문자가 있으면 바꾸지 않고 첫 위치를 표시합니다."));
                Popup->ShowAbove(); return;
            }
            if (Id == "eol") {
                const auto Stats = CountLineEndings(QStringView(Text->Content()));
                const auto Detected = Stats.Dominant();
                // Without line breaks, use the editor mode applied from new-file settings.
                const auto Dominant = Detected.isEmpty()
                    ? QString(Text->detectedLineEnding() == QTextView::CRLF ? "CRLF" : Text->detectedLineEnding() == QTextView::CR ? "CR" : "LF")
                    : Detected;
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("줄 끝 — %1").arg(Dominant == "MIXED" ? tr("혼합") : Dominant));
                Popup->AddNote(Popup->Root(), Dominant == "MIXED"
                    ? tr("혼합 (CRLF %1 · LF %2 · CR %3)").arg(Stats.crlf_).arg(Stats.lf_).arg(Stats.cr_)
                    : tr("%1줄 모두 %2 · 혼합 없음").arg(Stats.Lines()).arg(Dominant));
                for (const auto& [Name, Ending] : {std::pair{QString("CRLF"), QTextView::CRLF}, std::pair{QString("LF"), QTextView::LF}, std::pair{QString("CR"), QTextView::CR}}) {
                    if (Dominant == Name) continue;
                    const auto Changes = Stats.Breaks() - (Name == "CRLF" ? Stats.crlf_ : Name == "LF" ? Stats.lf_ : Stats.cr_);
                    auto* Button = Popup->AddAction(Popup->Root(), tr("모든 줄을 %1로 변환").arg(Name), tr("%1줄 변경").arg(Changes), [Self, Text, Ending, Name] {
                        if (!Self) return;
                        Text->ConvertLineEnding(Ending); Self->eol_ = Name; Self->statusEol_ = Name; emit Self->ViewStateChanged(); Self->Focus();
                    });
                    Button->setEnabled(!Text->isReadOnly() && Changes > 0);
                }
                Popup->ShowAbove(); return;
            }
            if (Id == "indent") {
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("들여쓰기 — %1 %2").arg(Text->useTabs() ? tr("탭") : tr("공백")).arg(Text->tabWidth()));
                const auto Guess = DetectIndent(QStringView(Text->Content()));
                Popup->AddNote(Popup->Root(), tr("탭 들여쓰기 %1줄 · 공백 들여쓰기 %2줄").arg(Guess.tabLines_).arg(Guess.spaceLines_));
                Popup->AddChips(Popup->Root(), {tr("탭"), tr("공백")}, Text->useTabs() ? 0 : 1, [Self, Text](int Index) {
                    Text->SetDocumentIndentation(Index == 0, Text->tabWidth()); if (Self) emit Self->ViewStateChanged();
                });
                const QList<int> Widths{2, 4, 8};
                Popup->AddChips(Popup->Root(), {QStringLiteral("2"), QStringLiteral("4"), QStringLiteral("8"), tr("사용자 지정…")}, Widths.indexOf(Text->tabWidth()), [Self, Popup, Text, Widths](int Index) {
                    if (Index < Widths.size()) { Text->SetDocumentIndentation(Text->useTabs(), Widths[Index]); if (Self) emit Self->ViewStateChanged(); return; }
                    Popup->close();
                    bool Ok = false;
                    const int Width = hc::ui::InputDialog::getInt(Self, tr("들여쓰기 크기"), tr("크기"), Text->tabWidth(), 1, 16, 1, &Ok);
                    if (Ok) { Text->SetDocumentIndentation(Text->useTabs(), Width); if (Self) emit Self->ViewStateChanged(); }
                });
                for (const bool ToTabs : {true, false}) {
                    auto* Button = Popup->AddAction(Popup->Root(), ToTabs ? tr("들여쓰기를 탭으로 변환") : tr("들여쓰기를 공백으로 변환"), {}, [Self, Text, ToTabs] {
                        Text->ConvertIndentation(ToTabs); if (Self) { emit Self->ViewStateChanged(); Self->Focus(); }
                    });
                    Button->setEnabled(!Text->isReadOnly());
                }
                Popup->AddCheck(Popup->Root(), tr("파일을 열 때 자동 감지"), AppSettings().value("textView/detectIndent").toBool(), [](bool On) {
                    AppSettings Settings; Settings.setValue("textView/detectIndent", On);
                });
                Popup->ShowAbove(); return;
            }
        }
        if (pdf_) {
            auto* Pdf = pdf_;
            if (Id == "page") {
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("쪽으로 이동"));
                const int Count = Pdf->totalPageCount();
                Popup->AddInput(Popup->Root(), QString::number(Pdf->currentPage() + 1), QStringLiteral("1–%1").arg(Count), [Pdf, Count, Done](const QString& Value) {
                    bool Ok = false; const int Page = Value.trimmed().toInt(&Ok);
                    if (!Ok || Page < 1 || Page > Count) return false;
                    Pdf->goToPage(Page - 1); Done(); return true;
                });
                // 이전 위치: the page before the last jump (first/last page or a page number).
                const int Back = Pdf->PreviousLocation();
                QStringList Chips{tr("첫 쪽"), tr("마지막 쪽")}; if (Back >= 0) Chips << tr("이전 위치 (%1쪽)").arg(Back + 1);
                Popup->AddChips(Popup->Root(), Chips, -1, [Popup, Pdf, Back, Done](int Index) {
                    Pdf->goToPage(Index == 0 ? 0 : Index == 1 ? Pdf->totalPageCount() - 1 : Back); Popup->close(); Done();
                });
                Popup->ShowAbove(); return;
            }
            if (Id == "zoom") {
                const QStringList Fits{tr("너비 맞춤"), tr("높이 맞춤"), tr("쪽 맞춤"), tr("사용자 배율")};
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("배율 — %1 · %2").arg(Percent(Pdf->zoomLevel()), Fits.value(int(Pdf->zoomMode()))));
                const QList<int> Levels{50, 75, 100, 125, 150, 200, 400};
                QStringList Labels; for (const int Level : Levels) Labels << Percent(Level);
                Popup->AddChips(Popup->Root(), Labels, Pdf->zoomMode() == QPDFView::CustomZoom ? Levels.indexOf(qRound(Pdf->zoomLevel())) : -1, [Pdf, Levels, Done](int Index) {
                    Pdf->setZoomLevel(Levels.value(Index, 100)); Done();
                });
                const int Fit = Pdf->zoomMode() == QPDFView::FitWidth ? 0 : Pdf->zoomMode() == QPDFView::FitPage ? 1 : -1;
                Popup->AddChips(Popup->Root(), {tr("너비 맞춤"), tr("쪽 맞춤"), tr("실제 크기")}, Fit, [Pdf, Done](int Index) {
                    if (Index == 0) Pdf->setZoomMode(QPDFView::FitWidth); else if (Index == 1) Pdf->setZoomMode(QPDFView::FitPage); else Pdf->setZoomLevel(100);
                    Done();
                });
                Popup->ShowAbove(); return;
            }
            if (Id == "pdfLayout") {
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("쪽 배치 · 넘김 방향"));
                Popup->AddChips(Popup->Root(), {tr("한 쪽"), tr("연속 스크롤"), tr("두 쪽")}, int(Pdf->viewMode()), [Pdf, Done](int Index) {
                    Pdf->setViewMode(static_cast<QPDFView::ViewMode>(Index)); Done();
                });
                Popup->AddChips(Popup->Root(), {tr("좌 → 우"), tr("우 → 좌")}, int(Pdf->spreadReadingDirection()), [Pdf, Done](int Index) {
                    Pdf->setSpreadReadingDirection(Index == 0 ? QPDFView::LeftToRight : QPDFView::RightToLeft); Done();
                });
                Popup->AddNote(Popup->Root(), tr("넘김 방향은 두 쪽 배치에서 쓰입니다."));
                Popup->ShowAbove(); return;
            }
            if (Id == "rotation") {
                // View rotation: the document is not changed (PDF › 현재 쪽 회전 (편집) edits a page).
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("회전 — %1°").arg(Pdf->viewRotation()));
                const QList<int> Angles{0, 90, 180, 270};
                Popup->AddChips(Popup->Root(), {QStringLiteral("0°"), QStringLiteral("90°"), QStringLiteral("180°"), QStringLiteral("270°")}, Angles.indexOf(Pdf->viewRotation()),
                    [Pdf, Angles, Done](int Index) { Pdf->setViewRotation(Angles.value(Index)); Done(); });
                Popup->AddNote(Popup->Root(), tr("보기만 돌립니다. 저장되는 쪽 회전은 PDF › 현재 쪽 회전 (편집)을 쓰십시오."));
                Popup->ShowAbove(); return;
            }
        }
        if (image_) {
            auto* Image = image_; auto* Reading = reading_;
            const bool Book = Reading && !Reading->isHidden();
            const auto State = Book ? Reading->State() : QJsonObject{};
            if (Id == "fit") {
                const QStringList Fits{tr("원본"), tr("창에 맞춤"), tr("너비"), tr("높이"), tr("늘림")};
                const int Current = Book ? State["fit"].toInt() : int(Image->fitMode());
                auto* Popup = DocumentStatusPopup::Open(Anchor, Book ? tr("맞춤 — %1").arg(Fits.value(Current))
                    : tr("맞춤 — %1 · %2").arg(Fits.value(Current), Percent(Image->displayScalePercent())));
                // Chips follow the prototype order (창에 맞춤 · 너비 · 높이 · 원본 · 늘림); values are FitMode.
                const QList<int> Order = Book ? QList<int>{1, 2, 3, 0} : QList<int>{1, 2, 3, 0, 4};
                QStringList Labels; for (const int Value : Order) Labels << Fits[Value];
                Popup->AddChips(Popup->Root(), Labels, Order.indexOf(Current), [Image, Reading, Book, Order, Done](int Index) {
                    if (Book) Reading->SetFit(Order[Index]); else Image->setFitMode(static_cast<QImageView::FitMode>(Order[Index]));
                    Done();
                });
                if (!Book) {
                    const QList<int> Levels{25, 50, 100, 200};
                    QStringList Zooms; for (const int Level : Levels) Zooms << Percent(Level);
                    Popup->AddChips(Popup->Root(), Zooms, -1, [Image, Levels, Done](int Index) { Image->setZoomLevel(Levels.value(Index, 100)); Done(); });
                }
                Popup->ShowAbove(); return;
            }
            if (Id == "imageMode") {
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("보기 방식"));
                const int Current = Book ? (State["mode"].toInt() == ImageReadingView::Webtoon ? 2 : 1) : 0;
                Popup->AddChips(Popup->Root(), {tr("일반 이미지"), tr("만화"), tr("웹툰")}, Current, [Self, Popup](int Index) {
                    Popup->close(); if (Self) { SelectImageMode(Self, Index); emit Self->ViewStateChanged(); }
                });
                Popup->AddNote(Popup->Root(), tr("보기 › 보기 방식과 같습니다."));
                Popup->ShowAbove(); return;
            }
            if (Id == "resample") {
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("보기 품질 (리샘플링)"));
                Popup->AddNote(Popup->Root(), tr("화면 표시용 확대·축소 필터입니다. 저장 결과에는 영향이 없습니다."));
                QStringList Ids{QStringLiteral("auto")}; QList<QPair<QString, QString>> Items{{tr("자동"), {}}};
                for (const auto& Filter : ImageFilterRegistry::Filters()) { Ids << Filter.id_; Items.append({Filter.label_, {}}); }
                const auto Current = Book ? State["filter"].toString() : Image->DisplayFilter();
                Popup->AddList(Popup->Root(), Items, qMax(0, int(Ids.indexOf(Current))), [Popup, Image, Reading, Ids, Done](int Index) {
                    Image->SetDisplayFilter(Ids.value(Index)); if (Reading) Reading->SetFilter(Ids.value(Index));
                    Popup->close(); Done();
                });
                Popup->ShowAbove(); return;
            }
            if (Book && Id == "page") {
                const bool Webtoon = State["mode"].toInt() == ImageReadingView::Webtoon;
                const int Count = State["pageCount"].toInt();
                auto* Popup = DocumentStatusPopup::Open(Anchor, Webtoon ? tr("이미지로 이동") : tr("쪽으로 이동"));
                Popup->AddInput(Popup->Root(), QString::number(State["page"].toInt() + 1), QStringLiteral("1–%1").arg(Count), [Reading, Count, Done](const QString& Value) {
                    bool Ok = false; const int Page = Value.trimmed().toInt(&Ok);
                    if (!Ok || Page < 1 || Page > Count) return false;
                    Reading->GoTo(Page - 1); Done(); return true;
                });
                Popup->AddChips(Popup->Root(), {Webtoon ? tr("첫 이미지") : tr("첫 쪽"), Webtoon ? tr("마지막 이미지") : tr("마지막 쪽"), tr("책갈피로 이동")}, -1,
                    [Popup, Reading, Count, Done](int Index) {
                        if (Index == 0) Reading->GoTo(0); else if (Index == 1) Reading->GoTo(Count - 1); else Reading->OpenBookmark();
                        Popup->close(); Done();
                    });
                Popup->ShowAbove(); return;
            }
            if (Book && Id == "spread") {
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("펼침 · 방향"));
                const int Mode = State["mode"].toInt();
                Popup->AddChips(Popup->Root(), {tr("한 쪽"), tr("두 쪽")}, Mode == ImageReadingView::Comic ? 1 : 0, [Reading, Done](int Index) {
                    Reading->SetMode(Index == 1 ? ImageReadingView::Comic : ImageReadingView::Single); Done();
                });
                Popup->AddCheck(Popup->Root(), tr("표지 단독"), State["cover"].toBool(), [Reading, Done](bool On) {
                    Reading->SetCover(On); AppSettings().setValue("imageView/comic/coverAlone", On); Done();
                });
                Popup->AddCheck(Popup->Root(), tr("펼침면 분할"), State["split"].toBool(), [Reading, Done](bool On) { Reading->SetSplit(On); Done(); });
                Popup->AddChips(Popup->Root(), {tr("왼쪽 → 오른쪽"), tr("오른쪽 → 왼쪽")}, State["rtl"].toBool() ? 1 : 0, [Reading, Done](int Index) {
                    Reading->SetRightToLeft(Index == 1); Done();
                });
                Popup->AddNote(Popup->Root(), tr("보기 방식"));
                Popup->AddChips(Popup->Root(), {tr("일반 이미지"), tr("만화"), tr("웹툰")}, 1, [Self, Popup](int Index) {
                    if (Index == 1) return;
                    Popup->close(); if (Self) { SelectImageMode(Self, Index); emit Self->ViewStateChanged(); }
                });
                Popup->ShowAbove(); return;
            }
            if (Book && Id == "webtoon") {
                const int Gap = State["gap"].toInt();
                auto* Popup = DocumentStatusPopup::Open(Anchor, tr("세로 연속"));
                Popup->AddNote(Popup->Root(), tr("이미지 간격"));
                const QList<int> Gaps{0, 8, 16};
                Popup->AddChips(Popup->Root(), {tr("0 px"), tr("8 px"), tr("16 px"), tr("사용자 지정…")}, Gaps.indexOf(Gap), [Self, Popup, Reading, Gaps, Done](int Index) {
                    if (Index < Gaps.size()) { Reading->SetGap(Gaps[Index]); Done(); return; }
                    Popup->close();
                    bool Ok = false;
                    const int Value = hc::ui::InputDialog::getInt(Self, tr("이미지 간격"), tr("간격 (DIP)"), Reading->State()["gap"].toInt(), 0, 200, 1, &Ok);
                    if (Ok) { Reading->SetGap(Value); Done(); }
                });
                Popup->ShowAbove(); return;
            }
        }
        // A field without a picker of its own keeps its command.
        RunStatusCommand(Id == "position" ? QStringLiteral("text.goToLine") : Id == "page" ? QStringLiteral("status.page") : Id == "zoom" ? QStringLiteral("status.zoom") : Id);
    }
}
