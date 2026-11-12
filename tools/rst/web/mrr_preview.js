// MultiRoot-reST 프리뷰 페이지 스크립트.
//
// 에디터와 프리뷰의 스크롤을 "뷰포트 비율 보존" 방식으로 맞춘다.
// 즉, 어떤 줄이 에디터 창의 세로 50% 위치에 있으면 프리뷰에서도 그 줄에
// 해당하는 내용이 창의 세로 50% 위치에 오게 한다. 단순히 "그 줄로 스크롤"
// 하는 것과 다르다.
//
// 매핑의 근거는 빌더가 심어 둔 속성이다:
//   data-mrr-start-line / data-mrr-end-line : 이 요소가 덮는 원본 줄 범위
//   data-mrr-src                            : 원본 파일 인덱스 (include 대응)
//
// C++ 과는 QWebChannel 객체 `bridge` 로만 통신한다. runJavaScript 로 문자열을
// 조립해 보내지 않는다 (이스케이프 버그의 온상이고 매번 재파싱된다).

(function () {
    "use strict";

    if (window.__mrrPreviewInstalled) {
        return;
    }
    window.__mrrPreviewInstalled = true;

    // 3: previewScrolled 에 "사용자가 만든 스크롤인가" 를 실어 보낸다
    var PROTOCOL_VERSION = 3;
    // 스크롤 위치를 대표하는 기준점. 0.5 = 창의 정중앙.
    var ANCHOR_RATIO = 0.5;
    // C++ 이 우리를 스크롤시킨 직후 우리가 다시 C++ 에 보고하면 무한 왕복이 된다.
    var FEEDBACK_GUARD_MS = 250;

    // 사용자 입력이 만들어 낸 스크롤의 꼬리. 이 시간이 지나도록 새 입력이 없으면
    // 그다음 스크롤은 페이지가 스스로 움직인 것으로 본다.
    //
    // 휠·키 반복·스크롤바 드래그는 스크롤이 이어지는 동안 이벤트도 계속 오므로
    // 이 값이 그 길이를 정하지 않는다. 마지막 입력과 그것이 만든 마지막 scroll
    // 사이의 틈만 덮으면 된다.
    var USER_INTENT_TAIL_MS = 400;

    var bridge = null;
    var suppressUntil = 0;
    var userIntentUntil = 0;
    var scrollTimer = null;
    var cachedRanges = null;
    var cachedAnchors = null;
    var lastViewportWidth = 0;

    function now() {
        return new Date().getTime();
    }

    /// 지금 온 스크롤이 사용자가 만든 것인가.
    ///
    /// **이 판정이 없으면 프리뷰가 편집기를 밀어낸다.** 페이지는 내용이 바뀐 뒤에도
    /// 한참 동안 스스로 스크롤한다 — 이미지가 뒤늦게 도착하고, 폰트가 바뀌고,
    /// KaTeX 와 mermaid 가 자리를 다시 잡고, 그러면서 문서 높이가 계속 달라진다.
    /// 그 스크롤을 C++ 에 올리면 저쪽은 "사용자가 프리뷰를 굴렸다" 로 읽고 편집기를
    /// 그리로 옮긴다. 편집하던 줄이 눈앞에서 사라지는 것이 이것이다.
    ///
    /// 시간으로 가르려던 적이 있다 — 내용을 바꾼 뒤 얼마 동안은 무시하는 방식.
    /// 실측에서 1773줄 문서를 저장했을 때 로드가 끝나고 **32초 뒤까지** 스스로
    /// 움직였고, 그 사이 편집기가 네 번 밀렸다. 무시 구간을 그만큼 늘리면 그동안
    /// 사용자가 굴린 것까지 함께 버리게 된다. 구분의 근거는 시간이 아니라
    /// **직전에 사용자 입력이 있었는가** 다.
    function scrollIsUserDriven() {
        return now() < userIntentUntil;
    }

    function markUserIntent() {
        userIntentUntil = now() + USER_INTENT_TAIL_MS;
    }

    // capture 로 잡는다. 페이지 안의 스크립트가 이벤트를 삼켜도 우리가 먼저 본다.
    ["wheel", "keydown", "mousedown", "pointerdown", "pointermove", "touchstart", "touchmove"]
        .forEach(function (type) {
            window.addEventListener(type, function (event) {
                // 포인터가 그냥 지나가는 것은 입력이 아니다. 단추를 누른 채
                // 움직이는 것만 — 스크롤바를 끌고 있는 중이다.
                if (type === "pointermove" && !event.buttons) {
                    return;
                }
                markUserIntent();
            }, { passive: true, capture: true });
        });

    function invalidateCache() {
        cachedRanges = null;
        cachedAnchors = null;
    }

    /// 창 너비가 바뀌면 자동 줄바꿈 위치가 달라져 모든 기준점 좌표가 무의미해진다.
    function invalidateIfResized() {
        if (window.innerWidth !== lastViewportWidth) {
            lastViewportWidth = window.innerWidth;
            invalidateCache();
        }
    }

    /// 화면에 실제로 자리를 차지하는 범위 요소들을 문서 좌표로 수집한다.
    function ranges() {
        if (cachedRanges) {
            return cachedRanges;
        }
        var out = [];
        var elements = document.querySelectorAll("[data-mrr-start-line]");
        for (var i = 0; i < elements.length; i += 1) {
            var element = elements[i];
            var start = Number(element.getAttribute("data-mrr-start-line"));
            if (!start) {
                continue;
            }
            var end = Number(element.getAttribute("data-mrr-end-line") || start);
            var src = Number(element.getAttribute("data-mrr-src") || 0);
            var rect = element.getBoundingClientRect();
            if (rect.height <= 0) {
                continue;
            }
            out.push({
                src: src,
                start: start,
                end: Math.max(start, end),
                top: rect.top + window.scrollY,
                height: Math.max(1, rect.height)
            });
        }
        cachedRanges = out;
        return out;
    }

    /// 앵커(id="mrr-line-<src>-<line>") 기반 폴백. 범위 요소가 하나도 없을 때 쓴다.
    function anchors() {
        var out = [];
        var elements = document.querySelectorAll('[id^="mrr-line-"]');
        for (var i = 0; i < elements.length; i += 1) {
            var match = String(elements[i].id).match(/^mrr-line-(\d+)-(\d+)$/);
            if (!match) {
                continue;
            }
            out.push({
                src: Number(match[1]),
                line: Number(match[2]),
                y: elements[i].getBoundingClientRect().top + window.scrollY
            });
        }
        out.sort(function (a, b) { return a.line - b.line; });
        return out;
    }

    /// 보간 기준점 목록.
    ///
    /// 여기가 이 파일의 핵심이다. 원본 줄 간격과 렌더된 픽셀 간격은 비례하지
    /// 않는다 — 에디터에서 접히지도 줄바꿈되지도 않은 한 줄이, 좁은 프리뷰
    /// 창에서는 여러 줄로 접혀 몇 배 높이를 차지할 수 있다. 반대로 여러 원본
    /// 줄이 한 문단으로 합쳐져 한 줄 높이만 차지할 수도 있다.
    ///
    /// 그래서 "줄 수에 비례해 픽셀을 나눈다" 는 가정을 쓰지 않고, 실제로 측정된
    /// 기준점 쌍 사이를 구간별 선형 보간한다. 두 기준점 사이의 픽셀 거리는 이미
    /// 줄바꿈이 반영된 실제 렌더 높이이므로, 접힘 정도가 구간마다 달라도 맞는다.
    function anchorTable() {
        if (cachedAnchors) {
            return cachedAnchors;
        }

        var byKey = {};
        var all = ranges();
        for (var i = 0; i < all.length; i += 1) {
            var range = all[i];
            var key = range.src + ":" + range.start;
            var existing = byKey[key];
            // 같은 시작 줄을 가진 요소가 여럿이면 가장 작은 것을 쓴다.
            // (section 같은 큰 요소보다 문단 단위 요소가 훨씬 정확하다.)
            if (!existing || range.height < existing.height) {
                byKey[key] = range;
            }
        }

        var points = [];
        for (var k in byKey) {
            if (Object.prototype.hasOwnProperty.call(byKey, k)) {
                var r = byKey[k];
                points.push({ src: r.src, line: r.start, end: r.end, y: r.top, height: r.height });
            }
        }
        points.sort(function (a, b) { return a.y - b.y || a.line - b.line; });

        // 보간이 흔들리지 않으려면 y 와 line 이 **둘 다** 증가해야 한다.
        //
        // y 가 같은 기준점이 연달아 나오는 경우가 실제로 많다. 예를 들어
        // list-table 한 행의 셀 8개는 소스에서는 8줄이지만 화면에서는 모두 같은
        // 높이에 있다. 이걸 그대로 두면 에디터에서 그 8줄을 지나는 동안 프리뷰가
        // 멈춰 있다가 다음 행에서 갑자기 점프한다.
        //
        // 같은 y 를 가진 묶음에서는 첫 항목만 남긴다. 그러면 그 줄 범위 전체가
        // 다음 기준점까지의 픽셀 구간에 고르게 매핑되어 스크롤이 매끄러워지고,
        // 역변환도 하나의 값으로 확정된다.
        var monotonic = [];
        for (var m = 0; m < points.length; m += 1) {
            var last = monotonic.length ? monotonic[monotonic.length - 1] : null;
            if (last && points[m].src === last.src
                && (points[m].line <= last.line || points[m].y <= last.y)) {
                continue;
            }
            monotonic.push(points[m]);
        }

        // 마지막 요소의 끝을 기준점으로 하나 더 둬서 문서 끝쪽 보간을 가둔다.
        if (monotonic.length) {
            var tail = monotonic[monotonic.length - 1];
            monotonic.push({
                src: tail.src,
                line: Math.max(tail.line + 1, tail.end + 1),
                end: tail.end + 1,
                y: tail.y + tail.height,
                height: 0
            });
        }

        cachedAnchors = monotonic;
        return monotonic;
    }

    /// 원본 줄(소수 가능) -> 문서 Y 좌표.
    function documentYForLine(src, line) {
        var table = anchorTable();
        var mine = [];
        for (var i = 0; i < table.length; i += 1) {
            if (table[i].src === src) {
                mine.push(table[i]);
            }
        }
        if (!mine.length) {
            return null;
        }
        if (line <= mine[0].line) {
            return mine[0].y;
        }
        for (var j = 0; j < mine.length - 1; j += 1) {
            var a = mine[j];
            var b = mine[j + 1];
            if (line >= a.line && line <= b.line) {
                var lineSpan = Math.max(1e-6, b.line - a.line);
                var t = (line - a.line) / lineSpan;
                // a.y ~ b.y 의 픽셀 거리에는 프리뷰 쪽 자동 줄바꿈이 이미 반영돼 있다.
                return a.y + (b.y - a.y) * t;
            }
        }
        return mine[mine.length - 1].y;
    }

    /// 문서 Y 좌표 -> 원본 줄(소수). documentYForLine 의 역함수.
    function lineForDocumentY(y) {
        var table = anchorTable();
        if (!table.length) {
            return null;
        }
        if (y <= table[0].y) {
            return { src: table[0].src, line: table[0].line };
        }
        for (var i = 0; i < table.length - 1; i += 1) {
            var a = table[i];
            var b = table[i + 1];
            if (y >= a.y && y <= b.y) {
                // 두 기준점이 다른 파일에 속하면(include 경계) 보간하지 않고
                // 가까운 쪽으로 붙인다.
                if (a.src !== b.src) {
                    return (y - a.y) <= (b.y - y)
                        ? { src: a.src, line: a.line }
                        : { src: b.src, line: b.line };
                }
                var ySpan = Math.max(1e-6, b.y - a.y);
                var t = (y - a.y) / ySpan;
                return { src: a.src, line: a.line + (b.line - a.line) * t };
            }
        }
        var lastPoint = table[table.length - 1];
        return { src: lastPoint.src, line: lastPoint.line };
    }

    /// 프리뷰를 스크롤해 (src, line) 이 창의 ratio 위치에 오게 한다.
    function scrollToSourceLine(src, line, ratio) {
        invalidateIfResized();

        // 에디터가 문서 맨 위다. 첫 기준점은 테마 헤더나 여백만큼 아래에 있어서
        // 비율 보정을 태우면 0 보다 큰 값이 나오고, 그러면 제목이 화면 위로
        // 잘려 나간다. 문서 시작은 문서 시작으로 맞춘다.
        if (line <= 1) {
            suppressUntil = now() + FEEDBACK_GUARD_MS;
            window.scrollTo(0, 0);
            return;
        }

        var y = documentYForLine(src, line);
        if (y === null) {
            return;
        }
        var target = Math.max(0, y - window.innerHeight * ratio);
        suppressUntil = now() + FEEDBACK_GUARD_MS;
        window.scrollTo(0, target);
    }

    function reportScroll() {
        if (!bridge || now() < suppressUntil) {
            return;
        }
        invalidateIfResized();
        var hit = lineForDocumentY(window.scrollY + window.innerHeight * ANCHOR_RATIO);
        if (hit) {
            // 판정은 여기서 한다. C++ 에는 이 정보가 없다 — 저쪽에서 보이는 것은
            // "프리뷰가 어느 줄에 있다" 뿐이고, 그것이 사용자의 뜻인지 페이지가
            // 스스로 움직인 것인지는 입력 이벤트를 받는 이 창만 안다.
            bridge.previewScrolled(hit.src, hit.line, ANCHOR_RATIO, scrollIsUserDriven());
        }
    }

    window.addEventListener("scroll", function () {
        if (now() < suppressUntil) {
            return;
        }
        if (scrollTimer) {
            window.clearTimeout(scrollTimer);
        }
        scrollTimer = window.setTimeout(reportScroll, 60);
    }, { passive: true });

    // 레이아웃이 바뀌면 캐시한 좌표가 무의미해진다.
    window.addEventListener("resize", invalidateCache);
    if (window.ResizeObserver) {
        var observer = new ResizeObserver(invalidateCache);
        observer.observe(document.documentElement);
    }

    document.addEventListener("click", function (event) {
        if (!bridge) {
            return;
        }
        // 링크 클릭은 원래 동작을 살린다.
        var node = event.target;
        while (node && node !== document.body) {
            if (node.tagName === "A" && node.getAttribute("href")) {
                return;
            }
            node = node.parentNode;
        }
        var hit = lineForDocumentY(event.clientY + window.scrollY);
        if (hit) {
            var viewportRatio = Math.min(0.9, Math.max(0,
                event.clientY / Math.max(1, window.innerHeight)));
            bridge.sourceLocationClicked(hit.src, hit.line, viewportRatio);
        }
    }, true);

    function connectBridge(channel) {
        bridge = channel.objects.bridge;
        if (!bridge) {
            return;
        }
        bridge.scrollToLineRequested.connect(function (src, line, ratio) {
            scrollToSourceLine(src, line, ratio);
        });
        bridge.rebindRequested.connect(function () {
            invalidateCache();
        });
        bridge.scrollFeedbackSuppressed.connect(function (milliseconds) {
            suppressUntil = now() + milliseconds;
        });
        bridge.hotSwapRequested.connect(function (documentHtml, baseUrl, allowMorph, token) {
            hotSwap(documentHtml, baseUrl, allowMorph, token);
        });
        bridge.ready(PROTOCOL_VERSION);
    }

    // ── DOM morph ────────────────────────────────────────────────────────────
    //
    // 새 HTML 로 통째로 갈아 끼우는 대신 **기존 DOM 을 제자리에서 고친다.**
    //
    // innerHTML 대입은 문서의 모든 노드를 버리고 다시 만든다. 그러면 브라우저가
    // 트리 전체의 스타일과 레이아웃을 다시 계산한다 — 본문 2.5MB / 높이 333,000px
    // 짜리 페이지에서 강제 레이아웃만 122.6ms 였고(실측), 그것이 편집 중에 화면이
    // 출렁이는 정체다. 같은 문서를 morph 로 고치면 9ms 이고, 줄 번호만 밀린
    // 경우에는 **0ms** 다 — data-mrr-* 는 어떤 CSS 규칙도 쓰지 않아 값을 바꿔도
    // 레이아웃이 무효화되지 않는다.
    //
    // 노드가 살아남는 것 자체도 이득이다. 코드 블록의 가로 스크롤, <details> 의
    // 펼침, 프리뷰에서 끌어 둔 선택, <iframe> 임베드가 갱신을 견딘다.
    //
    // 결과는 innerHTML 경로와 **같다**. 세 가지 변형(한 글자·줄번호 전체 시프트·
    // 문단 20% 교체)에서 morph 를 거친 DOM 의 innerHTML 이 innerHTML 경로의 결과와
    // 문자열 단위로 일치함을 확인했다. 그래서 실패하면 옛 경로로 되돌아가도 된다.
    //
    // 이 파일에 두는 이유: Sphinx 가 만든 file:// 페이지와 qrc 의 Markdown 셸,
    // **두 프리뷰 모두에 주입되는 유일한 파일**이다. mrr_markdown.js 는 셸에만
    // 있으므로 그쪽은 window.__mrrMorph 로 불러 쓴다.

    /// 짝을 못 찾았을 때 형제를 몇 칸까지 훑을 것인가.
    ///
    /// 문단 하나를 새로 만들면 그 아래 형제가 한 칸씩 밀린다. 앞보기가 없으면
    /// 밀린 자리마다 짝이 어긋나 그림·표가 있는 자리에서 노드 교체까지 간다.
    /// 8은 한 번의 편집이 만드는 블록 증감의 현실적인 상한이다.
    var MORPH_LOOKAHEAD = 8;

    /// morph 한 판마다 올라가는 번호. touched 목록의 중복 제거에 쓴다.
    var morphSession = 0;

    /// 32비트 FNV-1a.
    ///
    /// data-mrr-keep 지문에만 쓴다. 충돌이 나도 서브트리를 한 번 더 그릴 뿐이라
    /// 암호학적 강도가 필요 없다.
    function fingerprint(text) {
        var hash = 0x811c9dc5;
        for (var i = 0; i < text.length; i += 1) {
            hash ^= text.charCodeAt(i);
            // * 16777619 를 시프트로 편다. 곱셈은 53비트를 넘겨 정밀도를 잃는다.
            hash = (hash + ((hash << 1) + (hash << 4) + (hash << 7)
                            + (hash << 8) + (hash << 24))) >>> 0;
        }
        return hash.toString(16);
    }

    /// 순회에서 아예 보이지 않는 노드.
    ///
    /// <script> 는 양쪽 모두에서 건너뛴다. 지금의 innerHTML 대입은 스크립트를
    /// DOM 에 넣기는 하되 **실행하지 않는다**(HTML 명세). morph 가 importNode 로
    /// 넣으면 실행된다 — 본문 스크립트가 편집할 때마다 다시 도는 것은 원하는 바가
    /// 아니다. 그래서 새 것을 넣지 않고 옛 것을 지우지도 않는다.
    function isTransparent(node) {
        return node.nodeType === 1 && node.nodeName === "SCRIPT";
    }

    function compatible(a, b) {
        if (a.nodeType !== b.nodeType) {
            return false;
        }
        if (a.nodeType === 1) {
            return a.nodeName === b.nodeName;
        }
        return true;
    }

    function touch(session, element) {
        if (!element || element.nodeType !== 1) {
            return;
        }
        if (element.__mrrTouched === session.mark) {
            return;
        }
        element.__mrrTouched = session.mark;
        session.touched.push(element);
    }

    /// 새 요소에 있는 속성만 맞춘다. 값이 같으면 **쓰지 않는다** — 같은 class 를
    /// 다시 써도 스타일이 무효화된다.
    function applyAttributes(oldEl, newEl) {
        var attributes = newEl.attributes;
        for (var i = 0; i < attributes.length; i += 1) {
            var attribute = attributes[i];
            if (oldEl.getAttribute(attribute.name) !== attribute.value) {
                oldEl.setAttribute(attribute.name, attribute.value);
            }
        }
    }

    function syncAttributes(oldEl, newEl) {
        applyAttributes(oldEl, newEl);
        var existing = oldEl.attributes;
        for (var i = existing.length - 1; i >= 0; i -= 1) {
            if (!newEl.hasAttribute(existing[i].name)) {
                oldEl.removeAttribute(existing[i].name);
            }
        }
    }

    function morphNode(oldNode, newNode, session) {
        if (oldNode.nodeType !== 1) {
            if (oldNode.nodeValue !== newNode.nodeValue) {
                oldNode.nodeValue = newNode.nodeValue;
                session.changed += 1;
                touch(session, oldNode.parentNode);
            }
            return;
        }

        // 네이티브 깊은 비교가 1차 관문이다. 문서의 대부분은 매 편집에서 그대로
        // 이므로 이 한 줄이 순회를 바뀐 부분에만 가둔다. JS 순회보다 훨씬 싸고
        // 할당이 없다.
        if (oldNode.isEqualNode(newNode)) {
            return;
        }

        // 이 서브트리는 렌더된 뒤 **변형되었다**(mermaid 가 SVG 로 바꿨거나 KaTeX 가
        // 조판했다). 값은 변형 **전** 내용의 지문이다. 들어온 내용이 그때와 같으면
        // 손대지 않는다 — 그래야 타이핑 중에 도형과 수식이 원문으로 되돌아갔다가
        // 다시 나타나는 일이 없다.
        //
        // 지문이 어긋나면 아래 syncAttributes 가 data-mrr-keep 을 지운다(새 요소에는
        // 없으므로). 낡은 지문이 남지 않는다.
        var keep = oldNode.getAttribute("data-mrr-keep");
        if (keep !== null && keep === fingerprint(newNode.innerHTML)) {
            // 속성은 맞추되 **지우지는 않는다.** 렌더러가 붙인 data-processed 를
            // 지우면 다시 그릴 것이 없는데도 다시 그리게 된다.
            applyAttributes(oldNode, newNode);
            return;
        }

        syncAttributes(oldNode, newNode);
        morphChildren(oldNode, newNode, session);
    }

    function morphChildren(oldParent, newParent, session) {
        var oldChild = oldParent.firstChild;
        var newChild = newParent.firstChild;

        while (newChild) {
            var nextNew = newChild.nextSibling;
            if (isTransparent(newChild)) {
                newChild = nextNew;
                continue;
            }
            while (oldChild && isTransparent(oldChild)) {
                oldChild = oldChild.nextSibling;
            }

            if (!oldChild) {
                oldParent.appendChild(document.importNode(newChild, true));
                session.changed += 1;
                touch(session, oldParent);
                newChild = nextNew;
                continue;
            }

            if (compatible(oldChild, newChild)) {
                var nextOld = oldChild.nextSibling;
                morphNode(oldChild, newChild, session);
                oldChild = nextOld;
                newChild = nextNew;
                continue;
            }

            // 옛 노드가 사라진 것인가? 옛 쪽을 앞보기 한다.
            var probe = oldChild;
            var steps = 0;
            var found = null;
            while (probe && steps < MORPH_LOOKAHEAD) {
                if (!isTransparent(probe) && compatible(probe, newChild)) {
                    found = probe;
                    break;
                }
                probe = probe.nextSibling;
                steps += 1;
            }
            if (found) {
                while (oldChild !== found) {
                    var dead = oldChild;
                    oldChild = oldChild.nextSibling;
                    if (!isTransparent(dead)) {
                        oldParent.removeChild(dead);
                        session.changed += 1;
                        touch(session, oldParent);
                    }
                }
                continue;   // 같은 newChild 로 다시 판정한다
            }

            // 새 노드가 끼어든 것인가? 새 쪽을 앞보기 한다.
            probe = newChild;
            steps = 0;
            found = null;
            while (probe && steps < MORPH_LOOKAHEAD) {
                if (!isTransparent(probe) && compatible(oldChild, probe)) {
                    found = probe;
                    break;
                }
                probe = probe.nextSibling;
                steps += 1;
            }
            if (found) {
                var cursor = newChild;
                while (cursor !== found) {
                    var nextCursor = cursor.nextSibling;
                    if (!isTransparent(cursor)) {
                        oldParent.insertBefore(document.importNode(cursor, true), oldChild);
                        session.changed += 1;
                        touch(session, oldParent);
                    }
                    cursor = nextCursor;
                }
                newChild = found;
                continue;
            }

            // 둘 다 아니면 그냥 갈아 끼운다. 앞보기가 빗나가도 결과는 틀리지 않고,
            // 최악의 경우 지금까지 하던 만큼 다시 쓸 뿐이다.
            var replacement = document.importNode(newChild, true);
            var after = oldChild.nextSibling;
            oldParent.replaceChild(replacement, oldChild);
            session.changed += 1;
            touch(session, oldParent);
            oldChild = after;
            newChild = nextNew;
        }

        while (oldChild) {
            var tail = oldChild.nextSibling;
            if (!isTransparent(oldChild)) {
                oldParent.removeChild(oldChild);
                session.changed += 1;
                touch(session, oldParent);
            }
            oldChild = tail;
        }
    }

    /// oldParent 의 자식들을 newParent 의 자식들에 맞춘다.
    ///
    /// 돌려주는 것:
    ///   changed  실제로 고친 횟수. 0 이면 화면이 움직이지 않았다는 뜻이다.
    ///   touched  자식 목록이나 텍스트가 바뀐 **부모 요소**들. 호출자가 조판·
    ///            다이어그램 같은 후처리를 그 범위로만 좁히는 데 쓴다.
    function morph(oldParent, newParent) {
        morphSession += 1;
        var session = { changed: 0, touched: [], mark: morphSession };
        morphChildren(oldParent, newParent, session);
        return session;
    }

    /// 이 서브트리는 렌더 뒤 변형되었다고 표시한다. sourceHtml 은 변형 **전**의
    /// innerHTML 이다. mrr_markdown.js 가 mermaid / KaTeX 뒤에 부른다.
    function stampKeep(element, sourceHtml) {
        element.setAttribute("data-mrr-keep", fingerprint(sourceHtml));
    }

    window.__mrrMorph = morph;
    window.__mrrStampKeep = stampKeep;

    /// 아직 그리지 않은 다이어그램만 그린다.
    ///
    /// 본문을 갱신해도 <head> 의 mermaid 모듈은 살아 있지만, 새로 들어온
    /// `<pre class="mermaid">` 를 그려 주는 쪽은 아무도 없다 (mermaid 는
    /// startOnLoad=false 로 초기화되고 load 이벤트는 다시 오지 않는다).
    /// sphinxcontrib-mermaid 가 window 에 남겨 둔 재실행 훅을 부른다.
    ///
    /// **그 훅은 전부 아니면 전무다.** `runMermaid(true)` 는 문서의 모든
    /// `.mermaid` 를 `data-original-code` 로 되돌린 뒤 다시 그린다. mermaid 모듈은
    /// `type="module"` 안에 있어 `window.mermaid` 가 없으므로 우리가 골라서 그릴
    /// 방법이 없다. 그래서 **이미 그린 것을 그쪽 시야에서 잠깐 숨긴다** — 클래스를
    /// 떼면 그쪽 querySelectorAll 에 잡히지 않는다.
    ///
    /// 숨긴 것을 곧바로 되돌려도 되는 근거: `runMermaid` 는 async 이지만 목록을
    /// 확정하고 `mermaid.run(...)` 을 부르는 데까지가 **동기 구간**이다. 첫 await
    /// 에서 제어가 돌아온 시점에는 그릴 대상이 이미 배열로 잡혀 있다. 이 가정이
    /// 미래의 확장 버전에서 깨지면 결과는 "전부 다시 그린다" 로 되돌아갈 뿐이다.
    function rerenderDiagrams() {
        if (typeof window.runMermaid !== "function") {
            return;
        }

        var all = document.querySelectorAll(".mermaid");
        var undrawn = [];
        var drawn = [];
        for (var i = 0; i < all.length; i += 1) {
            // data-processed 는 mermaid 가, data-mrr-keep 은 우리가 남긴다. 둘 다
            // 있어야 "그려졌고 morph 가 지켜 줄 것" 이 된다.
            if (all[i].getAttribute("data-processed") === "true"
                && all[i].getAttribute("data-mrr-keep") !== null) {
                drawn.push(all[i]);
            } else {
                undrawn.push(all[i]);
            }
        }
        if (!undrawn.length) {
            return;   // 그릴 것이 없다. 산문만 고쳤을 때 여기서 끝난다.
        }

        try {
            for (i = 0; i < drawn.length; i += 1) {
                drawn[i].classList.remove("mermaid");
            }
            var pending;
            try {
                pending = window.runMermaid(true);
            } finally {
                // 예외가 나도 반드시 되돌린다. 클래스를 잃으면 테마 CSS 까지 빠진다.
                for (i = 0; i < drawn.length; i += 1) {
                    drawn[i].classList.add("mermaid");
                }
            }

            var settle = function () {
                stampDrawnDiagrams();
                // 다이어그램이 그려지면 문서 높이가 달라진다. 스크롤 동기화가 쓰는
                // 좌표 캐시는 그 뒤에 버려야 의미가 있다.
                invalidateCache();
            };
            if (pending && typeof pending.then === "function") {
                pending.then(settle, settle);
            } else {
                settle();
            }
        } catch (error) {
            // 다이어그램을 못 그린 것과 본문 교체가 실패한 것은 다른 문제다.
            // 여기서 실패해도 핫스왑 자체는 성공으로 보고한다.
        }
    }

    /// 그려진 다이어그램에 morph 가 알아볼 지문을 남긴다.
    ///
    /// 지문의 원본은 sphinxcontrib-mermaid 가 그리기 **전에** 떠 둔
    /// `data-original-code` 다. 그 값이 곧 HTML 파일에 있던 innerHTML 이므로,
    /// 다음 갱신에서 들어오는 요소의 innerHTML 과 같은 문자열이 된다.
    function stampDrawnDiagrams() {
        var nodes = document.querySelectorAll(".mermaid[data-processed='true']");
        for (var i = 0; i < nodes.length; i += 1) {
            var source = nodes[i].getAttribute("data-original-code");
            if (source === null) {
                continue;   // 그쪽이 원본을 남기지 않았다면 지켜 줄 근거가 없다
            }
            stampKeep(nodes[i], source);
        }
    }

    /// 현재 HTML 파일 URL(쿼리 포함)을 기준으로 상대 경로와 내부 링크를 푼다.
    /// 디렉터리 URL 을 넣으면 #fragment 도 그 디렉터리로 이동하므로 안 된다.
    function updatePreviewBase(baseUrl) {
        if (!baseUrl) {
            return;
        }
        var base = document.querySelector("base[data-mrr-preview-base]");
        if (!base) {
            base = document.createElement("base");
            base.setAttribute("data-mrr-preview-base", "");
            document.head.insertBefore(base, document.head.firstChild);
        }
        if (base.getAttribute("href") !== baseUrl) {
            base.setAttribute("href", baseUrl);
        }
    }

    /// 옛 경로 — 본문을 통째로 갈아 끼운다.
    ///
    /// morph 가 던졌을 때의 그물이다. 결과 DOM 은 morph 와 같으므로 여기로
    /// 떨어져도 화면은 맞다. 다만 레이아웃을 전부 다시 계산하므로 출렁인다.
    function replaceBody(parsedBody) {
        // 교체 도중 문서 높이가 줄면 브라우저가 스크롤을 잘라버린다.
        var previousMinHeight = document.body.style.minHeight;
        document.body.style.minHeight = document.body.scrollHeight + "px";
        document.body.innerHTML = parsedBody.innerHTML;
        document.body.style.minHeight = previousMinHeight;
    }

    /// 전체 리로드 없이 본문만 갱신한다.
    ///
    /// 재빌드마다 페이지를 새로 로드하면 흰 화면이 한 번 깜빡이고 스크롤이
    /// 튄다. <head> 가 그대로일 때만 C++ 이 이 경로를 고르므로, 이미 적용된
    /// 스타일시트는 그대로 두고 본문만 바꾸면 된다.
    ///
    /// 본문을 **갈아 끼우지 않고 고친다**(morph). 근거는 이 파일의 「DOM morph」
    /// 절에 있다. allowMorph 는 C++ 이 preview/domMorph 설정을 실어 보낸 것으로,
    /// 문제가 생긴 사용자가 옛 경로로 되돌릴 수 있게 한다.
    ///
    /// 실패는 세 단계로 물러선다: morph → 본문 통째 교체 → (C++ 이) 전체 리로드.
    function hotSwap(documentHtml, baseUrl, allowMorph, token) {
        try {
            var parsed = new DOMParser().parseFromString(documentHtml, "text/html");
            if (!parsed || !parsed.body) {
                throw new Error("parse failed");
            }

            updatePreviewBase(baseUrl);

            // **morph 보다 먼저** 지문을 남긴다.
            //
            // 페이지를 처음 읽었을 때 도형을 그리는 것은 우리가 아니라 그 페이지의
            // load 처리다(sphinxcontrib-mermaid). 그래서 첫 갱신 시점에는 그려진
            // 도형에 지문이 없고, 그대로 두면 첫 편집에서만 도형이 통째로 다시
            // 그려진다. 여기서 한 번 훑어 주면 그 한 번도 없어진다.
            stampDrawnDiagrams();

            var keepX = window.scrollX;
            var keepY = window.scrollY;

            var session = null;
            if (allowMorph) {
                try {
                    // <body> 자신의 속성은 건드리지 않는다. 지금까지의 innerHTML
                    // 대입도 그랬고, 더 중요한 이유가 있다 — sphinxcontrib-mermaid 가
                    // document.body 의 class/style/data-theme 에 MutationObserver 를
                    // 걸어 두고, 바뀌면 테마가 바뀐 줄 알고 전 도형을 다시 그린다.
                    session = morph(document.body, parsed.body);
                } catch (error) {
                    session = null;   // 아래에서 옛 경로로 떨어진다
                }
            }
            if (!session) {
                replaceBody(parsed.body);
                session = { changed: 1, touched: [] };
            }

            if (session.changed > 0) {
                invalidateCache();
                // morph 경로에서는 문서가 한순간도 비지 않아 스크롤이 잘리지
                // 않는다. 그래도 높이가 실제로 줄어든 경우가 있으므로 어긋났을
                // 때만 되돌린다 — 늘 부르면 사용자가 방금 굴린 것을 되감는다.
                if (window.scrollX !== keepX || window.scrollY !== keepY) {
                    window.scrollTo(keepX, keepY);
                }
                suppressUntil = now() + FEEDBACK_GUARD_MS;
            }

            // changed 와 무관하게 부른다. 그릴 것이 없으면 곧바로 돌아오고, 있으면
            // 그리는 것이 맞다 — 앞선 갱신에서 렌더러가 실패해 남은 도형이 다음
            // 갱신에 그려질 길을 changed 로 막을 이유가 없다.
            rerenderDiagrams();

            bridge.hotSwapResult(token, true, "");
        } catch (error) {
            // 실패하면 C++ 이 전체 리로드로 되돌린다.
            bridge.hotSwapResult(token, false, String(error));
        }
    }

    // 셸 페이지의 렌더러가 나중에(폰트 도착, 다이어그램 완성) 좌표를 흔들 때
    // 캐시를 버릴 통로. 이 파일은 무엇이 흔들었는지 알 필요가 없다.
    window.__mrrInvalidatePreviewCache = invalidateCache;

    // 오른쪽 버튼 메뉴의 "소스에서 이 위치 보기" 가 묻는다: 창 좌표 clientY 에 놓인 원본 줄.
    window.__mrrHitAt = function (clientY) {
        var hit = lineForDocumentY(clientY + window.scrollY);
        return hit ? { src: hit.src, line: hit.line } : null;
    };

    // 매핑 함수 검증용 훅. 스크롤 동기화 정확도는 눈으로 보기 어려워서
    // 왕복 변환(line -> Y -> line)이 일치하는지 자동으로 확인할 수 있게 한다.
    window.__mrrTestHooks = {
        documentYForLine: documentYForLine,
        lineForDocumentY: lineForDocumentY,
        anchorTable: anchorTable,
        ranges: ranges,
        // morph 는 눈으로 보기 어렵다. 결과 동등성(같은 DOM 에 도달하는가)과 노드
        // 보존(같은 객체가 남는가)을 테스트가 직접 확인할 수 있게 연다.
        morph: morph,
        stampKeep: stampKeep,
        fingerprint: fingerprint,
        // 이미 그린 도형을 렌더러 시야에서 숨기는 처리는 남의 스크립트의 모양에
        // 기대고 있다. 가짜 runMermaid 를 세워 그 계약을 붙들어 둔다.
        rerenderDiagrams: rerenderDiagrams
    };

    function boot() {
        if (typeof QWebChannel === "undefined" || !window.qt || !window.qt.webChannelTransport) {
            return;
        }
        new QWebChannel(window.qt.webChannelTransport, connectBridge);
    }

    if (document.readyState === "complete" || document.readyState === "interactive") {
        boot();
    } else {
        document.addEventListener("DOMContentLoaded", boot);
    }
}());
