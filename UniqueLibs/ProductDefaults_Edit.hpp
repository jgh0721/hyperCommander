#pragma once
#include <array>
#include <type_traits>
#include <QtCore>

// HyperViewer / HyperEditor 제품 기본값의 편집 지점.
// 저장된 값(빈 문자열·단축키 해제 포함)이 우선하며 기본값은 파일에 미리 쓰지 않는다.
// 키는 documentViews 아래 경로다. Viewer / Editor의 서로 다른 시작 모드는 별도로 둔다.

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariant>

namespace hc::defaults::edit
{
    struct Command { QString id_, label_, key_, scope_; };

    inline QList<Command> Commands()
    {
        return {
            {"markdown.toggleSource",QObject::tr("소스·미리보기 전환"),"Alt+O","markdown"},
            {"markdown.preview",QObject::tr("미리보기"),{},"markdown"},
            {"markdown.source",QObject::tr("소스 보기·편집"),{},"markdown"},
            {"markdown.split",QObject::tr("나란히 보기"),{},"markdown"},
            {"markdown.toggleToc",QObject::tr("개요 표시 전환"),"Ctrl+Alt+O","markdown"},
            {"markdown.syncScroll",QObject::tr("스크롤 동기화"),{},"markdown"},
            {"markdown.refresh",QObject::tr("미리보기 갱신"),"Ctrl+Alt+R","markdown"},
            {"markdown.focusOtherPane",QObject::tr("소스·미리보기 포커스 이동"),"F6","markdown"},
            {"markdown.find",QObject::tr("마크다운 찾기"),{},"markdown"},
            {"markdown.zoomIn",QObject::tr("미리보기 확대"),{},"markdown"},
            {"markdown.zoomOut",QObject::tr("미리보기 축소"),{},"markdown"},
            {"markdown.fullScreen",QObject::tr("미리보기 전체 화면"),"F11","markdown"},
            // reStructuredText: Markdown 과 같은 키. 두 범위는 함께 켜지지 않는다.
            {"rst.toggleSource",QObject::tr("소스·미리보기 전환"),"Alt+O","rst"},
            {"rst.preview",QObject::tr("미리보기"),{},"rst"},
            {"rst.source",QObject::tr("소스 보기·편집"),{},"rst"},
            {"rst.split",QObject::tr("나란히 보기"),{},"rst"},
            {"rst.toggleToc",QObject::tr("개요 표시 전환"),"Ctrl+Alt+O","rst"},
            {"rst.toggleProblems",QObject::tr("진단 패널 표시 전환"),"Ctrl+Alt+D","rst"},
            {"rst.syncScroll",QObject::tr("스크롤 동기화"),{},"rst"},
            {"rst.rebuild",QObject::tr("미리보기 다시 빌드"),"Ctrl+Alt+R","rst"},
            {"rst.fullScreen",QObject::tr("미리보기 전체 화면"),"F11","rst"},
            {"rst.focusOtherPane",QObject::tr("소스·미리보기 포커스 이동"),"F6","rst"},
            {"rst.complete",QObject::tr("자동 완성"),"Ctrl+Space","rst"},
            {"rst.goToReference",QObject::tr("참조로 이동"),"F12","rst"},
            {"rst.buildProject",QObject::tr("프로젝트 빌드"),{},"rst"},
            {"rst.find",QObject::tr("reStructuredText 찾기"),{},"rst"},
            {"rst.zoomIn",QObject::tr("미리보기 확대"),{},"rst"},
            {"rst.zoomOut",QObject::tr("미리보기 축소"),{},"rst"},
            {"file.new", QObject::tr("새 문서"), "Ctrl+N", "common"},
            {"file.newText", QObject::tr("새 텍스트 문서"), "", "common"},
            {"file.newMarkdown", QObject::tr("새 Markdown 문서"), "", "common"},
            {"file.newRst", QObject::tr("새 reStructuredText 문서"), "", "common"},
            {"file.newSphinxProject", QObject::tr("새 Sphinx 프로젝트"), "", "common"},
            {"file.newEpub", QObject::tr("새 EPUB 책"), "", "common"},
            {"file.newImage", QObject::tr("새 이미지"), "", "common"},
            {"file.openWorkspace", QObject::tr("문서 열기"), "Ctrl+O", "common"},
            {"file.save", QObject::tr("저장"), "Ctrl+S", "common"},
            {"file.saveAs", QObject::tr("다른 이름으로 저장"), "Ctrl+Shift+S", "common"},
            {"file.reload", QObject::tr("다시 읽기"), "Ctrl+R", "common"},
            {"file.cancel", QObject::tr("작업 취소"), "Ctrl+.", "common"},
            {"file.print", QObject::tr("인쇄"), "Ctrl+P", "common"},
            {"file.printPreview", QObject::tr("인쇄 미리보기"), "", "common"},
            {"capture.screen", QObject::tr("화면 캡처"), "Alt+Shift+S", "common"},
            {"tab.close", QObject::tr("현재 탭 닫기"), "Ctrl+W", "common"},
            {"tab.next", QObject::tr("다음 탭"), "Ctrl+Tab", "common"},
            {"tab.previous", QObject::tr("이전 탭"), "Ctrl+Shift+Tab", "common"},
            {"app.settings", QObject::tr("설정"), "Ctrl+I", "common"},
            {"view.displayOptions", QObject::tr("표시 옵션"), "Ctrl+Shift+M", "common"},
            {"edit.undo", QObject::tr("실행 취소"), "Ctrl+Z", "edit"},
            {"edit.redo", QObject::tr("다시 실행"), "Ctrl+Y", "edit"},
            {"edit.cut", QObject::tr("잘라내기"), "Ctrl+X", "edit"},
            {"edit.copy", QObject::tr("복사"), "Ctrl+C", "edit"},
            {"edit.paste", QObject::tr("붙여넣기"), "Ctrl+V", "edit"},
            {"edit.selectAll", QObject::tr("전체 선택"), "Ctrl+A", "edit"},
            {"text.find", QObject::tr("찾기"), "Ctrl+F", "text"},
            {"document.findInFiles", QObject::tr("파일에서 찾기"), "Ctrl+Shift+F", "common"},
            {"document.replaceInFiles", QObject::tr("파일에서 바꾸기"), "Ctrl+Shift+H", "common"},
            {"text.replace", QObject::tr("바꾸기"), "Ctrl+H", "text"},
            {"text.findNext", QObject::tr("다음 찾기"), "F3", "text"},
            {"text.findPrev", QObject::tr("이전 찾기"), "Shift+F3", "text"},
            {"text.goToLine", QObject::tr("줄 이동"), "Ctrl+G", "text"},
            {"text.wordWrap", QObject::tr("자동 줄바꿈 전환"), "Alt+Z", "text"},
            {"text.minimap", QObject::tr("미니맵 표시 전환"), "Ctrl+Alt+M", "text"},
            {"text.foldAll", QObject::tr("전체 접기"), "Ctrl+Shift+-", "text"},
            {"text.unfoldAll", QObject::tr("전체 펼치기"), "Ctrl+Shift++", "text"},
            {"epub.next", QObject::tr("EPUB 다음 화면"), "PgDown", "epub"},
            {"epub.previous", QObject::tr("EPUB 이전 화면"), "PgUp", "epub"},
            {"epub.next-chapter", QObject::tr("EPUB 다음 장"), "Ctrl+Alt+Right", "epub"},
            {"epub.previous-chapter", QObject::tr("EPUB 이전 장"), "Ctrl+Alt+Left", "epub"},
            {"epub.back", QObject::tr("EPUB 이전 위치"), "Alt+Left", "epub"},
            {"epub.focus-toc", QObject::tr("EPUB 목차"), "Ctrl+Alt+T", "epub"},
            {"epub.focus-settings", QObject::tr("EPUB 읽기 설정"), "Ctrl+Shift+E", "epub"},
            {"epub.focus-search", QObject::tr("EPUB 검색"), "Ctrl+F", "epub"},
            {"epub.bookmark-add", QObject::tr("EPUB 책갈피 추가"), "Ctrl+D", "epub"},
            {"epub.cycle-focus", QObject::tr("EPUB 영역 포커스 이동"), "F6", "epub"},
            {"epub.fullscreen", QObject::tr("EPUB 전체 화면"), "F11", "epub"},
            {"epubEdit.apply", QObject::tr("변경 반영"), "Ctrl+Enter", "epub-edit"},
            {"epubEdit.validate", QObject::tr("기본 검사"), "F7", "epub-edit"},
            {"epubEdit.epubcheck", QObject::tr("EPUBCheck 검사"), "Shift+F7", "epub-edit"},
            {"epubEdit.filter", QObject::tr("리소스 필터"), "Ctrl+E", "epub-edit"},
            {"epubEdit.previewRefresh", QObject::tr("미리보기 갱신"), "F5", "epub-edit"},
            {"epubEdit.scrollSync", QObject::tr("스크롤 동기화"), "", "epub-edit"},
            {"epubEdit.closeTab", QObject::tr("편집기 탭 닫기"), "Ctrl+F4", "epub-edit"},
            {"epubEdit.toc", QObject::tr("목차 편집…"), "", "epub-edit"},
            {"epubEdit.metadata", QObject::tr("서지정보 편집…"), "", "epub-edit"},
            {"epubEdit.toggleTree", QObject::tr("책 구조 패널 표시"), "", "epub-edit"},
            {"epubEdit.togglePreview", QObject::tr("미리보기 패널 표시"), "", "epub-edit"},
            {"epubEdit.toggleProblems", QObject::tr("문제 패널 표시"), "", "epub-edit"},
            {"epubEdit.orientation", QObject::tr("좌우 · 상하 배치 전환"), "", "epub-edit"},
            {"epubEdit.recover", QObject::tr("초안 복원"), "", "epub-edit"},
            {"epubTree.rename", QObject::tr("이름·경로 변경…"), "F2", "epub-edit.tree"},
            {"epubTree.delete", QObject::tr("삭제…"), "Del", "epub-edit.tree"},
            {"epubTree.moveUp", QObject::tr("읽기 순서 위로"), "Alt+Up", "epub-edit.tree"},
            {"epubTree.moveDown", QObject::tr("읽기 순서 아래로"), "Alt+Down", "epub-edit.tree"},
            {"epubTree.copyPath", QObject::tr("경로 복사"), "Ctrl+Shift+C", "epub-edit.tree"},
            {"epubSource.quickFix", QObject::tr("빠른 수정"), "Alt+Enter", "epub-edit.source"},
            {"epubSource.openLink", QObject::tr("링크된 리소스 열기"), "F12", "epub-edit.source"},
            {"epubSource.locate", QObject::tr("미리보기에서 이 위치 보기"), "", "epub-edit.source"},
            {"epubSource.wrapEm", QObject::tr("태그로 감싸기 · em"), "", "epub-edit.source"},
            {"epubSource.wrapStrong", QObject::tr("태그로 감싸기 · strong"), "", "epub-edit.source"},
            {"epubSource.wrapSpan", QObject::tr("태그로 감싸기 · span"), "", "epub-edit.source"},
            {"epubSource.wrapLink", QObject::tr("태그로 감싸기 · a"), "", "epub-edit.source"},
            {"epubSource.wrapRuby", QObject::tr("태그로 감싸기 · ruby"), "", "epub-edit.source"},
            {"image.fullscreen", QObject::tr("전체 화면 전환"), "F11", "image"},
            {"image.animation", QObject::tr("재생/정지"), "Alt+Pause", "image"},
            {"image.previousSpace", QObject::tr("이전 쪽 (Shift+Space)"), "Shift+Space", "image.comic"},
            {"image.next", QObject::tr("다음 이미지 · 다음 쪽"), "Space", "image"},
            {"image.previous", QObject::tr("이전 이미지 · 이전 쪽"), "Backspace", "image"},
            {"image.scrollDown", QObject::tr("아래로 스크롤"), "Down", "image.comic"},
            {"image.scrollUp", QObject::tr("위로 스크롤"), "Up", "image.comic"},
            {"image.pageNext", QObject::tr("다음 화면"), "PgDown", "image.comic"},
            {"image.pagePrevious", QObject::tr("이전 화면"), "PgUp", "image.comic"},
            {"image.pageNextSingle", QObject::tr("한 쪽 앞으로"), "Shift+PgDown", "image.comic"},
            {"image.pagePreviousSingle", QObject::tr("한 쪽 뒤로"), "Shift+PgUp", "image.comic"},
            {"image.first", QObject::tr("첫 쪽"), "Home", "image.comic"},
            {"image.last", QObject::tr("마지막 쪽"), "End", "image.comic"},
            {"image.volumeNext", QObject::tr("다음 권"), "]", "image.comic"},
            {"image.volumePrevious", QObject::tr("이전 권"), "[", "image.comic"},
            {"image.right", QObject::tr("오른쪽 쪽"), "Right", "image.comic"},
            {"image.left", QObject::tr("왼쪽 쪽"), "Left", "image.comic"},
            {"pdf.pageNext", QObject::tr("다음 쪽"), "PgDown", "pdf"},
            {"pdf.pagePrevious", QObject::tr("이전 쪽"), "PgUp", "pdf"},
            {"pdf.first", QObject::tr("첫 쪽"), "Home", "pdf"},
            {"pdf.last", QObject::tr("마지막 쪽"), "End", "pdf"},
            {"pdf.goToPage", QObject::tr("쪽으로 이동"), "Ctrl+G", "pdf"},
            {"pdf.find", QObject::tr("PDF 찾기"), "Ctrl+F", "pdf"},
            {"pdf.findNext", QObject::tr("다음 검색 결과"), "F3", "pdf"},
            {"pdf.findPrev", QObject::tr("이전 검색 결과"), "Shift+F3", "pdf"},
            {"image.goToPage", QObject::tr("쪽 · 이미지로 이동"), "Ctrl+G", "image.comic"},
            {"image.bookmarkSave", QObject::tr("책갈피 저장"), "Ctrl+D", "image.comic"},
            {"image.bookmarkOpen", QObject::tr("책갈피로 이동"), "", "image.comic"},
            // Zoom follows the active view (text, PDF, image, Markdown preview, EPUB reading text size).
            // Zoom keys belong to the views that zoom; in text the editor keeps its own zoom keys (reserved).
            {"pdf.zoomIn", QObject::tr("PDF 확대"), "Ctrl++", "pdf"},
            {"pdf.zoomOut", QObject::tr("PDF 축소"), "Ctrl+-", "pdf"},
            {"pdf.zoomReset", QObject::tr("PDF 원래 크기"), "Ctrl+0", "pdf"},
            {"image.zoomIn", QObject::tr("이미지 확대"), "Ctrl++", "image"},
            {"image.zoomOut", QObject::tr("이미지 축소"), "Ctrl+-", "image"},
            {"image.zoomReset", QObject::tr("이미지 원래 크기"), "Ctrl+0", "image"}
        };
    }

