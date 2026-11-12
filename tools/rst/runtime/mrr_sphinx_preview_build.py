"""MultiRoot-reST 프리뷰 빌더.

Sphinx 를 Python API 로 직접 구동해 HTML 을 만들고, docutils 노드가 알고 있는
원본 줄 번호를 그대로 HTML 속성으로 심는다. 결과 요약은 사이드카 JSON 으로
내보내며 C++ 쪽은 그 JSON 만 신뢰한다.

stdout 은 로그 패널 표시용이다. 딱 하나 예외가 PROGRESS_TAG 로 시작하는 줄인데,
그것은 진행막대용이라 앱이 걷어내고 로그에는 넣지 않는다 (_emit_progress 참고).

에디터 <-> 프리뷰 양방향 스크롤 동기화가 정확하려면 "이 DOM 요소가 원본 몇 번째
줄에서 몇 번째 줄까지인가" 를 알아야 한다. 생성된 HTML 에서 원본 텍스트를
substring 검색하는 방식으로는 인라인 마크업(**굵게**, :ref:`x`) 한 개만 있어도
깨지므로, 빌드 안에서 doctree 를 통해 얻는다.

종료 코드:
  0 성공 / 2 Sphinx 예외 / 3 인자 오류 / 4 sphinx·docutils 임포트 불가
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import sys
import time
import traceback
from pathlib import Path

try:
    import docutils.writers._html_base as docutils_html_base
    from docutils import nodes
    from sphinx.application import Sphinx
    from sphinx.errors import ExtensionError, ThemeError
except Exception:  # noqa: BLE001 - 런타임이 덜 갖춰진 경우를 명시적으로 구분한다.
    traceback.print_exc(file=sys.stderr)
    sys.exit(4)


REPORT_SCHEMA = 1

# 빌드가 도는 동안만 존재하는 표식. 출력 디렉터리는 프로젝트당 하나로 고정이므로
# (회전시키면 Sphinx 가 <outdir>/<docname>.html 로 증분을 판정하지 못한다) 빌드가
# 중간에 죽으면 **쓰다 만 HTML 이 최신 mtime 으로** 남아 영영 다시 쓰이지 않는다.
# 시작할 때 이 표식이 남아 있으면 직전 빌드가 중단된 것으로 보고 HTML 을 걷어낸다.
BUILDING_SENTINEL = ".mrr-building"

# data-mrr-src 인덱스 <-> 원본 경로 대응표. 출력 디렉터리와 수명을 같이한다.
SOURCE_MAP_NAME = ".mrr-sources.json"

# 문서별 읽기(파싱) 소요 ms. 미저장 편집을 반영할지 판정하는 근거다.
READ_COST_NAME = ".mrr-readcost.json"

# 직전 빌드에서 미저장 사본을 적용한 docname 목록. 사본이 사라졌을 때
# doctree 에 남은 편집 내용을 원본으로 되돌리는 데 쓴다.
SHADOWED_NAME = ".mrr-shadowed.json"

# 진행률 줄의 표식. 이 줄만 stdout 으로 **즉시** 나가고, 앱이 걷어내 진행막대에
# 쓴다. Sphinx 자신의 status 는 status_stream(StringIO)에 모였다가 _finish() 에서
# 한꺼번에 나오므로 진행률로 쓸 수 없다 — 빌드가 끝난 뒤에 도착한다.
PROGRESS_TAG = "@@MRST-PROGRESS "

# 입력 지문을 두던 옛 자리(출력 디렉터리 안). 지금은 앱이 `--inputs-file` 로
# 워크스페이스의 .multiroot 아래 경로를 지정한다. 옛 파일이 남아 있으면 아무도
# 읽지 않으면서 디버깅만 혼란스럽게 하므로 빌드할 때 걷어낸다.
LEGACY_INPUTS_NAME = ".mrr-inputs.json"

# 진단은 로그 패널과 진단 테이블에 그대로 실린다. breathe 가 doxygen 의 namespace 를
# 문서마다 다시 등록하면서 내는 Duplicate ID 경고는 같은 (파일,줄,메시지)가 수천 번
# 반복된다(실측 5911건 -> 사이드카 JSON 1.5MB). 중복은 정보가 아니므로 접는다.
MAX_DIAGNOSTICS = 2000

# Sphinx 경고의 표준 출력 형식: "<path>:<line>: WARNING: <message>"
WARNING_RE = re.compile(
    r"^(?P<path>.+?):(?P<line>\d+):\s*(?P<level>WARNING|ERROR|CRITICAL|SEVERE):\s*(?P<message>.*)$"
)
# 줄 번호 없이 파일만 나오는 형태도 있다.
WARNING_NO_LINE_RE = re.compile(
    r"^(?P<path>.+?):\s*(?P<level>WARNING|ERROR|CRITICAL|SEVERE):\s*(?P<message>.*)$"
)

# Sphinx 는 StringIO 로 보내도 색을 입힌다. 진단 텍스트에 이스케이프가 섞이면
# 그대로 진단 테이블에 노출되므로 파싱 전에 제거한다.
ANSI_RE = re.compile(r"\x1B\[[0-9;?]*[ -/]*[@-~]")

MISSING_EXTENSION_RE = re.compile(r"Could not import extension ([\w.]+)")
MISSING_THEME_RE = re.compile(r"no theme named ['\"]([^'\"]+)['\"]")
MISSING_MODULE_RE = re.compile(r"No module named ['\"]([^'\"]+)['\"]")


# ── docutils 패치 ────────────────────────────────────────────────────────────
#
# HTMLTranslator.starttag() 는 **attributes 로 넘어온 것과 node 의
# 'classes'/'ids' 만 출력한다. node.attributes 를 순회하지 않으므로 임의
# 속성(data-mrr-*)은 그냥 사라진다. 파이썬 원본은 이 사실을 놓쳐서 범위 기반
# 동기화가 한 번도 동작한 적이 없다(항상 앵커 보간으로 degrade).
#
# docutils 베이스 클래스를 패치하면 Sphinx 의 HTML5Translator 와 테마가 제공하는
# 서브클래스까지 모두 커버된다. 값은 docutils 자체 attval() 이스케이프를 탄다.
def _install_starttag_passthrough() -> None:
    base = docutils_html_base.HTMLTranslator
    if getattr(base, "_mrr_patched", False):
        return

    original_starttag = base.starttag

    def starttag(self, node, tagname, suffix="\n", empty=False, **attributes):
        node_attributes = getattr(node, "attributes", None)
        if isinstance(node_attributes, dict):
            for key, value in node_attributes.items():
                if isinstance(key, str) and key.startswith("data-mrr-"):
                    attributes[key] = value
        return original_starttag(self, node, tagname, suffix, empty, **attributes)

    base.starttag = starttag
    base._mrr_patched = True


# ── C++ 상호참조: 반드시 실패할 조회 건너뛰기 ────────────────────────────────

# `nsAit::Foo` 같은 대상에서 선행 식별자(`nsAit`)만 뽑는다. 평범한 식별자가
# 아니면(연산자, 소멸자, 템플릿 등) None 을 내어 **건너뛰지 않게** 한다.
_LEADING_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def _leading_identifier(target: str) -> str | None:
    head = target.strip()
    if head.startswith("::"):
        head = head[2:]
    head = head.split("::", 1)[0]
    for stop in "<([{ \t*&":
        index = head.find(stop)
        if index >= 0:
            head = head[:index]
    return head if _LEADING_IDENT_RE.match(head) else None


def _declared_cpp_identifiers(root_symbol) -> set[str]:
    """C++ 심볼 트리에 **선언된 모든 식별자 이름**.

    연산자 등 ASTIdentifier 가 아닌 것은 담지 않는다. 담지 않으면 그 이름은
    "선언되지 않은 것" 으로 보여 건너뛰기 대상이 되는데, 아래 호출측이 평범한
    식별자만 판정에 쓰므로 문제가 되지 않는다.
    """
    names: set[str] = set()
    stack = [root_symbol]
    while stack:
        symbol = stack.pop()
        name = getattr(getattr(symbol, "identOrOp", None), "name", None)
        if isinstance(name, str) and name:
            names.add(name)
        children = getattr(symbol, "_children", None)
        if children:
            stack.extend(children)
    return names


def _install_cpp_xref_shortcut(app: Sphinx) -> None:
    """해석될 수 없는 C++ 상호참조를 심볼 테이블 스캔 없이 즉시 포기한다.

    왜: 실측(iMonAIT/docs, code-libs 재빌드)에서 `CPPDomain._resolve_xref_inner`
    가 31.1초를 썼다. 그 중 **실패 8,203회가 17.8초**(성공 7,888회는 3.8초)다.
    실패가 4.6배 비싼 이유는, 조회가 안쪽 스코프에서 루트까지 거슬러 올라가며
    끝까지 못 찾으면 심볼 테이블 **전체**를 훑기 때문이다. 그리고 실패 대상은
    `std`(3,136회), `std::string`, `std::wstring`, `uint32_t`, `QString`,
    `ULONG` 처럼 이 프로젝트에 문서화되지 않은 외부 타입이다.

    안전한 근거: C++ 조회는 스코프를 안에서 밖으로 훑으므로, 어떤 이름이 트리
    **어디에도** 선언돼 있지 않으면 어느 스코프에서도 찾을 수 없다. 그래서 이
    단축은 원래도 반드시 실패했을 조회만 없앤다. 실패의 결과물도 바뀌지 않는다 —
    Sphinx 는 해석 실패 시 pending_xref 를 내용 노드로 그대로 대체한다
    (transforms/post_transforms/__init__.py 의 `new_nodes = [contnode]`).
    즉 링크가 걸리던 것은 그대로 걸리고, 안 걸리던 것은 그대로 안 걸린다.

    형제 스코프에 `nsAit::detail::std` 같은 것이 있다면 그 이름이 집합에 들어
    있으므로 건너뛰지 않는다. 판정은 안전한 쪽으로 실패한다.

    Sphinx 내부에 의존하므로 어느 단계에서든 실패하면 원래 동작으로 돌아간다.
    """
    # Sphinx 내부를 건드리므로 끌 수단을 둔다. 산출물이 달라 보이는 일이 생기면
    # 이 변수로 원래 동작과 바로 비교할 수 있다(개발 중 A/B 검증에도 쓴다).
    if os.environ.get("MRST_NO_CPP_XREF_SHORTCUT"):
        return

    try:
        from sphinx.domains.cpp import CPPDomain
    except Exception:  # noqa: BLE001
        return

    if getattr(CPPDomain, "_mrr_xref_shortcut", False):
        return

    original_inner = CPPDomain._resolve_xref_inner

    def _resolve_xref_inner(self, env, fromdocname, builder, typ, target, node, contnode):
        try:
            leading = _leading_identifier(target)
            if leading is not None:
                declared = getattr(self, "_mrr_declared_idents", None)
                if declared is None:
                    root = self.data.get("root_symbol")
                    declared = _declared_cpp_identifiers(root) if root is not None else set()
                    self._mrr_declared_idents = declared
                if declared and leading not in declared:
                    return None, None
        except Exception:  # noqa: BLE001
            pass   # 판정에 실패하면 그냥 원래 조회를 한다
        return original_inner(self, env, fromdocname, builder, typ, target, node, contnode)

    CPPDomain._resolve_xref_inner = _resolve_xref_inner
    CPPDomain._mrr_xref_shortcut = True


# ── 줄 번호 스탬핑 ───────────────────────────────────────────────────────────

class SourceLineStamper:
    """doctree 노드에 원본 (파일, 시작줄, 끝줄) 을 심는다."""

    def __init__(self, known_sources: list[str] | None = None) -> None:
        # HTML 에 심는 data-mrr-src 는 이 목록의 **인덱스**다. 증분 빌드에서는 이번에
        # 다시 쓰이지 않은 문서의 HTML 이 그대로 남으므로, 그 안의 인덱스가 계속
        # 같은 파일을 가리키려면 목록이 빌드마다 흔들리면 안 된다. 그래서 목록을
        # 출력 디렉터리에 남겨 두고 이어받으며, **추가만** 하고 재배열하지 않는다.
        self.sources: list[str] = []
        self._source_index: dict[str, int] = {}
        self._line_counts: dict[str, int] = {}
        # Path.resolve() 결과 캐시. 아래 _resolve() 주석 참고.
        self._resolved: dict[str, str] = {}
        # 이번 빌드에서 실제로 스탬프된 파일. 진단 교체 범위를 정하는 데 쓴다.
        self.touched: list[str] = []
        self._touched_seen: set[str] = set()

        for path in known_sources or []:
            if isinstance(path, str) and path and path not in self._source_index:
                self._source_index[path] = len(self.sources)
                self.sources.append(path)

    def _resolve(self, path: str) -> str:
        """Path(path).resolve() 를 문자열 키로 메모이제이션한다.

        왜 필요한가: 이 클래스는 doctree 의 **모든 노드**에 대해 원본 경로를
        정규화했다. 실측(iMonAIT/docs, code-libs 재빌드): 노드 하나당 두 번,
        합계 563,031회. Windows 의 Path.resolve() 는 nt._getfinalpathname 으로
        내려가 **파일 핸들을 열어** 정규화하므로 호출당 37.5us 다
        → 21.1초. 그런데 서로 다른 경로는 문서 수만큼(이 프로젝트에서 14개)뿐이다.

        캐시 후 같은 부하가 0.022초가 된다.
        """
        cached = self._resolved.get(path)
        if cached is None:
            cached = str(Path(path).resolve())
            self._resolved[path] = cached
        return cached

    def source_index(self, path: str | None) -> int:
        if not path:
            return -1
        resolved = self._resolve(path)
        if resolved not in self._source_index:
            self._source_index[resolved] = len(self.sources)
            self.sources.append(resolved)
        if resolved not in self._touched_seen:
            self._touched_seen.add(resolved)
            self.touched.append(resolved)
        return self._source_index[resolved]

    def line_count(self, path: str) -> int:
        if path not in self._line_counts:
            try:
                text = Path(path).read_text(encoding="utf-8", errors="replace")
                self._line_counts[path] = max(1, text.count("\n") + 1)
            except OSError:
                self._line_counts[path] = 1
        return self._line_counts[path]

    def stamp_doctree(self, doctree: nodes.document) -> None:
        entries: list[dict] = []
        self._collect(doctree, entries, getattr(doctree, "source", None))
        if not entries:
            return

        # 1) line 이 없는 노드는 문서 순서상 가장 가까운 앞 노드의 줄을 물려받는다.
        #    (원본은 이런 노드를 통째로 건너뛰어 커버리지가 크게 낮았다.)
        last_known: int | None = None
        for entry in entries:
            if entry["line"] is None:
                entry["line"] = last_known
            else:
                last_known = entry["line"]

        _mark_first_anchor_per_line(entries)
        self._apply(entries)

    def _apply(self, entries: list[dict]) -> None:
        # 끝줄 = 문서 순서상 자기 서브트리 바로 다음 노드의 시작줄 - 1.
        # rawsource 의 줄 수를 세는 방식은 section/container 처럼 rawsource 가
        # 빈 노드에서 end == start 가 되어, 정작 범위 보간이 필요한 큰 요소가
        # 쓸모없어진다.
        total = len(entries)
        for entry in entries:
            line = entry["line"]
            if line is None or line < 1:
                continue

            end_line = None
            following = entry["subtree_last"] + 1
            if following < total:
                candidate = entries[following]
                if candidate["src"] == entry["src"] and candidate["line"]:
                    end_line = candidate["line"] - 1
            if end_line is None and entry["src"]:
                end_line = self.line_count(entry["src"])
            if end_line is None or end_line < line:
                end_line = line

            node = entry["node"]
            node["data-mrr-start-line"] = str(line)
            node["data-mrr-end-line"] = str(end_line)

            src_index = self.source_index(entry["src"])
            if src_index >= 0:
                node["data-mrr-src"] = str(src_index)

            # ids 는 어떤 경우에도 출력되므로, starttag 패치가 깨지더라도
            # 앵커 기반 보간은 계속 동작한다.
            if not entry["anchor_taken"]:
                anchor = f"mrr-line-{max(0, src_index)}-{line}"
                ids = node.setdefault("ids", [])
                if anchor not in ids:
                    ids.append(anchor)

    def _collect(self, node: nodes.Element, entries: list[dict], parent_source: str | None) -> int:
        index = len(entries)
        raw_line = getattr(node, "line", None)
        line = raw_line if isinstance(raw_line, int) and raw_line >= 1 else None
        source = getattr(node, "source", None) or parent_source

        entries.append(
            {
                "node": node,
                "line": line,
                "src": self._resolve(source) if source else None,
                "subtree_last": index,
                "anchor_taken": False,
            }
        )

        for child in node.children:
            if isinstance(child, nodes.Element):
                self._collect(child, entries, source)

        entries[index]["subtree_last"] = len(entries) - 1
        return index


def _mark_first_anchor_per_line(entries: list[dict]) -> None:
    """같은 (파일, 줄) 조합에는 앵커를 하나만 남긴다."""
    seen: set[tuple[str | None, int]] = set()
    for entry in entries:
        key = (entry["src"], entry["line"])
        if key in seen:
            entry["anchor_taken"] = True
        else:
            seen.add(key)


# ── 진단 수집 ────────────────────────────────────────────────────────────────

def _parse_warnings(text: str) -> list[dict]:
    diagnostics: list[dict] = []
    seen: set[tuple[str, int, str, str]] = set()
    dropped = 0

    def add(path: str, line: int, severity: str, message: str) -> None:
        nonlocal dropped
        key = (path, line, severity, message)
        if key in seen:
            dropped += 1
            return
        seen.add(key)
        if len(diagnostics) >= MAX_DIAGNOSTICS:
            dropped += 1
            return
        diagnostics.append(
            {"path": path, "line": line, "severity": severity, "message": message}
        )

    for raw_line in ANSI_RE.sub("", text).splitlines():
        line = raw_line.strip()
        if not line:
            continue

        match = WARNING_RE.match(line)
        if match:
            add(
                match.group("path"),
                int(match.group("line")),
                match.group("level").lower(),
                match.group("message").strip(),
            )
            continue

        match = WARNING_NO_LINE_RE.match(line)
        if match and not match.group("path").upper().startswith("WARNING"):
            add(
                match.group("path"),
                1,
                match.group("level").lower(),
                match.group("message").strip(),
            )

    if dropped and diagnostics:
        # 접힌 것이 있다는 사실 자체는 남긴다. 조용히 자르면 "경고가 없다" 로 읽힌다.
        diagnostics.append(
            {
                "path": diagnostics[0]["path"],
                "line": 1,
                "severity": "warning",
                "message": f"(중복·초과 진단 {dropped}건을 접었습니다)",
            }
        )
    return diagnostics


def _collect_missing(text: str) -> tuple[list[dict], list[str]]:
    text = ANSI_RE.sub("", text)
    modules = {m for m in MISSING_EXTENSION_RE.findall(text)}
    modules.update(MISSING_MODULE_RE.findall(text))
    themes = sorted(set(MISSING_THEME_RE.findall(text)))
    missing = [{"module": name, "distribution": _distribution_for(name)} for name in sorted(modules)]
    return missing, themes


def _distribution_for(module: str) -> str:
    """모듈명 -> PyPI 배포명 추정. 확실하지 않으면 일반 규칙을 쓴다."""
    known = {
        "myst_parser": "myst-parser",
        "sphinx_design": "sphinx-design",
        "sphinx_copybutton": "sphinx-copybutton",
        "sphinx_tabs": "sphinx-tabs",
        "sphinx_rtd_theme": "sphinx-rtd-theme",
        "sphinx_book_theme": "sphinx-book-theme",
        "pydata_sphinx_theme": "pydata-sphinx-theme",
        "sphinxawesome_theme": "sphinxawesome-theme",
    }
    if module in known:
        return known[module]
    return module.replace("_", "-").replace(".", "-")


# ── conf.py 정적 검사 ────────────────────────────────────────────────────────

def _conf_declares_empty_html_style(conf_path: Path) -> bool:
    """레거시 html_style = '' 감지. Sphinx 8 에서 _static checksum 오류를 낸다."""
    try:
        import ast

        # Sphinx 는 conf.py 를 BOM 을 벗겨 읽는다. utf-8 로 읽으면 첫 글자 U+FEFF 에서
        # SyntaxError 가 나고 감지가 조용히 빠져, 빈 html_style 이 그대로 빌드를 깬다.
        tree = ast.parse(conf_path.read_text(encoding="utf-8-sig", errors="replace"))
    except (OSError, SyntaxError, ValueError):
        return False

    found = False
    for node in ast.walk(tree):
        targets = []
        if isinstance(node, ast.Assign):
            targets = node.targets
            value = node.value
        elif isinstance(node, ast.AnnAssign):
            targets = [node.target]
            value = node.value
        else:
            continue

        for target in targets:
            if isinstance(target, ast.Name) and target.id == "html_style":
                if isinstance(value, ast.Constant) and value.value == "":
                    found = True
                else:
                    found = False
    return found


# ── 메인 ─────────────────────────────────────────────────────────────────────

def _parse_shadow_sources(values: list[str]) -> dict[str, Path]:
    shadow: dict[str, Path] = {}
    for value in values:
        source_text, separator, temp_text = value.partition("=")
        if not separator or not source_text.strip() or not temp_text.strip():
            continue
        shadow[str(Path(source_text.strip()).resolve())] = Path(temp_text.strip())
    return shadow


_DOXYGEN_DIRECTIVE_RE = re.compile(r"^([ \t]*)\.\.[ \t]+(doxygen[a-z]*)[ \t]*::(.*)$",
                                   re.IGNORECASE)


def _stub_doxygen_directives(text: str) -> tuple[str, int]:
    """`.. doxygen*::` 블록을 자리표시자로 바꾼다. **줄 수를 그대로 유지한다.**

    왜 하는가: 이 지시어 하나가 doxygen XML 수백 개를 훑어 docutils 노드 수만 개로
    펼쳐진다. 실측(iMonAIT/docs)에서 재파싱이 code-libs 38.3초 / code-svc 8.3초라,
    타이핑할 때마다 반영하는 것이 불가능했다(그래서 지금은 아예 포기하고 있다).
    타이핑 중에는 API 본문이 필요 없으므로 자리표시자로 두고, 저장하면 온전히 만든다.

    **줄 수를 유지해야 하는 이유**: 양방향 스크롤 동기화는 HTML 에 심은 원본 줄
    번호(data-mrr-start-line)로 동작한다. 블록을 줄 수가 다른 것으로 바꾸면 그
    뒤의 모든 줄 번호가 밀려 동기화가 어긋난다. 그래서 첫 줄만 자리표시자로 쓰고
    나머지는 빈 줄로 채운다.

    블록의 범위는 들여쓰기로 정한다 — 지시어보다 깊게 들여쓴 줄과 빈 줄이 본문이고,
    들여쓰기가 지시어 이하인 비어 있지 않은 줄에서 끝난다(docutils 규칙).
    자리표시자를 지시어와 같은 열에 두므로, 다른 지시어 안에 중첩돼 있어도
    부모 블록의 구조가 깨지지 않는다.
    """
    lines = text.split("\n")
    out = list(lines)
    total = len(lines)
    stubbed = 0
    index = 0

    while index < total:
        match = _DOXYGEN_DIRECTIVE_RE.match(lines[index])
        if match is None:
            index += 1
            continue

        indent = len(match.group(1))
        directive = match.group(2)
        argument = match.group(3).strip()

        cursor = index + 1
        while cursor < total:
            line = lines[cursor]
            if line.strip() == "":
                cursor += 1
                continue
            if len(line) - len(line.lstrip()) <= indent:
                break
            cursor += 1

        # 블록 끝의 빈 줄은 블록에 넣지 않는다. 뒤 문단과의 구분을 남겨야 한다.
        last = cursor - 1
        while last > index and lines[last].strip() == "":
            last -= 1

        # 백틱은 인라인 리터럴을 깨므로 지운다.
        label = f"{directive}:: {argument}".strip().replace("`", "")
        out[index] = (f"{' ' * indent}.. note:: 저장하면 표시됩니다 "
                      f"— 미저장 편집 중에는 ``{label}`` 을 생략합니다.")
        for blanked in range(index + 1, last + 1):
            out[blanked] = ""

        stubbed += 1
        index = last + 1

    return "\n".join(out), stubbed


def _shadow_source_reader(shadow_by_docname: dict[str, Path],
                          stub_doxygen: bool,
                          stubbed_docs: dict[str, int]):
    def replace_source(_app: Sphinx, docname: str, source: list[str]) -> None:
        replacement = shadow_by_docname.get(docname)
        if replacement is None:
            return
        try:
            text = replacement.read_text(encoding="utf-8-sig")
        except OSError:
            return

        if stub_doxygen:
            text, stubbed = _stub_doxygen_directives(text)
            if stubbed:
                stubbed_docs[docname] = stubbed

        source[0] = text

    return replace_source


def _plan_shadow(app: Sphinx, args, read_costs: dict[str, int],
                 status: io.StringIO) -> tuple[dict[str, Path], list[str]]:
    """미저장 사본을 어느 문서에 적용할지 정하고, 재읽기 상태를 맞춘다.

    Sphinx 는 **원본 파일의 mtime** 으로만 재읽기를 판정하는데(get_outdated_files),
    사본을 써도 원본 mtime 은 그대로라 그 문서가 outdated 로 잡히지 않는다. 그러면
    source-read 훅 자체가 호출되지 않아 사본이 반영될 길이 없다. 그래서 적용 대상은
    reread_always 에 넣어 강제로 다시 읽게 한다.

    되돌리는 쪽도 함께 처리해야 한다. all_docs 에 들어가는 값은 원본 mtime 이 아니라
    **읽은 시각**이라(builders/__init__.py 의 all_docs[docname] = time_ns), 사본으로
    한 번 읽고 나면 원본 mtime 이 항상 그보다 과거가 된다. 저장하지 않고 편집을
    되돌리면 그 문서는 영영 다시 읽히지 않고 사라진 편집 내용이 프리뷰에 남는다.
    직전에 사본이 걸렸다가 이번에 빠진 문서는 all_docs 에서 빼서 강제로 원본을
    다시 읽게 한다.

    Returns (docname -> 사본 경로, 비용 때문에 건너뛴 docname 목록).
    """
    shadow_by_path = _parse_shadow_sources(args.shadow)
    limit = args.shadow_max_read_ms
    stub_doxygen = bool(getattr(args, "stub_doxygen", False))

    applied: dict[str, Path] = {}
    skipped: list[str] = []
    for source_path, replacement in shadow_by_path.items():
        try:
            docname = app.env.path2doc(source_path)
        except Exception:  # noqa: BLE001
            docname = None
        if not docname:
            continue

        # 비용을 아직 모르는 문서는 **적용하지 않는다.** 일단 적용해 보고 재는
        # 쪽은 그 한 번이 수십 초일 수 있어서, 하필 사용자가 처음 타이핑한
        # 순간에 멈춘다. 비용은 전량 빌드나 저장 시 재읽기에서 채워지므로,
        # 한 번 읽히고 나면 다음 편집부터 정상적으로 반영된다.
        cost = read_costs.get(docname)
        # 지시어를 자리표시자로 바꿀 것이면 재파싱 비용이 어차피 작아지므로
        # 이 임계값이 의미가 없다. 임계값이 막고 있던 것이 정확히 이 비용이다.
        if stub_doxygen:
            limit = -1
        if limit >= 0:
            if cost is None:
                skipped.append(docname)
                status.write(
                    f"미저장 편집을 건너뜁니다(재파싱 시간을 아직 모름): {docname}\n")
                continue
            if cost > limit:
                skipped.append(docname)
                status.write(
                    f"미저장 편집을 건너뜁니다(재파싱 {cost}ms > {limit}ms): {docname}\n")
                continue
        applied[docname] = replacement

    previous = _load_json(args.out_dir / SHADOWED_NAME, [])
    for docname in previous:
        if not isinstance(docname, str) or docname in applied:
            continue
        # 사본이 빠졌다 -> 원본을 한 번 강제로 다시 읽어야 한다.
        app.env.reread_always.discard(docname)
        app.env.all_docs.pop(docname, None)

    for docname in applied:
        app.env.reread_always.add(docname)

    return applied, skipped


def _disable_mermaid_fullscreen(app: Sphinx) -> bool:
    """프리뷰에서는 sphinxcontrib-mermaid 의 전체화면 장식을 끈다.

    켜져 있으면(기본값이 True 다) 그쪽 스크립트가 도형마다 `<pre class="mermaid">`
    를 `<div class="mermaid-container">` 로 **감싸고** 단추를 붙이며 `<body>` 끝에
    모달을 만든다. 셋 다 우리가 보낸 HTML 에 없는 노드다. 그러면 프리뷰가 본문을
    갱신할 때 짝지을 상대가 없어 도형을 통째로 갈아 끼우게 되고, 이미 그려 둔
    SVG 가 매번 버려진다 (mrr_preview.js 의 `rerenderDiagrams` 참고).

    끄면 도형은 그대로 그려지고 `<pre class="mermaid">` 가 제자리에 남는다.
    잃는 것은 **프리뷰에서** 도형을 눌러 크게 보는 기능뿐이며, 사용자가 실제로
    내는 문서(`sphinx-build`)에는 영향이 없다.

    `confoverrides` 로 넘기지 않는 이유: 그 값을 모르는 프로젝트에서는 Sphinx 가
    「unknown config value ... in override, ignoring」 경고를 내고, 그 줄이 빌드마다
    로그에 쌓인다. 확장이 실제로 실려 설정이 등록된 뒤에만 손대면 조용하다.
    `install_js` 가 `html-page-context` 에서 이 값을 읽으므로 시점도 늦지 않다.
    """
    if "mermaid_fullscreen" not in app.config:
        return False

    app.config.mermaid_fullscreen = False
    return True


def _install_read_cost_meter(app: Sphinx, costs: dict[str, int]) -> None:
    """문서별 읽기(파싱) 소요 시간을 ms 로 잰다.

    미저장 편집을 반영하려면 그 문서를 **매번 다시 읽어야** 하는데, breathe 처럼
    디렉티브 하나가 doxygen XML 수백 개를 훑는 문서는 재파싱만 수십 초가 걸린다.
    그런 문서까지 반영하면 타이핑할 때마다 프리뷰가 멈춰 버리므로, 직전 빌드에서
    잰 값으로 다음 빌드에서 반영할지 말지를 정한다.

    Sphinx 자체 sphinx.ext.duration 과 같은 방식이다(source-read ~ doctree-read).
    """
    started: dict[str, float] = {}

    def on_source_read(_app: Sphinx, docname: str, _source: list[str]) -> None:
        started[docname] = time.perf_counter()

    def on_doctree_read(_app: Sphinx, _doctree: nodes.document) -> None:
        docname = _app.env.docname
        begin = started.pop(docname, None)
        if begin is not None:
            costs[docname] = int((time.perf_counter() - begin) * 1000)

    # 치환보다 먼저 걸어야 사본을 읽는 시간까지 비용에 포함된다.
    app.connect("source-read", on_source_read, priority=100)
    app.connect("doctree-read", on_doctree_read)


def _emit_progress(phase: str, done: int, total: int) -> None:
    """진행률 한 줄을 stdout 으로 즉시 흘려보낸다.

    flush 가 핵심이다. stdout 이 파이프면 파이썬이 블록 버퍼링을 하므로, flush 하지
    않으면 이 줄들도 빌드가 끝날 때 한꺼번에 나가 status_stream 과 다를 바가 없다.
    """
    try:
        # print 가 줄바꿈을 붙이고 flush 까지 한 번에 한다.
        print(PROGRESS_TAG + json.dumps(
            {"phase": phase, "done": int(done), "total": int(total)}), flush=True)
    except Exception:  # noqa: BLE001 - 진행막대 때문에 빌드를 깨뜨리지 않는다.
        pass


def _install_progress_reporter(app: Sphinx) -> None:
    """읽기/쓰기 진행률을 Sphinx 이벤트에서 뽑아 흘려보낸다.

    **status 문자열을 파싱하지 않는다.** `reading sources... [ 42%]` 같은 형식은
    Sphinx 버전과 tty 여부에 따라 달라지고, 애초에 그 글자는 버퍼에 모였다가
    끝에 나온다. 이벤트는 그런 사정이 없다.

    분모는 어림값이다. 쓰기 쪽은 `html-page-context` 가 genindex·search 처럼
    문서 목록에 없는 페이지에도 오므로 done 이 total 을 넘을 수 있어서, 넘는
    만큼 total 을 늘려 잡는다. 앱 쪽에서도 구간 안으로 가둔다.

    사용자 프로젝트의 venv 에 어떤 Sphinx 가 있을지 모르므로 이벤트마다 따로
    감싼다. 없는 이벤트가 있어도 빌드는 그대로 되고 진행률만 그 단계가 빠진다.
    """
    state = {"read_total": 0, "read_done": 0, "write_total": 0, "write_done": 0}

    def on_before_read(_app, _env, docnames) -> None:
        state["read_total"] = len(docnames)
        state["read_done"] = 0
        _emit_progress("read", 0, state["read_total"])

    def on_source_read(_app, _docname, _source) -> None:
        state["read_done"] += 1
        _emit_progress("read", state["read_done"], max(state["read_total"], state["read_done"]))

    def on_page_context(app_, _pagename, _templatename, _context, _doctree) -> None:
        if not state["write_total"]:
            state["write_total"] = max(len(app_.env.all_docs), 1)
        state["write_done"] += 1
        _emit_progress("write", state["write_done"], max(state["write_total"], state["write_done"]))

    for event, handler in (
        ("env-before-read-docs", on_before_read),
        ("source-read", on_source_read),
        ("html-page-context", on_page_context),
    ):
        try:
            app.connect(event, handler)
        except Exception:  # noqa: BLE001 - 그 단계의 진행률만 빠진다.
            pass


def _claim_out_dir(out_dir: Path) -> None:
    """중단된 직전 빌드의 잔해를 걷어내고 이번 빌드의 표식을 남긴다.

    표식이 남아 있다는 것은 이전 프로세스가 HTML 을 쓰는 도중에 죽었다는 뜻이다.
    그 파일들은 내용이 잘렸는데 mtime 은 최신이라 Sphinx 가 최신으로 판정한다.
    지워 두면 다음 빌드가 doctree 를 다시 읽지 않고 쓰기만 다시 한다.
    """
    sentinel = out_dir / BUILDING_SENTINEL
    try:
        out_dir.mkdir(parents=True, exist_ok=True)
        if sentinel.exists():
            for stale in out_dir.glob("*.html"):
                try:
                    stale.unlink()
                except OSError:
                    pass
        sentinel.write_text("", encoding="utf-8")
    except OSError:
        pass


def _release_out_dir(out_dir: Path) -> None:
    try:
        (out_dir / BUILDING_SENTINEL).unlink(missing_ok=True)
    except OSError:
        pass


def _load_source_map(out_dir: Path) -> list[str]:
    try:
        data = json.loads((out_dir / SOURCE_MAP_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    if isinstance(data, list):
        return [item for item in data if isinstance(item, str)]
    return []


def _save_source_map(out_dir: Path, sources: list[str]) -> None:
    try:
        (out_dir / SOURCE_MAP_NAME).write_text(
            json.dumps(sources, ensure_ascii=False), encoding="utf-8")
    except OSError:
        pass


def _template_directories(app, conf_dir: Path) -> dict[str, list[str]]:
    """프로젝트와 상속 테마의 템플릿 파일 목록. 빈/없는 폴더도 기록한다."""
    roots = {Path(conf_dir) / entry for entry in getattr(app.config, "templates_path", ())}
    templates = getattr(getattr(app, "builder", None), "templates", None)
    roots.update(Path(entry) for entry in getattr(templates, "pathchain", ()))
    result = {}
    for root in sorted({entry.resolve() for entry in roots}):
        result[str(root)] = sorted(
            entry.relative_to(root).as_posix() for entry in root.rglob("*") if entry.is_file()
        )
    return result


def _collect_input_files(app, conf_dir: Path, template_dirs=None) -> set[str]:
    """이번 빌드의 결과를 좌우하는 입력 파일 전부.

    문서·설정·확장 의존 파일과 템플릿을 합친다.

    1. conf.py — 확장/테마/설정이 전부 여기서 나온다.
    2. 소스 문서와 그 의존 파일 — ``env.doc2path`` 와 ``env.dependencies``.
       후자는 ``.. include::`` / ``literalinclude`` 처럼 Sphinx 가 이미 추적하는 것이다.
    3. breathe 가 읽은 doxygen XML — breathe 는 ``note_dependency()`` 를 쓰지 않고
       대신 ``env.breathe_file_state`` 에 {경로: (mtime, docnames)} 를 담아
       environment.pickle 로 영속화한다(breathe/file_state_cache.py).
       실제로 읽힌 것만 들어 있으므로 XML 전량이 아니다.

    4. 프로젝트·상속 테마의 템플릿과 실제 로드된 외부 템플릿.

    breathe 가 없는 프로젝트에서는 3번이 그냥 비어 있다.
    """
    paths: set[str] = set()
    directories = _template_directories(app, conf_dir) if template_dirs is None else template_dirs
    for root, files in directories.items():
        paths.update(str((Path(root) / entry).resolve()) for entry in files)
    templates = getattr(getattr(app, "builder", None), "templates", None)
    for template in (getattr(getattr(templates, "environment", None), "cache", None) or {}).values():
        filename = getattr(template, "filename", None)
        if filename:
            paths.add(str(Path(filename).resolve()))

    conf_py = conf_dir / "conf.py"
    if conf_py.is_file():
        paths.add(str(conf_py.resolve()))

    env = getattr(app, "env", None)
    if env is None:
        return paths

    for docname in getattr(env, "found_docs", set()):
        try:
            paths.add(str(Path(env.doc2path(docname)).resolve()))
        except Exception:  # noqa: BLE001
            continue

    srcdir = Path(app.srcdir)
    for deps in getattr(env, "dependencies", {}).values():
        for dep in deps:
            try:
                paths.add(str((srcdir / dep).resolve()))
            except Exception:  # noqa: BLE001
                continue

    for filename in getattr(env, "breathe_file_state", {}):
        try:
            paths.add(str(Path(filename).resolve()))
        except Exception:  # noqa: BLE001
            continue

    return paths


def _save_inputs(inputs_file: Path, app, conf_dir: Path, source_dir: Path) -> None:
    """이번 빌드가 읽은 입력 파일들의 (경로, mtime, 크기) 를 남긴다.

    앱은 다음 기동에서 이것만 비교해 "바뀐 것이 없으면 아예 python 을 띄우지
    않는다". 변경이 하나도 없어도 빌드 한 번이 통째로 드는데(프로세스 기동 +
    sphinx 임포트 + 32MB environment.pickle 언피클, 실측 1.7~2.6초) 그동안
    프리뷰가 비어 있다. Sphinx 자신도 같은 판정을 하지만, 그 판정을 하려면
    먼저 그 시간을 다 써야 한다는 것이 문제다.

    경로는 앱이 정해서 넘긴다(`--inputs-file`). 이름 규칙을 파이썬과 C++ 양쪽에
    두면 한쪽만 고쳐졌을 때 조용히 어긋나기 때문이다.
    """
    template_dirs = _template_directories(app, conf_dir)
    templates = getattr(getattr(app, "builder", None), "templates", None)
    custom_bridge = getattr(app.config, "template_bridge", None)
    templates_complete = not custom_bridge or hasattr(templates, "pathchain")
    entries = []
    for path in sorted(_collect_input_files(app, conf_dir, template_dirs)):
        try:
            stat = os.stat(path)
        except OSError:
            # 지금 없는 파일은 기록하지 않는다. 대신 아래 sourceCount 가
            # 개수 변화를 잡고, 목록에 없던 파일이 생기는 것도 그쪽이 잡는다.
            continue
        entries.append({"p": path, "m": int(stat.st_mtime_ns), "s": stat.st_size})

    # 목록에 없는 **새 문서**가 추가되는 것은 mtime 비교로 잡을 수 없다.
    # 소스 트리의 문서 개수를 함께 남겨 그 경우를 잡는다.
    # source_suffix 는 Sphinx 버전과 설정에 따라 dict / list / str 셋 다 될 수 있다.
    # str 을 그대로 tuple() 에 넣으면 글자 단위로 쪼개진다.
    raw_suffix = getattr(app.config, "source_suffix", None) or {".rst": None}
    suffixes = (raw_suffix,) if isinstance(raw_suffix, str) else tuple(raw_suffix)
    source_count = 0
    for entry in source_dir.rglob("*"):
        if entry.is_file() and entry.suffix in suffixes:
            source_count += 1

    # .multiroot 이 아직 없을 수 있다(워크스페이스를 처음 여는 경우).
    try:
        inputs_file.parent.mkdir(parents=True, exist_ok=True)
    except OSError:
        return

    _save_json(inputs_file, {
        "schema": 2,
        "templatesComplete": templates_complete,
        "templateDirs": [{"p": root, "files": files} for root, files in template_dirs.items()],
        "sourceDir": str(source_dir.resolve()),
        "sourceSuffixes": sorted(suffixes),
        "sourceCount": source_count,
        "files": entries,
    })


def _load_json(path: Path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def _save_json(path: Path, payload) -> None:
    try:
        path.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
    except OSError:
        pass


def _trim_builder_output(app: Sphinx) -> None:
    """프리뷰에 쓰이지 않는 산출물을 끈다.

    검색 인덱스( searchindex.js — 이 저장소 실측 5MB )와 전역 색인
    ( genindex.html — 2.3MB )은 프리뷰에서 아무도 열지 않는데, 문서를 쓸 때마다
    indexer.feed() 가 함께 돌고 finish 단계에서 매번 다시 만들어진다.

    conf 값( html_use_index )이 아니라 **빌더 인스턴스 속성**을 바꾼다. confval 을
    건드리면 .buildinfo 의 config 해시가 달라져 전 문서가 한 번 더 재작성된다.
    """
    builder = getattr(app, "builder", None)
    if builder is None:
        return
    if hasattr(builder, "search"):
        builder.search = False
    if hasattr(builder, "use_index"):
        builder.use_index = False


def _write_report(path: Path, payload: dict) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    except OSError:
        pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build a Sphinx HTML preview for MultiRoot reST.")
    parser.add_argument("--conf-dir", required=True, type=Path)
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--doctree-dir", required=True, type=Path)
    parser.add_argument("--report", type=Path, default=None)
    parser.add_argument("--inputs-file", type=Path, default=None,
                        help="입력 지문을 남길 경로. 생략하면 남기지 않는다"
                             "(그러면 앱이 매번 빌드한다).")
    parser.add_argument("--primary", type=Path, default=None,
                        help="편집 중인 원본 파일. htmlPath 계산에 쓴다.")
    parser.add_argument("--shadow", action="append", default=[], metavar="SRC=TMP",
                        help="원본 파일의 내용을 임시 파일 내용으로 대체 (미저장 버퍼 반영).")
    parser.add_argument("--stub-doxygen", action="store_true",
                        help="미저장 사본을 적용하는 문서의 .. doxygen*:: 블록을 "
                             "자리표시자로 바꾼다(타이핑 중 반응성). 줄 수는 유지한다.")
    parser.add_argument("--shadow-max-read-ms", type=int, default=-1, metavar="MS",
                        help="직전 빌드의 읽기 시간이 이 값을 넘는 문서에는 --shadow 를 "
                             "적용하지 않는다. 음수면 제한 없음.")
    parser.add_argument("--auto-fix-legacy-conf", action="store_true",
                        help="html_style='' 을 감지해 None 으로 override.")
    parser.add_argument("--define", action="append", default=[], metavar="KEY=VALUE")
    args = parser.parse_args(argv)

    report_path = args.report or (args.out_dir / ".mrr-build.json")
    report: dict = {
        "schema": REPORT_SCHEMA,
        "ok": False,
        "outDir": str(args.out_dir),
        "htmlPath": "",
        "primaryDocname": "",
        "sources": [],
        "processedSources": [],
        "shadowApplied": [],
        "shadowSkipped": [],
        "diagnostics": [],
        "missingExtensions": [],
        "missingThemes": [],
        "traceback": None,
    }

    confoverrides: dict[str, object] = {}
    conf_file = args.conf_dir / "conf.py"
    if args.auto_fix_legacy_conf and _conf_declares_empty_html_style(conf_file):
        confoverrides["html_style"] = None
        report["confOverridesApplied"] = {"html_style": None}

    for define in args.define:
        key, separator, value = define.partition("=")
        if separator and key.strip():
            confoverrides[key.strip()] = value

    warning_stream = io.StringIO()
    status_stream = io.StringIO()

    _claim_out_dir(args.out_dir)

    try:
        _install_starttag_passthrough()

        app = Sphinx(
            srcdir=str(args.source_dir),
            confdir=str(args.conf_dir),
            outdir=str(args.out_dir),
            doctreedir=str(args.doctree_dir),
            buildername="html",
            confoverrides=confoverrides,
            status=status_stream,
            warning=warning_stream,
        )

        if _disable_mermaid_fullscreen(app):
            report["previewDecorationsOff"] = ["mermaid_fullscreen"]

        read_costs: dict[str, int] = {
            name: int(value)
            for name, value in _load_json(args.out_dir / READ_COST_NAME, {}).items()
            if isinstance(value, (int, float))
        }
        _install_read_cost_meter(app, read_costs)
        _install_progress_reporter(app)

        applied, skipped = _plan_shadow(app, args, read_costs, status_stream)
        stubbed_docs: dict[str, int] = {}
        if applied:
            app.connect("source-read",
                        _shadow_source_reader(applied, bool(args.stub_doxygen), stubbed_docs))
        report["shadowApplied"] = sorted(applied)
        report["shadowSkipped"] = sorted(skipped)

        _trim_builder_output(app)
        _install_cpp_xref_shortcut(app)

        stamper = SourceLineStamper(_load_source_map(args.out_dir))
        app.connect("doctree-resolved",
                    lambda _app, doctree, _docname: stamper.stamp_doctree(doctree))
        # 계측기가 값을 덮어쓰기 **전에** 원본을 떠 둔다. 아래에서 자리표시자로
        # 읽은 문서만 되돌린다.
        costs_before_build = dict(read_costs)

        app.build()
        _save_source_map(args.out_dir, stamper.sources)

        # 자리표시자로 읽은 문서의 비용은 기록하지 않는다. 그것은 **온전한 문서의
        # 재파싱 비용이 아니라** 지시어를 뺀 값이라, 기록해 두면 나중에 이 기능을
        # 껐을 때 임계값 판정이 "이 문서는 싸다" 고 오판해 38초짜리 문서에
        # 미저장 편집을 적용하고 그 자리에서 멈춘다. 직전 값을 되돌려 둔다.
        for docname in stubbed_docs:
            previous_cost = costs_before_build.get(docname)
            if previous_cost is None:
                read_costs.pop(docname, None)
            else:
                read_costs[docname] = previous_cost
        report["stubbedDoxygen"] = {name: count for name, count in sorted(stubbed_docs.items())}
        if stubbed_docs:
            for docname, count in sorted(stubbed_docs.items()):
                status_stream.write(
                    f"미저장 편집: {docname} 의 doxygen 지시어 {count}개를 생략했습니다"
                    f"(저장하면 온전히 만듭니다).\n")

        _save_json(args.out_dir / READ_COST_NAME, read_costs)
        _save_json(args.out_dir / SHADOWED_NAME, sorted(applied))

        # 지문을 출력 디렉터리에 두던 시절의 파일을 걷어낸다. 아무도 읽지 않으면서
        # 디버깅만 혼란스럽게 한다(이 저장소 기준 200KB).
        try:
            (args.out_dir / LEGACY_INPUTS_NAME).unlink(missing_ok=True)
        except OSError:
            pass

        # 미저장 사본을 적용한 빌드의 입력 지문은 남기지 않는다. 그 빌드의 결과는
        # 디스크의 원본이 아니라 편집 버퍼를 반영한 것이라, 그대로 두면 다음 기동에
        # "바뀐 것 없음" 으로 판정되어 **저장하지 않은 편집이 프리뷰에 굳는다.**
        if args.inputs_file is not None:
            if applied:
                try:
                    args.inputs_file.unlink(missing_ok=True)
                except OSError:
                    pass
            else:
                _save_inputs(args.inputs_file, app, args.conf_dir, args.source_dir)

        # app.config.version 은 "프로젝트" 버전이다. Sphinx 자체 버전이 필요하다.
        import sphinx

        report["sphinxVersion"] = sphinx.__version__
        report["htmlTheme"] = getattr(app.config, "html_theme", "")
        # sources 는 data-mrr-src 인덱스를 되돌리기 위한 **누적** 대응표이고,
        # processedSources 는 이번 빌드가 실제로 건드린 파일이다. 증분 빌드에서는
        # 둘이 다르며, 진단 교체 범위는 후자여야 다른 파일의 진단이 사라지지 않는다.
        report["sources"] = stamper.sources
        report["processedSources"] = stamper.touched

        if args.primary is not None:
            try:
                docname = app.env.path2doc(str(args.primary.resolve()))
            except Exception:  # noqa: BLE001
                docname = None
            if docname:
                report["primaryDocname"] = docname
                candidate = args.out_dir / (docname + ".html")
                if candidate.exists():
                    report["htmlPath"] = str(candidate)

        if not report["htmlPath"]:
            root_doc = getattr(app.config, "root_doc", None) or getattr(app.config, "master_doc", "index")
            candidate = args.out_dir / (str(root_doc) + ".html")
            if candidate.exists():
                report["htmlPath"] = str(candidate)

        report["ok"] = True

    except (ExtensionError, ThemeError, ModuleNotFoundError) as error:
        report["traceback"] = traceback.format_exc()
        warning_stream.write("\n" + str(error) + "\n")
        traceback.print_exc(file=sys.stderr)
        _finish(report, warning_stream, status_stream, report_path)
        return 2
    except Exception:  # noqa: BLE001
        report["traceback"] = traceback.format_exc()
        traceback.print_exc(file=sys.stderr)
        _finish(report, warning_stream, status_stream, report_path)
        return 2

    _finish(report, warning_stream, status_stream, report_path)
    return 0


def _finish(report: dict, warning_stream: io.StringIO, status_stream: io.StringIO,
            report_path: Path) -> None:
    warnings_text = warning_stream.getvalue()
    report["diagnostics"] = _parse_warnings(warnings_text)
    missing_extensions, missing_themes = _collect_missing(
        warnings_text + "\n" + (report.get("traceback") or "")
    )
    report["missingExtensions"] = missing_extensions
    report["missingThemes"] = missing_themes

    # stdout/stderr 은 로그 패널 표시용으로만 흘려보낸다.
    sys.stdout.write(status_stream.getvalue())
    sys.stderr.write(warnings_text)

    _write_report(report_path, report)

    # 여기까지 왔으면 출력 디렉터리는 온전하다. 프로세스가 이 앞에서 죽으면 표식이
    # 남아 다음 빌드가 HTML 을 걷어낸다.
    out_dir = report.get("outDir")
    if out_dir:
        _release_out_dir(Path(out_dir))


if __name__ == "__main__":  # pragma: no cover
    _code = main()

    # 인터프리터를 정상 종료시키면 environment.pickle 에서 펼쳐진 객체 그래프를
    # 파이썬이 전부 해제하느라 **실측 1.8초**가 더 든다(이 저장소 기준 32MB pickle,
    # 빌드 한 번이 4.4초에서 2.6초로 줄어든다). 결과는 _finish() 가 리포트·표식·
    # 스트림까지 전부 디스크에 내려놓은 뒤이고 atexit 훅도 쓰지 않으므로, 버퍼만
    # 비우고 그대로 나간다.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(_code)
