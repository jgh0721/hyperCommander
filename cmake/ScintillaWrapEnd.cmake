# Build a private core copy: a wrap boundary has one byte position but two
# screen positions. Keep the upstream checkout untouched and fail if any
# replacement stops matching when Scintilla is upgraded.
set(MV_SCINTILLA_CORE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/scintilla-core")
file(MAKE_DIRECTORY "${MV_SCINTILLA_CORE_ROOT}")
file(GLOB _coreFiles CONFIGURE_DEPENDS "${MV_SCINTILLA_ROOT}/src/*")
foreach(_source IN LISTS _coreFiles)
    if(NOT IS_DIRECTORY "${_source}")
        get_filename_component(_name "${_source}" NAME)
        if(NOT _name MATCHES "^(Selection\\.h|Selection\\.cxx|Editor\\.cxx|EditView\\.cxx)$")
            configure_file("${_source}" "${MV_SCINTILLA_CORE_ROOT}/${_name}" COPYONLY)
        endif()
    endif()
endforeach()

macro(mrst_wrap_replace before after)
    string(FIND "${_text}" "${before}" _offset)
    if(_offset EQUAL -1)
        message(FATAL_ERROR "Scintilla wrap-end patch no longer matches ${_name}")
    endif()
    string(REPLACE "${before}" "${after}" _text "${_text}")
endmacro()

foreach(_name Selection.h Selection.cxx Editor.cxx EditView.cxx)
    set(_source "${MV_SCINTILLA_ROOT}/src/${_name}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_source}")
    file(READ "${_source}" _text)
    string(REPLACE "\r\n" "\n" _text "${_text}")
    if(_name STREQUAL "Selection.h")
        mrst_wrap_replace([=[Sci::Position virtualSpace = 0;]=] [=[Sci::Position virtualSpace = 0;
	bool atWrapEnd = false;]=])
        mrst_wrap_replace([=[	explicit SelectionPosition(std::string_view &sv);]=] [=[	explicit SelectionPosition(std::string_view &sv);
	bool AtWrapEnd() const noexcept { return atWrapEnd; }
	void SetWrapEnd(bool value) noexcept { atWrapEnd = value; }]=])
        mrst_wrap_replace([=[		position = 0;]=] [=[		atWrapEnd = false;
		position = 0;]=])
        mrst_wrap_replace([=[		position = position_;]=] [=[		atWrapEnd = false;
		position = position_;]=])
        mrst_wrap_replace([=[		position = position + increment;]=] [=[		atWrapEnd = false;
		position = position + increment;]=])
    elseif(_name STREQUAL "Selection.cxx")
        mrst_wrap_replace([=[bool moveForEqual) noexcept {
	if (insertion)]=] [=[bool moveForEqual) noexcept {
	atWrapEnd = false;
	if (insertion)]=])
    elseif(_name STREQUAL "Editor.cxx")
        mrst_wrap_replace([=[return LocationFromPosition(SelectionPosition(pos), pe);]=] [=[return LocationFromPosition(pos == sel.MainCaret() ? sel.RangeMain().caret : SelectionPosition(pos), pe);]=])
        mrst_wrap_replace([=[		Point::FromInts(lastX - xOffset, static_cast<int>(newY)), false, false, UserVirtualSpace());

	if (direction < 0) {]=] [=[		Point::FromInts(lastX - xOffset, static_cast<int>(newY)), false, false, UserVirtualSpace());

	// A hit at the end of a wrapped row shares its document position with
	// the next row's start. Retain the target row instead of backing up over
	// its final character. Use SelectionPosition to avoid main-caret affinity.
	if (LocationFromPosition(posNew).y > static_cast<XYPOSITION>(newY)) {
		SelectionPosition wrapEnd = posNew;
		wrapEnd.SetVirtualSpace(0);
		wrapEnd.SetWrapEnd(true);
		if (LocationFromPosition(wrapEnd).y == static_cast<XYPOSITION>(newY))
			return wrapEnd;
	}

	if (direction < 0) {]=])
        mrst_wrap_replace([=[	case Message::CharRightExtend:
		if (FlagSet(virtualSpaceOptions, VirtualSpace::UserAccessible)]=] [=[	case Message::CharRightExtend:
		if (spCaret.AtWrapEnd()) {
			spCaret.SetWrapEnd(false);
			return spCaret;
		}
		if (FlagSet(virtualSpaceOptions, VirtualSpace::UserAccessible)]=])
        mrst_wrap_replace([=[		return SelectionPosition(StartEndDisplayLine(spCaret.Position(), false));]=] [=[	{
		// Include the final character, retaining the preceding screen row.
		const Sci::Position probe = spCaret.AtWrapEnd()
			? pdoc->NextPosition(spCaret.Position(), -1) : spCaret.Position();
		const Sci::Position end = StartEndDisplayLine(probe, false);
		const bool wrapped = end < pdoc->LineEndPosition(probe);
		SelectionPosition result(wrapped ? pdoc->NextPosition(end, 1) : end);
		result.SetWrapEnd(wrapped);
		return result;
	}]=])
        mrst_wrap_replace([=[return SelectionPosition(StartEndDisplayLine(spCaret.Position(), true));]=] [=[return SelectionPosition(StartEndDisplayLine(spCaret.AtWrapEnd()
			? pdoc->NextPosition(spCaret.Position(), -1) : spCaret.Position(), true));]=])
    elseif(_name STREQUAL "EditView.cxx")
        mrst_wrap_replace([=[				     const ViewStyle &vs, PointEnd pe, const PRectangle rcClient) {
	Point pt;]=] [=[				     const ViewStyle &vs, PointEnd pe, const PRectangle rcClient) {
	if (pos.AtWrapEnd())
		pe = static_cast<PointEnd>(static_cast<int>(pe) | static_cast<int>(PointEnd::subLineEnd));
	Point pt;]=])
        mrst_wrap_replace([=[		if (ll->InLine(offset, subLine) && offset <= ll->numCharsBeforeEOL) {]=] [=[		const bool inCaretLine = posCaret.AtWrapEnd()
			? ll->SubLineFromPosition(offset, PointEnd::subLineEnd) == subLine
			: ll->InLine(offset, subLine);
		if (inCaretLine && offset >= 0 && offset <= ll->numCharsBeforeEOL) {]=])
    endif()
    file(CONFIGURE OUTPUT "${MV_SCINTILLA_CORE_ROOT}/${_name}" CONTENT "${_text}" @ONLY NEWLINE_STYLE CRLF)
endforeach()
unset(_text)
unset(_coreFiles)