    inline const QHash<QString, QVariant>& ViewValues()
    {
        static const QHash<QString, QVariant> Values{
            {"common/closeViewerOnEscape", true}, {"theme/colorOverridesSchemaVersion", 1},
            {"imageView/navigatorPinned", false}, {"epubEditor/layout/preview", true},
            {"textView/fontFamily", "D2Coding ligature"}, {"textView/fontSize", 17},
            {"textView/lineSpacing", 1.2}, {"textView/fontRendering", 2},
            {"textView/showLineNumbers", true}, {"textView/highlightCurrentLine", true},
            {"textView/braceHighlight", true}, {"textView/showCodeFolding", true},
            {"textView/showWhitespace", false}, {"textView/showIndentationGuides", true},
            {"textView/indentGuideStyle", 1}, {"textView/changeHistoryMode", 1},
            {"textView/tabWidth", 4}, {"textView/autoIndent", true}, {"textView/useTabs", false},
            {"textView/wordWrapMode", 2}, {"textView/wordWrapLastMode", 2},
            {"textView/wrapVisualFlags", 1}, {"textView/wrapIndentMode", 1},
            {"textView/showRuler", true}, {"textView/rulerFontFamily", "D2Coding"},
            {"textView/rulerFontSize", 8}, {"textView/defaultLineEnding", "CRLF"},
            {"textView/findInFiles/openLocation", "tab"},
            {"textView/saveEncoding", "UTF-8"}, {"textView/saveBomMode", 1},
            {"textView/recoveryEnabled", true}, {"textView/hotExitEnabled", true}, {"textView/hotExitPolicyVersion", 1}, {"textView/restoreCleanDocuments", true},
            {"textView/largeFileMB", 2}, {"textView/externalChangeAction", 1},
            {"textView/externalChangeDetection", 0}, {"textView/externalChangePollSeconds", 3},
            {"textView/mask/enabled", true}, {"textView/mask/gamma", 1.2}, {"textView/mask/strength", 1.1},
            // Minimap: auto = document thumbnail, outline from largeLines lines or largeBytes bytes, scroll bar marks only
            // when the editor viewport is narrower than narrowWidth logical pixels.
            {"textView/minimap/enabled", true}, {"textView/minimap/kind", "auto"}, {"textView/minimap/side", "right"},
            {"textView/minimap/width", 104}, {"textView/minimap/scale", 1}, {"textView/minimap/render", "chars"},
            {"textView/minimap/slider", "always"}, {"textView/minimap/layers/search", true},
            {"textView/minimap/layers/selection", true}, {"textView/minimap/layers/changes", true},
            {"textView/minimap/layers/caret", true}, {"textView/minimap/layers/problems", true},
            {"textView/minimap/labels", true}, {"textView/minimap/peek", true}, {"textView/minimap/peekDelayMs", 300},
            {"textView/minimap/largeLines", 20000}, {"textView/minimap/largeBytes", qint64(16) * 1024 * 1024},
            {"textView/minimap/narrowWidth", 480},
            {"markdown/viewerMode", "preview"}, {"markdown/editorMode", "split"},
            {"markdown/viewerOutline", true}, {"markdown/editorOutline", true},
            {"markdown/outlineDepth", 6}, {"markdown/scrollSync", true},
            {"markdown/math", true}, {"markdown/inlineDollar", true}, {"markdown/mermaid", true},
            {"markdown/codeHighlight", true}, {"markdown/autoPreview", true},
            {"markdown/debounceMs", 250}, {"markdown/remoteImages", true},
            {"markdown/previewFonts/enabled", false}, {"markdown/previewFonts/mode", "document"},
            {"markdown/previewFonts/bodyFamily", QStringLiteral("맑은 고딕")},
            {"markdown/previewFonts/bodySizePt", 11}, {"markdown/previewFonts/codeFamily", "Consolas"},
            {"markdown/previewFonts/codeSizePt", 10}, {"markdown/previewFonts/lineHeight", 1.7},
            {"markdown/previewFonts/headings", true},
            // reStructuredText: 보기 · 미리보기 빌드 · 자동완성 · 언어 서버 · Python 환경.
            {"rst/viewerMode", "preview"}, {"rst/editorMode", "split"},
            {"rst/viewerOutline", false}, {"rst/editorOutline", true},
            {"rst/outlineTab", "auto"}, {"rst/outlineDepth", 3}, {"rst/scrollSync", true},
            {"rst/problemsPanel", "auto"},
            {"rst/autoPreview", true}, {"rst/debounceMs", 600},
            {"rst/applyUnsavedEdits", true}, {"rst/unsavedEditMaxReadMs", 2000},
            {"rst/stubDoxygenWhileTyping", true}, {"rst/skipUnchangedBuild", true},
            {"rst/unlimitedWidth", false}, {"rst/allowRemoteContent", true}, {"rst/domMorph", true},
            {"rst/virtualProjectTheme", ""},
            {"rst/previewFonts/enabled", false}, {"rst/previewFonts/mode", "document"},
            {"rst/previewFonts/bodyFamily", QStringLiteral("맑은 고딕")},
            {"rst/previewFonts/bodySizePt", 11}, {"rst/previewFonts/codeFamily", "Consolas"},
            {"rst/previewFonts/codeSizePt", 10}, {"rst/previewFonts/lineHeight", 1.7},
            {"rst/previewFonts/headings", true},
            {"rst/completion/autoTrigger", true}, {"rst/completion/imagePreview", true},
            {"rst/completion/termHover", true},
            {"rst/languageServer", true}, {"rst/maxLspProcesses", 3},
            {"rst/buildDirName", "_build/hypereditor"},
            // uv: ask (확인 후 내려받기) · auto (묻지 않고 내려받기) · never (직접 지정한 uv 만).
            {"rst/python/uvAcquire", "ask"}, {"rst/python/useExternalUv", false},
            {"rst/python/externalUvPath", ""},
            {"rst/python/installOptionalThemes", true}, {"rst/python/installOptionalExtensions", true},
            {"rst/build/lastBuilder", "html"}, {"rst/build/openWhenDone", true},
            {"epubEditor/javaPath", ""}, {"epubEditor/epubcheckJar", ""},
            {"epubEditor/requireEpubCheck", false},
            {"imageView/comic/coverAlone", false}, {"imageView/fullscreenRemember", true},
            {"imageView/fullscreenPinTop", false}, {"imageView/fullscreenPinBottom", false},
            {"imageView/fullscreenHideCursor", true}, {"imageView/normalMaximized", false},
            {"imageView/fullscreen", false},
            {"textView/detectIndent", true},
            {"recent/enabled", true}, {"recent/menuCount", 5},
            // Ctrl+N: ask (종류 선택 팝업) · text · markdown · rst. The popup starts on the last kind made.
            {"common/newDocumentAction", "ask"}, {"common/newDocumentLastKind", "text"},
            {"common/newImagePreset", "fhd"}, {"common/newImageWidth", 1920}, {"common/newImageHeight", 1080},
            {"common/newImageBackground", "white"},
            // Untitled Save As starts here (else Documents); a new EPUB remembers its folder and author.
            {"common/lastSaveFolder", ""}, {"common/newEpubFolder", ""}, {"common/newEpubCreator", ""},
            {"common/newSphinxFolder", ""}, {"common/newSphinxAuthor", ""},
            {"pdfView/annotationToolbar", false}
        };
        return Values;
    }

