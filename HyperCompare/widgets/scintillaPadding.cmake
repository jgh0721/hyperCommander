macro(hc_compare_padding_replace before after)
    string(FIND "${_text}" "${before}" _offset)
    if(_offset EQUAL -1)
        message(FATAL_ERROR "Scintilla compare padding patch no longer matches ${_name}")
    endif()
    string(REPLACE "${before}" "${after}" _text "${_text}")
endmacro()
# Applied after ScintillaWrapEnd to the generated engine copy. In a combined
# product build the engine is shared; padding stays inactive unless requested.
# Upstream files are untouched. Fail closed when
# upstream context changes. Prefix rows have no document positions or undo data.
foreach(_name ContractionState.h ContractionState.cxx Editor.cxx EditView.cxx)
    file(READ "${MV_SCINTILLA_CORE_ROOT}/${_name}" _text)
    string(REPLACE "\r\n" "\n" _text "${_text}")
    if(_name STREQUAL "ContractionState.h")
        hc_compare_padding_replace([=[	virtual void Clear()=0;]=] [=[	virtual void Clear()=0;
    virtual void SetLeadingRows(Sci::Line rows) noexcept=0;
    virtual Sci::Line LeadingRows() const noexcept=0;
    virtual bool SetTrailingRows(Sci::Line line, int rows)=0;
    virtual void ClearPaddingRows()=0;]=])
    elseif(_name STREQUAL "ContractionState.cxx")
        hc_compare_padding_replace([=[	LINE linesInDocument;]=] [=[	LINE linesInDocument;
    Sci::Line leadingRows = 0;
    std::map<Sci::Line, int> trailingRows;
    int TrailingRows(Sci::Line line) const noexcept {
        const auto found = trailingRows.find(line);
        return found == trailingRows.end() ? 0 : found->second;
    }]=])
        hc_compare_padding_replace([=[#include <memory>]=] [=[#include <memory>
#include <map>]=])
        hc_compare_padding_replace([=[	void Clear() noexcept override;]=] [=[	void Clear() noexcept override;
    void SetLeadingRows(Sci::Line rows) noexcept override { leadingRows = rows; }
    Sci::Line LeadingRows() const noexcept override { return leadingRows; }
    bool SetTrailingRows(Sci::Line line, int rows) override {
        const int baseHeight = GetHeight(line) - TrailingRows(line);
        if (rows) trailingRows[line] = rows; else trailingRows.erase(line);
        return SetHeight(line, baseHeight);
    }
    void ClearPaddingRows() override {
        while (!trailingRows.empty()) SetTrailingRows(trailingRows.begin()->first, 0);
        leadingRows = 0;
    }]=])
        hc_compare_padding_replace([=[const Sci::Line lineDisplay = DisplayFromDoc(lineDoc);]=] [=[const Sci::Line lineDisplay = DisplayFromDoc(lineDoc) - leadingRows;]=])
        hc_compare_padding_replace([=[	linesInDocument = 1;]=] [=[	linesInDocument = 1;
    leadingRows = 0;
    trailingRows.clear();]=])
        hc_compare_padding_replace([=[bool ContractionState<LINE>::SetHeight(Sci::Line lineDoc, int height) {
]=] [=[bool ContractionState<LINE>::SetHeight(Sci::Line lineDoc, int height) {
    height += TrailingRows(lineDoc);
]=])
        hc_compare_padding_replace([=[Sci::Line ContractionState<LINE>::LinesDisplayed() const noexcept {
	if (OneToOne()) {
		return linesInDocument;
	}
	return displayLines->PositionFromPartition(line_cast(LinesInDoc()));
}]=] [=[Sci::Line ContractionState<LINE>::LinesDisplayed() const noexcept {
    return leadingRows + (OneToOne() ? linesInDocument : displayLines->PositionFromPartition(line_cast(LinesInDoc())));
}]=])
        hc_compare_padding_replace([=[return (lineDoc <= linesInDocument) ? lineDoc : linesInDocument;]=] [=[return leadingRows + ((lineDoc <= linesInDocument) ? lineDoc : linesInDocument);]=])
        hc_compare_padding_replace([=[return displayLines->PositionFromPartition(line_cast(lineDoc));]=] [=[return leadingRows + displayLines->PositionFromPartition(line_cast(lineDoc));]=])
        hc_compare_padding_replace([=[Sci::Line ContractionState<LINE>::DocFromDisplay(Sci::Line lineDisplay) const noexcept {
	if (OneToOne())]=] [=[Sci::Line ContractionState<LINE>::DocFromDisplay(Sci::Line lineDisplay) const noexcept {
    if (leadingRows) lineDisplay = std::max<Sci::Line>(0, lineDisplay - leadingRows);
	if (OneToOne())]=])
        hc_compare_padding_replace([=[if (lineDisplay > LinesDisplayed()) {
		return displayLines->PartitionFromPosition(line_cast(LinesDisplayed()));]=] [=[if (lineDisplay > LinesDisplayed() - leadingRows) {
		return displayLines->PartitionFromPosition(line_cast(LinesDisplayed() - leadingRows));]=])
    elseif(_name STREQUAL "Editor.cxx")
        hc_compare_padding_replace([=[	case Message::AnnotationSetText:]=] [=[    case static_cast<Message>(50010): // HyperCompare SetLeadingRows
        if (wParam > 2000000 || (wParam && Wrapping())) return -1;
        pcs->SetLeadingRows(static_cast<Sci::Line>(wParam));
        SetTopLine(std::min(topLine, MaxScrollPos()));
        SetScrollBars(); Redraw(); return 0;
    case static_cast<Message>(50011):
        return pcs->LeadingRows();
    case static_cast<Message>(50012): // unwrapped compare panes; controller clears stale padding on edits
        if (Wrapping() || wParam >= static_cast<uptr_t>(pdoc->LinesTotal()) || lParam < 0 || lParam > 2000000) return -1;
        pcs->SetTrailingRows(static_cast<Sci::Line>(wParam), static_cast<int>(lParam));
        SetScrollBars(); Redraw(); return 0;
    case static_cast<Message>(50013):
        pcs->ClearPaddingRows();
        SetTopLine(std::min(topLine, MaxScrollPos()));
        SetScrollBars(); Redraw(); return 0;
	case Message::AnnotationSetText:]=])
    elseif(_name STREQUAL "EditView.cxx")
        hc_compare_padding_replace([=[	if (subLine >= ll->lines) {
		DrawAnnotation]=] [=[    if (subLine >= ll->lines + model.pdoc->AnnotationLines(line)) {
        if (FlagSet(phase, DrawPhase::back))
            surface->FillRectangleAligned(rcLine, Fill(vsDraw.styles[255].back));
        return;
    }
	if (subLine >= ll->lines) {
		DrawAnnotation]=])
        hc_compare_padding_replace([=[if (subLine < ll->lines) {]=] [=[if (subLine >= 0 && subLine < ll->lines) {]=])
        hc_compare_padding_replace([=[	const Sci::Position posLineStart = model.pdoc->LineStart(lineDoc);
	std::shared_ptr<LineLayout> ll = RetrieveLineLayout(lineDoc, model);]=] [=[	const Sci::Position posLineStart = model.pdoc->LineStart(lineDoc);
    if (visibleLine < model.pcs->DisplayFromDoc(lineDoc))
        return SelectionPosition(canReturnInvalid ? Sci::invalidPosition : 0);
	std::shared_ptr<LineLayout> ll = RetrieveLineLayout(lineDoc, model);]=])
        # Skip virtual prefix before any layout/caret code sees a negative subline.
        hc_compare_padding_replace([=[				const int subLine = static_cast<int>(lineVisible - lineStartSet);

				// Copy this line]=] [=[				const int subLine = static_cast<int>(lineVisible - lineStartSet);
                if (subLine < 0) {
                    if (FlagSet(phase, DrawPhase::back)) {
                        PRectangle gap = rcTextArea;
                        gap.left -= leftTextOverlap;
                        gap.top = static_cast<XYPOSITION>(yposScreen);
                        gap.bottom = gap.top + vsDraw.lineHeight;
                        surfaceWindow->FillRectangleAligned(gap, Fill(vsDraw.styles[255].back));
                    }
                    if (!bufferedDraw) ypos += vsDraw.lineHeight;
                    yposScreen += vsDraw.lineHeight;
                    ++lineVisible;
                    continue;
                }

				// Copy this line]=])
    endif()
    file(CONFIGURE OUTPUT "${MV_SCINTILLA_CORE_ROOT}/${_name}" CONTENT "${_text}" @ONLY NEWLINE_STYLE CRLF)
endforeach()
unset(_text)