    inline QVariant ViewValue(const QString& Key)
    {
        return ViewValues().value(Key);
    }

    inline QJsonObject Workspace()
    {
        return {{"schemaVersion", 1}, {"themeMode", "product"}, {"mainMenuMode", "recommended"},
            {"mainToolbarMode", "recommended"}, {"statusBarMode", "recommended"}, {"toolbarLayout", "single"}, {"viewOverrides", QJsonObject{}}};
    }

    // Settings controls use full JSON paths; runtime AppSettings uses relative view paths.
    inline QVariant SettingValue(const QString& Key)
    {
        const QString ViewsPrefix = "documentViews/", WorkspacePrefix = "appearance/documentWorkspace/";
        if (Key.startsWith(ViewsPrefix)) return ViewValue(Key.mid(ViewsPrefix.size()));
        if (Key.startsWith(WorkspacePrefix)) {
            const auto Value = Workspace().value(Key.mid(WorkspacePrefix.size()));
            if (!Value.isUndefined()) return Value.toVariant();
        }
        return {};
    }
    inline constexpr QSize WindowSize{1000, 900};
    inline constexpr auto ViewerConfigFileName = "HyperViewer.json";
    inline constexpr auto ConfigFileName = "HyperEditor.json";
    inline constexpr auto LegacySettingsFileName = "settings.json";
    inline constexpr auto LegacyWorkspaceFileName = "workspace.json";
    inline constexpr auto LegacyViewsFileName = "document-views.ini";
    inline constexpr auto EpubReadingFileName = "epub-reading.json";
    inline constexpr auto ImageReadingFileName = "HyperViewer-reading.json";
    inline QString ConfigDirectory(const QString& ExecutableDirectory) { return ExecutableDirectory; }
    inline QString SettingsFile(const QString& Directory) { return QDir(Directory).filePath(ConfigFileName); }
    inline QString EpubReadingFile(const QString& Directory) { return QDir(Directory).filePath(EpubReadingFileName); }
    inline QString ImageReadingFile(const QString& Directory) { return QDir(Directory).filePath(ImageReadingFileName); }
    inline QString EpubProfilesFile(const QString& Directory) { return QDir(Directory).filePath("epub-profiles.json"); }
    inline QString EpubStateDirectory(const QString& Directory) { return QDir(Directory).filePath("epub-state"); }
    inline QString RecoveryDirectory(bool Viewer)
    { return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + (Viewer ? "/recovery/viewer" : "/recovery/editor"); }
    inline QString HwpCacheDirectory()
    { return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/rhwp"; }
    inline QString TextBackupDirectory()
    {
        auto Base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (Base.isEmpty()) Base = QDir::tempPath() + "/MultiViewer";
        return QDir(Base).filePath("TextHotExit");
    }
    inline QJsonObject EpubReading()
    {
        return {{"theme", "system"}, {"font", "Malgun Gothic"}, {"size", 19}, {"weight", 400},
            {"letter", 0}, {"word", 0}, {"line", 1.8}, {"before", 0}, {"after", 0.8}, {"indent", 0},
            {"background", "theme"}, {"backgroundColor", ""}, {"textColor", ""}, {"backgroundAsset", ""},
            {"customCss", ""}, {"cssEnabled", false}, {"cssForce", true}, {"resume", true},
            {"privateReading", false}, {"historyEpoch", 0}, {"backgroundFit", "cover"}, {"backgroundOpacity", 1},
            {"layout", "scroll"}, {"columns", 0}, {"align", "left"}, {"margin", 28}, {"width", 760}, {"override", true}};
    }
    inline QJsonObject ImageReading()
    {
        return {{"rtl", false}, {"split", false}, {"part", 0}, {"gap", 0}, {"fit", 1},
            {"brightness", 0}, {"contrast", 1}, {"gamma", 1}, {"sharpness", 0}, {"grayscale", false},
            {"invert", false}, {"automaticNext", true}, {"zoom", 1}, {"filter", "auto"}, {"mode", 0}};
    }
    inline QJsonValue ViewDefault(const QString& Key) { return QJsonValue::fromVariant(ViewValue(Key)); }
    inline int TextWrapMode(const QJsonObject& Text, bool ExistingSection)
    {
        if (Text.contains("wordWrapMode")) return Text["wordWrapMode"].toInt(ViewValue("textView/wordWrapMode").toInt());
        if (Text.contains("wordWrap")) return Text["wordWrap"].toBool() ? 1 : 0;
        return ExistingSection ? 0 : ViewValue("textView/wordWrapMode").toInt();
    }
    inline QVariant WorkspaceValue(const QString& Key)
    {
        if (Key == "viewOverrideMode") return "inherit";
        return Workspace().value(Key).toVariant();
    }

    inline constexpr int ConfigSchemaVersion = 2;
    inline constexpr int ThemeOverridesSchemaVersion = 4;
    inline constexpr int AnnotationDefaultColor = 0xffde59;
    inline constexpr auto AnnotationColor = "#ffde59";
    inline constexpr auto ProductThemeFallback = "system";
    inline constexpr auto ImportedThemeMode = "light";
    inline QJsonObject TextRuntime()
    { return {{"useUtf8", true}, {"readOnly", false}, {"lineNumberMarginDigits", 5}}; }
    inline QJsonObject DefaultDocument() { return {{"schemaVersion", ConfigSchemaVersion}}; }

    template<typename Token, typename Palette>
    inline auto Colors(const Palette& T, bool WantDark)
    {
        using Color = std::remove_cvref_t<decltype(T[Token::Surface])>;
    const auto Alpha = [](Color Value, int AlphaValue) { Value.setAlpha(AlphaValue); return Value; };
    QHash<QString, Color> c;
    c.insert(QStringLiteral("common.background"), T[Token::Win]);
    c.insert(QStringLiteral("common.surface"), T[Token::Surface]);
    c.insert(QStringLiteral("common.surfaceAlt"), T[Token::Alt]);
    c.insert(QStringLiteral("common.foreground"), T[Token::Fg]);
    c.insert(QStringLiteral("common.foregroundMuted"), T[Token::Fg2]);
    c.insert(QStringLiteral("common.accent"), T[Token::Accent]);
    c.insert(QStringLiteral("common.border"), T[Token::Line]);
    c.insert(QStringLiteral("common.toolbar"), T[Token::Win]);
    c.insert(QStringLiteral("common.tabActive"), T[Token::Surface]);
    c.insert(QStringLiteral("common.tabInactive"), T[Token::Alt]);
    c.insert(QStringLiteral("common.canvas"), T[Token::Alt]);

    c.insert(QStringLiteral("pdf.canvas"), T[Token::Alt]);
    c.insert(QStringLiteral("pdf.pageGap"), T[Token::Grid]);
    c.insert(QStringLiteral("pdf.searchHighlight"), Alpha(T[Token::Warn], 0x5A));
    c.insert(QStringLiteral("pdf.searchCurrent"), Alpha(T[Token::Accent], 0x6E));
    c.insert(QStringLiteral("pdf.textSelection"), Alpha(T[Token::Accent], 0x46));
    c.insert(QStringLiteral("pdf.imageSelection"), Alpha(T[Token::Ok], 0xE6));
    c.insert(QStringLiteral("pdf.annotation"), Alpha(T[Token::Warn], 0x80));

    c.insert(QStringLiteral("image.canvas"), T[Token::Alt]);
    c.insert(QStringLiteral("image.draw"), T[Token::Danger]);
    c.insert(QStringLiteral("image.selection"), T[Token::Accent]);
    c.insert(QStringLiteral("image.compositeOutline"), T[Token::Surface]);
    c.insert(QStringLiteral("image.handleFill"), T[Token::Surface]);
    c.insert(QStringLiteral("image.handleBorder"), T[Token::Fg]);
    c.insert(QStringLiteral("image.accept"), T[Token::Ok]);
    c.insert(QStringLiteral("image.cancel"), T[Token::Danger]);

    c.insert(QStringLiteral("markdown.background"), T[Token::Surface]);
    c.insert(QStringLiteral("markdown.foreground"), T[Token::Fg]);
    c.insert(QStringLiteral("markdown.link"), T[Token::AccentFg]);
    c.insert(QStringLiteral("markdown.heading"), T[Token::Fg]);
    c.insert(QStringLiteral("markdown.border"), T[Token::Line]);
    c.insert(QStringLiteral("markdown.codeBackground"), T[Token::Alt]);
    c.insert(QStringLiteral("markdown.inlineCode"), T[Token::Danger]);
    c.insert(QStringLiteral("markdown.blockquote"), T[Token::Fg2]);
    c.insert(QStringLiteral("markdown.taskChecked"), T[Token::Ok]);

    c.insert(QStringLiteral("text.background"), T[Token::Surface]);
    c.insert(QStringLiteral("text.foreground"), T[Token::Fg]);
    // Current line, shared by HyperEditor and HyperViewer: no fill, 1 px rules above and below the text
    // area, and an accent number cell. The rules are painted over glyphs, so they stay translucent; the
    // cell tint is laid over text.marginBackground. Inactive values apply while the editor lacks focus.
    c.insert(QStringLiteral("text.currentLineFrame"), Alpha(T[Token::Focus], WantDark ? 0x75 : 0x66));
    c.insert(QStringLiteral("text.currentLineGutter"), Alpha(T[Token::Accent], WantDark ? 0x38 : 0x1F));
    c.insert(QStringLiteral("text.currentLineNumber"), T[Token::AccentFg]);
    c.insert(QStringLiteral("text.currentLineFrameInactive"), T[Token::Line]);
    c.insert(QStringLiteral("text.currentLineGutterInactive"), Alpha(T[Token::Fg], WantDark ? 0x0F : 0x0D));
    c.insert(QStringLiteral("text.currentLineNumberInactive"), T[Token::Fg2]);
    c.insert(QStringLiteral("text.selection"), T[Token::AccentSoft]);
    c.insert(QStringLiteral("text.selectionForeground"), T[Token::Fg]);
    c.insert(QStringLiteral("text.caret"), T[Token::Fg]);
    c.insert(QStringLiteral("text.marginBackground"), T[Token::Win]);
    c.insert(QStringLiteral("text.marginForeground"), T[Token::Fg3]);
    c.insert(QStringLiteral("text.indentGuide"), T[Token::Line]);
    c.insert(QStringLiteral("text.foldMarker"), T[Token::Fg3]);
    c.insert(QStringLiteral("text.braceMatch"), T[Token::Accent]);
    c.insert(QStringLiteral("text.braceMismatch"), T[Token::Danger]);
    // Minimap and scroll bar marks. Change history uses Scintilla's margin marker colours so both read alike.
    c.insert(QStringLiteral("text.minimap.background"), T[Token::Surface]);
    c.insert(QStringLiteral("text.minimap.separator"), T[Token::Grid]);
    c.insert(QStringLiteral("text.minimap.slider"), Alpha(T[Token::Fg], 0x1A));
    c.insert(QStringLiteral("text.minimap.sliderHover"), Alpha(T[Token::Fg], 0x29));
    c.insert(QStringLiteral("text.minimap.sliderActive"), Alpha(T[Token::Fg], 0x38));
    c.insert(QStringLiteral("text.minimap.bracket"), T[Token::Fg]);
    c.insert(QStringLiteral("text.minimap.search"), WantDark ? Color(QStringLiteral("#F2C94C")) : Color(QStringLiteral("#D99A0B")));
    c.insert(QStringLiteral("text.minimap.selection"), Alpha(T[Token::Accent], WantDark ? 0x6B : 0x4D));
    c.insert(QStringLiteral("text.minimap.caret"), T[Token::AccentFg]);
    c.insert(QStringLiteral("text.minimap.error"), T[Token::Danger]);
    c.insert(QStringLiteral("text.minimap.warning"), T[Token::Warn]);
    c.insert(QStringLiteral("text.minimap.changeModified"), Color(QStringLiteral("#FF8000")));
    c.insert(QStringLiteral("text.minimap.changeSaved"), Color(QStringLiteral("#00A000")));
    c.insert(QStringLiteral("text.minimap.outline"), Alpha(T[Token::Fg], WantDark ? 0x6B : 0x5C));
    c.insert(QStringLiteral("text.minimap.label"), T[Token::Fg]);
    c.insert(QStringLiteral("text.minimap.labelMinor"), T[Token::Fg2]);

    // Keep syntax text at 4.5:1 against the Paper Fluent body surface. The current line adds no fill, so
    // the body is the only surface under syntax text. Explicit user syntax overrides are applied separately.
    if (WantDark) {
        c.insert(QStringLiteral("text.lexer.comment"), Color(QStringLiteral("#93907D")));
        c.insert(QStringLiteral("text.lexer.number"), Color(QStringLiteral("#AE81FF")));
        c.insert(QStringLiteral("text.lexer.keyword"), Color(QStringLiteral("#FF4388")));
        c.insert(QStringLiteral("text.lexer.type"), Color(QStringLiteral("#66D9EF")));
        c.insert(QStringLiteral("text.lexer.string"), Color(QStringLiteral("#E6DB74")));
        c.insert(QStringLiteral("text.lexer.preprocessor"), Color(QStringLiteral("#A6E22E")));
        c.insert(QStringLiteral("text.lexer.operator"), Color(QStringLiteral("#FF4388")));
        c.insert(QStringLiteral("text.lexer.identifier"), Color(QStringLiteral("#F8F8F2")));
        c.insert(QStringLiteral("text.lexer.function"), Color(QStringLiteral("#A6E22E")));
        c.insert(QStringLiteral("text.lexer.variable"), Color(QStringLiteral("#FD971F")));
    } else {
        c.insert(QStringLiteral("text.lexer.comment"), Color(QStringLiteral("#716D5A")));
        c.insert(QStringLiteral("text.lexer.number"), Color(QStringLiteral("#7444F5")));
        c.insert(QStringLiteral("text.lexer.keyword"), Color(QStringLiteral("#D0005F")));
        c.insert(QStringLiteral("text.lexer.type"), Color(QStringLiteral("#00749F")));
        c.insert(QStringLiteral("text.lexer.string"), Color(QStringLiteral("#856800")));
        c.insert(QStringLiteral("text.lexer.preprocessor"), Color(QStringLiteral("#437900")));
        c.insert(QStringLiteral("text.lexer.operator"), Color(QStringLiteral("#D0005F")));
        c.insert(QStringLiteral("text.lexer.identifier"), Color(QStringLiteral("#272822")));
        c.insert(QStringLiteral("text.lexer.function"), Color(QStringLiteral("#3B7D00")));
        c.insert(QStringLiteral("text.lexer.variable"), Color(QStringLiteral("#AB5500")));
    }

    const QStringList lexerKeys = {
        QStringLiteral("cpp"), QStringLiteral("python"), QStringLiteral("json"),
        QStringLiteral("xml"), QStringLiteral("hypertext"), QStringLiteral("css"),
        QStringLiteral("bash"), QStringLiteral("sql")
    };
    const QStringList tokenKeys = {
        QStringLiteral("comment"), QStringLiteral("number"), QStringLiteral("keyword"),
        QStringLiteral("type"), QStringLiteral("string"), QStringLiteral("preprocessor"),
        QStringLiteral("operator"), QStringLiteral("identifier"), QStringLiteral("function"),
        QStringLiteral("variable")
    };
    for (const QString& lexerKey : lexerKeys) {
        for (const QString& tokenKey : tokenKeys) {
            c.insert(QStringLiteral("text.lexer.%1.%2").arg(lexerKey, tokenKey),
                     c.value(QStringLiteral("text.lexer.%1").arg(tokenKey)));
        }
    }
    // reStructuredText has its own style set (container lexer). A directive or role is painted in one of three
    // states: known, not in the project's list, or not known yet (the language server has not answered).
    const QList<std::pair<const char*, const char*>> RstColors = WantDark
        ? QList<std::pair<const char*, const char*>>{
            {"title", "#66D9EF"}, {"transition", "#93907D"}, {"comment", "#93907D"}, {"explicitMarkup", "#9A9581"},
            {"directiveValid", "#A6E22E"}, {"directiveInvalid", "#FF4388"}, {"directiveUnknown", "#A8A493"},
            {"roleValid", "#FD971F"}, {"roleInvalid", "#FF4388"}, {"roleUnknown", "#A8A493"},
            {"literal", "#E6DB74"}, {"inlineLiteral", "#E6DB74"}, {"emphasis", "#D8CFA0"}, {"strong", "#FD971F"},
            {"interpreted", "#66D9EF"}, {"hyperlink", "#66D9EF"}, {"substitution", "#AE81FF"}, {"fieldName", "#FF4388"}}
        : QList<std::pair<const char*, const char*>>{
            {"title", "#00749F"}, {"transition", "#716D5A"}, {"comment", "#716D5A"}, {"explicitMarkup", "#716D5A"},
            {"directiveValid", "#3B7D00"}, {"directiveInvalid", "#D0005F"}, {"directiveUnknown", "#77735F"},
            {"roleValid", "#AB5500"}, {"roleInvalid", "#D0005F"}, {"roleUnknown", "#77735F"},
            {"literal", "#856800"}, {"inlineLiteral", "#856800"}, {"emphasis", "#6B5A00"}, {"strong", "#AB5500"},
            {"interpreted", "#00749F"}, {"hyperlink", "#0057B8"}, {"substitution", "#7444F5"}, {"fieldName", "#D0005F"}};
    for (const auto& [Token, Hex] : RstColors)
        c.insert(QStringLiteral("text.lexer.rst.%1").arg(QLatin1String(Token)), Color(QString::fromLatin1(Hex)));
    // Squiggles under language diagnostics (reStructuredText).
    c.insert(QStringLiteral("text.diagnostic.error"), T[Token::Danger]);
    c.insert(QStringLiteral("text.diagnostic.warning"), T[Token::Warn]);
    c.insert(QStringLiteral("text.diagnostic.info"), T[Token::AccentFg]);
    return c;
    }

    inline QJsonObject SettingsPreviewFont() { return {{"family", "Consolas"}, {"pointSize", 11}}; }
    // Python runtime of the reStructuredText preview (uv, CPython, Sphinx, Esbonio). It is hundreds of MB and is
    // not part of the product folder, which installation and updates manage; HyperEditor and HyperViewer share it.
    inline QString RstRuntimeDirectory()
    { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/HyperCommander/rst-runtime"; }
    inline QString EpubWorkspaceFile(const QString& Directory) { return QDir(Directory).filePath("HyperEditor.epub-workspace.json"); }
    inline QString EpubStandaloneSettingsFile()
    { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/HyperCommander/EpubPoc/reading-profiles.json"; }
    inline QString BookStateFile(const QString& Directory, const QString& Book) { return QDir(Directory).filePath(Book + ".json"); }

    inline QString Shortcut(const QString& Id)
    {
        for (const auto& Command : Commands()) if (Command.id_ == Id) return Command.key_;
        return {};
    }
    inline QKeyCombination AuxiliaryShortcut(const QString& Id)
    {
        static const QHash<QString, QKeyCombination> Keys{{"image.focusNavigator", Qt::Key_F6}, {"viewer.close", Qt::Key_Escape}, {"capture.cancel", Qt::Key_Escape}, {"reading.panel", Qt::CTRL | Qt::SHIFT | Qt::Key_R}};
        return Keys.value(Id, QKeyCombination::fromCombined(0));
    }

    inline QMap<QString, QString> ShortcutAliases() { return {{"edit.redo", "Ctrl+Shift+Z"}, {"edit.copy", "Ctrl+Insert"}, {"edit.cut", "Shift+Delete"}, {"edit.paste", "Shift+Insert"}}; }

    inline QMap<QString, QString> CommandAliases() { return {{"file.open", "file.openWorkspace"}, {"file.close", "tab.close"},
            {"view.capture", "capture.screen"}, {"text.new", "file.new"}, {"epubSource.findAll", "document.findInFiles"}}; }

    inline QStringList ReservedShortcuts() { return {"Ctrl+Space", "Ctrl+Shift+Space", "Ctrl+L", "Ctrl+Shift+L", "Ctrl+T", "Ctrl+Shift+T",
            "Ctrl+D", "Ctrl+U", "Ctrl+Shift+U", "Ctrl+[", "Ctrl+Shift+[", "Ctrl+]", "Ctrl+Shift+]",
            "Ctrl+/", "Ctrl+Shift+/", "Ctrl+\\", "Ctrl+Shift+\\", "Ctrl++", "Ctrl+-", "Ctrl+0"}; }

    // Scintilla protocol IDs; the backend checks these against the installed API.
    inline constexpr int NativeKeyHome = 304, NativeKeyEnd = 305, NativeShift = 1 << 16;
    inline constexpr int NativeHomeDisplay = 2345, NativeHomeDisplayExtend = 2346;
    inline constexpr int NativeLineEndDisplay = 2347, NativeLineEndDisplayExtend = 2348;
    inline constexpr std::array<std::pair<int, int>, 4> NativeTextBindings{{
        {NativeKeyHome, NativeHomeDisplay}, {NativeKeyHome | NativeShift, NativeHomeDisplayExtend},
        {NativeKeyEnd, NativeLineEndDisplay}, {NativeKeyEnd | NativeShift, NativeLineEndDisplayExtend}}};

    inline QList<Qt::Key> InputFocusKeys() { return {Qt::Key_Tab, Qt::Key_Backtab, Qt::Key_Return, Qt::Key_Enter, Qt::Key_Escape}; }

    inline QList<Qt::Key> EditingKeys() { return {Qt::Key_Delete, Qt::Key_Backspace, Qt::Key_Insert, Qt::Key_Left, Qt::Key_Right, Qt::Key_Up,
            Qt::Key_Down, Qt::Key_Home, Qt::Key_End, Qt::Key_PageUp, Qt::Key_PageDown}; }

    inline QString EpubDraftDirectory(const QString& Directory) { return QDir(Directory).absoluteFilePath("epub-drafts"); }

    inline constexpr int NativeControl = 2 << 16, NativeKeyInsert = 309;
    inline constexpr std::array<int, 5> NativeBrokerClipboardKeys{{
        'C' | NativeControl, 'X' | NativeControl, 'V' | NativeControl,
        NativeKeyInsert | NativeShift, NativeKeyInsert | NativeControl}};

}
