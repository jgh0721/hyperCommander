// == 이 파일은 Scintilla 상류 소스의 사본이다 ==============================
//
// 기준점: missdeer/scintilla rel-5-6-6 (82f7656) 의
//         qt/ScintillaEditBase/PlatQt.cpp
//
// 왜 갈라졌나: Scintilla 코어에도 등폭 지름길이 있지만(PositionCache.cxx) 그것은
//   세그먼트가 **전부 ASCII 그래픽**일 때만 걸린다. 한글이 한 글자만 섞여도
//   세그먼트 전체가 플랫폼 측정으로 오는데, 한국어 문서에서는 그게 대부분이다.
//   자동 줄바꿈이 기본으로 켜져 있어서 이 비용이 화면 밖 줄까지 번진다.
//
// 무엇을 바꿨나:
//   1. 문자 폭을 전부 아는 텍스트는 QTextLayout 을 만들지 않고 곱셈으로 끝낸다
//      (MonospaceWidths / FillMonospacePositions). 폰트별로 ASCII 폭과 전각 CJK
//      폭을 한 번 실측해 두고, 그 밖의 문자는 폴백 경로에서 배운다(LearnWidths).
//   2. 두 함수의 `if (!font) return;` 이 positions 배열을 안 채우고 반환했다.
//      계약 위반이라 호출자가 초기화되지 않은 값을 읽었다. 0 으로 채운다.
//   3. 호출 횟수 카운터와 켜고 끄는 스위치(PlatQtMetrics.hpp). 지름길이 실제로
//      걸렸는지 확인할 유일한 방법이고 - Scintilla 는 판정 결과를 알려주지 않는다 -
//      테스트가 두 경로의 문자 위치를 견주는 수단이기도 하다.
//
// 시도했다가 걷어낸 것: QTextLine::glyphRuns 로 코드유닛별 위치를 한 번에 받아
//   cursorToX 의 O(n^2) 를 없애는 경로. 실측에서 ASCII 는 13% 빨랐지만 한글은
//   6% 느렸고(폰트 폴백으로 런이 갈려 QList 할당이 늘었다), 무엇보다 결합 문자와
//   soft hyphen 과 폰트 폴백에서 폴백 경로와 다른 위치를 냈다(정확성 테스트가
//   34건 잡았다). 위 1번이 그 자리를 훨씬 크게 메운다.
//
// 상류가 바뀌면: cmake/BuildScintillaLexilla.cmake 의 해시 가드가 구성 시점에
//   FATAL_ERROR 로 멈춘다. 상류 파일과 이 파일을 diff 해서 상류 변경을 옮긴 뒤
//   그 해시를 갱신할 것. 헤더(PlatQt.h)는 복제하지 않았으므로 상류 것을 쓴다 -
//   클래스 선언이 바뀌면 조용한 드리프트가 아니라 컴파일 에러로 드러난다.
// =========================================================================
// @file PlatQt.cpp
//          Copyright (c) 1990-2011, Scientific Toolworks, Inc.
//
// The License.txt file describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//
// Additions Copyright (c) 2011 Archaeopteryx Software, Inc. d/b/a Wingware
// Scintilla platform layer for Qt

#include <cstdio>

#include "PlatQt.h"
#include "Scintilla.h"
#include "XPM.h"
#include "UniConversion.h"
#include "DBCS.h"

// ↓ 우리가 더한 것
#include "PlatQtMetrics.hpp"
#include "PlatQtTextRendering.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QApplication>
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
#include <QScreen>
#endif
#include <QFont>
#include <QColor>
#include <QRect>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QWidget>
#include <QWindow>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QMenu>
#include <QAction>
#include <QTime>
#include <QMessageBox>
#include <QTextCodec>
#include <QListWidget>
#include <QVarLengthArray>
#include <QScrollBar>
#include <QTextLayout>
#include <QTextLine>
#include <QLibrary>
#include <QtMath>

using namespace Scintilla;

namespace Scintilla::Internal {

//----------------------------------------------------------------------

// Convert from a Scintilla characterSet value to a Qt codec name.
const char *CharacterSetID(CharacterSet characterSet)
{
	switch (characterSet) {
		//case CharacterSet::Ansi:
		//	return "";
	case CharacterSet::Default:
		return "ISO 8859-1";
	case CharacterSet::Baltic:
		return "ISO 8859-13";
	case CharacterSet::ChineseBig5:
		return "Big5";
	case CharacterSet::EastEurope:
		return "ISO 8859-2";
	case CharacterSet::GB2312:
		return "GB18030-0";
	case CharacterSet::Greek:
		return "ISO 8859-7";
	case CharacterSet::Hangul:
		return "CP949";
	case CharacterSet::Mac:
		return "Apple Roman";
		//case SC_CHARSET_OEM:
		//	return "ASCII";
	case CharacterSet::Russian:
		return "KOI8-R";
	case CharacterSet::Cyrillic:
		return "Windows-1251";
	case CharacterSet::ShiftJis:
		return "Shift-JIS";
		//case SC_CHARSET_SYMBOL:
		//        return "";
	case CharacterSet::Turkish:
		return "ISO 8859-9";
		//case SC_CHARSET_JOHAB:
		//        return "CP1361";
	case CharacterSet::Hebrew:
		return "ISO 8859-8";
	case CharacterSet::Arabic:
		return "ISO 8859-6";
	case CharacterSet::Vietnamese:
		return "Windows-1258";
	case CharacterSet::Thai:
		return "TIS-620";
	case CharacterSet::Iso8859_15:
		return "ISO 8859-15";
	default:
		return "ISO 8859-1";
	}
}

QString UnicodeFromText(QTextCodec *codec, std::string_view text) {
	return codec->toUnicode(text.data(), static_cast<int>(text.length()));
}

static QFont::StyleStrategy ChooseStrategy(FontQuality eff)
{
	switch (eff) {
		case FontQuality::QualityDefault:         return QFont::PreferDefault;
		case FontQuality::QualityNonAntialiased: return QFont::NoAntialias;
		case FontQuality::QualityAntialiased:     return QFont::PreferAntialias;
		case FontQuality::QualityLcdOptimized:   return QFont::PreferAntialias;
		default:                             return QFont::PreferDefault;
	}
}

static QFont::Stretch QStretchFromFontStretch(Scintilla::FontStretch stretch)
{
	switch (stretch) {
	case FontStretch::UltraCondensed:
		return QFont::Stretch::UltraCondensed;
	case FontStretch::ExtraCondensed:
		return QFont::Stretch::ExtraCondensed;
	case FontStretch::Condensed:
		return QFont::Stretch::Condensed;
	case FontStretch::SemiCondensed:
		return QFont::Stretch::SemiCondensed;
	case FontStretch::Normal:
		return QFont::Stretch::Unstretched;
	case FontStretch::SemiExpanded:
		return QFont::Stretch::SemiExpanded;
	case FontStretch::Expanded:
		return QFont::Stretch::Expanded;
	case FontStretch::ExtraExpanded:
		return QFont::Stretch::ExtraExpanded;
	case FontStretch::UltraExpanded:
		return QFont::Stretch::UltraExpanded;
	default:
		return QFont::Stretch::Unstretched;
	}
}

// 우리가 더한 것: ASCII / 전각 CJK 폭 캐시.
//
// Scintilla 코어에도 등폭 지름길이 있지만(PositionCache.cxx) 그것은 세그먼트가
// **전부 ASCII 그래픽**일 때만 걸린다. 한글이 한 글자만 섞여도 세그먼트 전체가
// 플랫폼 측정으로 온다. 한국어 문서에서는 그게 대부분이다.
//
// 그런데 실측해 보면 한글 음절·호환 자모·한자는 어느 폰트에서든 폭이 정확히
// 같다. 한글 글리프가 없어 폰트 폴백이 일어나는 Consolas 에서도 그렇다(폴백
// 폰트가 하나로 고정되므로). 다만 ASCII 폭의 정수배는 아니다 — Consolas 1.82,
// Cascadia Code 1.71, 나눔고딕코딩만 정확히 2.0 이다. 그래서 폭을 둘 다 잰다.
//
// 넓히지 않은 범위와 그 이유(11pt 실측 편차):
//   한글 자모 U+1100  결합이 일어나 폭 0 이 섞인다 (편차 1275)
//   CJK 기호 U+3000   전각 공백 등에서 폰트마다 갈린다 (편차 85)
//   가나 U+3040/30A0  폰트마다 반각/전각이 갈린다
//   전각 기호 U+FF01  Consolas·나눔고딕코딩에서 반각으로 나온다
struct MonospaceWidths {
	bool probed = false;
	bool usable = false;
	XYPOSITION ascii = 0;
	XYPOSITION cjk = 0;

	// 프로브 범위 밖 문자의 폭을 폴백 경로에서 배워 둔다.
	//
	// 한국어 문서에는 초성+한자키로 넣는 기호가 흔하다 — ※ ○ → ℃ 「」 ‘’ … ―
	// ± × ÷ ≠ ㎡ 같은 것들. 이들은 동아시아 폭이 Ambiguous 라 폰트마다 반각과
	// 전각이 갈리므로 정적 범위로 묶을 수 없다(실측: 화살표 U+2190 은 Consolas
	// 에서 편차 10.09). 그런데 **한 폰트 안에서는** 값이 정해져 있다.
	//
	// 폴백 경로는 어차피 QTextLayout 을 만들어 문자별 위치를 다 계산한다. 거기서
	// 폭을 주워 담아 두면 다음번 같은 문자는 지름길을 탄다. 학습 자체는 공짜다.
	//
	// 값이 음수면 "문맥마다 달라서 못 믿는다" 는 표시다. 폭 0 인 문자(결합 마크
	// 따위)도 여기 넣는다 — 앞 글자에 붙는지 여부가 문맥에 달렸기 때문이다.
	QHash<unsigned int, XYPOSITION> learned;
};

class FontAndCharacterSet : public Font {
public:
	CharacterSet characterSet = CharacterSet::Ansi;
	std::unique_ptr<QFont> pfont;
	// const Font* 로만 접근되므로 mutable 이다. ViewStyle::Refresh 가 폰트를
	// 통째로 다시 만들기 때문에(fonts.clear()) 무효화는 저절로 된다.
	mutable MonospaceWidths widths;
	explicit FontAndCharacterSet(const FontParameters &fp) : characterSet(fp.characterSet) {
		pfont = std::make_unique<QFont>();
		pfont->setStyleStrategy(ChooseStrategy(fp.extraFontFlag));
		pfont->setFamily(QString::fromUtf8(fp.faceName));
		pfont->setPointSizeF(fp.size);
		pfont->setBold(static_cast<int>(fp.weight) > 500);
		pfont->setStretch(QStretchFromFontStretch(fp.stretch));
		pfont->setItalic(fp.italic);
	}
};

namespace {

const Supports SupportsQt[] = {
	Supports::LineDrawsFinal,
	Supports::FractionalStrokeWidth,
	Supports::TranslucentStroke,
	Supports::PixelModification,
};

const FontAndCharacterSet *AsFontAndCharacterSet(const Font *f) {
	return dynamic_cast<const FontAndCharacterSet *>(f);
}

QFont *FontPointer(const Font *f)
{
	return AsFontAndCharacterSet(f)->pfont.get();
}

}

std::shared_ptr<Font> Font::Allocate(const FontParameters &fp)
{
	return std::make_shared<FontAndCharacterSet>(fp);
}

SurfaceImpl::SurfaceImpl() = default;

SurfaceImpl::SurfaceImpl(int width, int height, SurfaceMode mode_, qreal scale_)
{
	if (width < 1) width = 1;
	if (height < 1) height = 1;
	deviceOwned = true;
	device = new QPixmap(width, height);
	mode = mode_;
	scale = scale_;
}

SurfaceImpl::~SurfaceImpl()
{
	Clear();
}

void SurfaceImpl::Clear()
{
	if (painterOwned && painter) {
		delete painter;
	}

	if (deviceOwned && device) {
		delete device;
	}
	device = nullptr;
	painter = nullptr;
	deviceOwned = false;
	painterOwned = false;
}

double ScaleOfWindow(WindowID wid)
{
	const QWidget *widget = window(wid);
	if (!widget) {
		return 1.0;
	}
	const QVariant variant = widget->property("ScintillaScale");
	return variant.toDouble();
}

// Define SCINTILLA_QT_BACKINGSTORE_FIXED when building against a Qt whose
// backing store tracks dirty state at device-pixel resolution; that makes the
// fractional-scaling workarounds below compile out entirely.

#if defined(Q_OS_APPLE) && !defined(SCINTILLA_QT_BACKINGSTORE_FIXED)
// Grow a logical update rect so its coordinates translate into even device
// pixels w/o rounding. The coordinates need to be even because there's
// a dirty grid that's 1/2 the size of the device pixel grid on Retina
// displays. This may grow the rect more than needed on non-Retina displays
static QRect SnapToDevicePixelGrid(QRect upd, qreal scale, QSize widgetSize)
{
	const int backing = (scale >= 2.0) ? 2 : 1;
	if (backing == 1)
		return upd;   // grid is already device-pixel resolution; nothing to snap

	// q logical pixels span q * (scale/backing) grid cells.  The smallest q
	// that makes this whole is the snap period whose multiples put logical
	// edges on the device-pixel grid.  (scale/backing is the fractional
	// QT_SCALE_FACTOR, e.g. 1.5 -> q == 2, 1.25 -> q == 4.)
	const double zoom = scale / backing;
	int q = 0;
	for (int n = 1; n <= 8; ++n) {
		if (std::fabs(zoom * n - std::round(zoom * n)) < 1e-4) {
			q = n;
			break;
		}
	}
	if (q == 0)
		return QRect();   // not a simple ratio
	if (q == 1)
		return upd;       // already on the grid

	auto floorTo = [](int v, int n) { int r = v % n; if (r < 0) r += n; return v - r; };
	const int l = floorTo(upd.left(),   q);
	const int t = floorTo(upd.top(),    q);
	const int r = floorTo(upd.right()  + q, q) - 1;   // ceil the exclusive edge
	const int b = floorTo(upd.bottom() + q, q) - 1;
	const QRect snapped(QPoint(l, t), QPoint(r, b));
	if (snapped.width() >= widgetSize.width() && snapped.height() >= widgetSize.height())
		return QRect();   // would cover the whole widget
	return snapped;
}
#endif

double ScaleToMultiply(WindowID wid)
{
	const qreal scale = ScaleOfWindow(wid);
	return scale ? scale : 1.0;
}

void SurfaceImpl::Init(WindowID wid)
{
	Release();
	device = window(wid);
	scale = ScaleOfWindow(wid);
}

void SurfaceImpl::Init(SurfaceID sid, WindowID /*wid*/)
{
	Release();
	device = static_cast<QPaintDevice *>(sid);
	scale = 0.0;
}

std::unique_ptr<Surface> SurfaceImpl::AllocatePixMap(int width, int height)
{
	return std::make_unique<SurfaceImpl>(width, height, mode, scale);
}

std::unique_ptr<Surface> SurfaceImpl_AllocatePixMap(int width, int height, SurfaceMode mode, qreal scale)
{
	return std::make_unique<SurfaceImpl>(width, height, mode, scale);
}

void SurfaceImpl::SetMode(SurfaceMode mode_)
{
	mode = mode_;
}

void SurfaceImpl::Release() noexcept
{
	Clear();
}

int SurfaceImpl::SupportsFeature(Supports feature) noexcept
{
	for (const Supports f : SupportsQt) {
		if (f == feature)
			return 1;
	}
	return 0;
}

bool SurfaceImpl::Initialised()
{
	return device != nullptr;
}

void SurfaceImpl::PenColour(ColourRGBA fore)
{
	QPen penOutline(QColorFromColourRGBA(fore));
	penOutline.setCapStyle(Qt::FlatCap);
	GetPainter()->setPen(penOutline);
}

void SurfaceImpl::PenColourWidth(ColourRGBA fore, XYPOSITION strokeWidth) {
	QPen penOutline(QColorFromColourRGBA(fore));
	penOutline.setCapStyle(Qt::FlatCap);
	penOutline.setJoinStyle(Qt::MiterJoin);
	penOutline.setWidthF(strokeWidth);
	GetPainter()->setPen(penOutline);
}

void SurfaceImpl::BrushColour(ColourRGBA back)
{
	GetPainter()->setBrush(QBrush(QColorFromColourRGBA(back)));
}

void SurfaceImpl::SetCodec(const Font *font)
{
	const FontAndCharacterSet *pfacs = AsFontAndCharacterSet(font);
	if (pfacs && pfacs->pfont) {
		const char *csid = "UTF-8";
		if (!(mode.codePage == SC_CP_UTF8))
			csid = CharacterSetID(pfacs->characterSet);
		if (csid != codecName) {
			codecName = csid;
			codec = QTextCodec::codecForName(csid);
		}
	}
}

void SurfaceImpl::SetFont(const Font *font)
{
	const FontAndCharacterSet *pfacs = AsFontAndCharacterSet(font);
	if (pfacs && pfacs->pfont) {
		GetPainter()->setFont(*(pfacs->pfont));
		SetCodec(font);
	}
}

int SurfaceImpl::LogPixelsY()
{
	return device->logicalDpiY();
}

int SurfaceImpl::PixelDivisions()
{
	// Qt uses device pixels.
	return 1;
}

int SurfaceImpl::DeviceHeightFont(int points)
{
	if (scale) {
		return points * scale;
	} else {
		return points;
	}
}

void SurfaceImpl::LineDraw(Point start, Point end, Stroke stroke)
{
	PenColourWidth(stroke.colour, stroke.width);
	QLineF line(start.x, start.y, end.x, end.y);
	GetPainter()->drawLine(line);
}

void SurfaceImpl::PolyLine(const Point *pts, size_t npts, Stroke stroke)
{
	// TODO: set line joins and caps
	PenColourWidth(stroke.colour, stroke.width);
	std::vector<QPointF> qpts;
	std::transform(pts, pts + npts, std::back_inserter(qpts), QPointFFromPoint);
	GetPainter()->drawPolyline(&qpts[0], static_cast<int>(npts));
}

void SurfaceImpl::Polygon(const Point *pts, size_t npts, FillStroke fillStroke)
{
	PenColourWidth(fillStroke.stroke.colour, fillStroke.stroke.width);
	BrushColour(fillStroke.fill.colour);

	std::vector<QPointF> qpts;
	std::transform(pts, pts + npts, std::back_inserter(qpts), QPointFFromPoint);

	GetPainter()->drawPolygon(&qpts[0], static_cast<int>(npts));
}

void SurfaceImpl::RectangleDraw(PRectangle rc, FillStroke fillStroke)
{
	PenColourWidth(fillStroke.stroke.colour, fillStroke.stroke.width);
	BrushColour(fillStroke.fill.colour);
	const QRectF rect = QRectFFromPRect(rc.Inset(fillStroke.stroke.width / 2));
	GetPainter()->drawRect(rect);
}

void SurfaceImpl::RectangleFrame(PRectangle rc, Stroke stroke) {
	PenColourWidth(stroke.colour, stroke.width);
	// Default QBrush is Qt::NoBrush so does not fill
	GetPainter()->setBrush(QBrush());
	const QRectF rect = QRectFFromPRect(rc.Inset(stroke.width / 2));
	GetPainter()->drawRect(rect);
}

void SurfaceImpl::FillRectangle(PRectangle rc, Fill fill)
{
	GetPainter()->fillRect(QRectFFromPRect(rc), QColorFromColourRGBA(fill.colour));
}

void SurfaceImpl::FillRectangleAligned(PRectangle rc, Fill fill)
{
	FillRectangle(PixelAlign(rc, 1), fill);
}

void SurfaceImpl::FillRectangle(PRectangle rc, Surface &surfacePattern)
{
	// Tile pattern over rectangle
	SurfaceImpl *surface = dynamic_cast<SurfaceImpl *>(&surfacePattern);
	const QPixmap *pixmap = static_cast<QPixmap *>(surface->GetPaintDevice());
	GetPainter()->drawTiledPixmap(QRectFromPRect(rc), *pixmap);
}

void SurfaceImpl::RoundedRectangle(PRectangle rc, FillStroke fillStroke)
{
	PenColourWidth(fillStroke.stroke.colour, fillStroke.stroke.width);
	BrushColour(fillStroke.fill.colour);
	GetPainter()->drawRoundedRect(QRectFFromPRect(rc), 3.0f, 3.0f);
}

void SurfaceImpl::AlphaRectangle(PRectangle rc, XYPOSITION cornerSize, FillStroke fillStroke)
{
	QColor qFill = QColorFromColourRGBA(fillStroke.fill.colour);
	QBrush brushFill(qFill);
	GetPainter()->setBrush(brushFill);
	if (fillStroke.fill.colour == fillStroke.stroke.colour) {
		painter->setPen(Qt::NoPen);
		QRectF rect = QRectFFromPRect(rc);
		if (cornerSize > 0.0f) {
			// A radius of 1 shows no curve so add 1
			qreal radius = cornerSize+1;
			GetPainter()->drawRoundedRect(rect, radius, radius);
		} else {
			GetPainter()->fillRect(rect, brushFill);
		}
	} else {
		QColor qOutline = QColorFromColourRGBA(fillStroke.stroke.colour);
		QPen penOutline(qOutline);
		penOutline.setWidthF(fillStroke.stroke.width);
		GetPainter()->setPen(penOutline);

		QRectF rect = QRectFFromPRect(rc.Inset(fillStroke.stroke.width / 2));
		if (cornerSize > 0.0f) {
			// A radius of 1 shows no curve so add 1
			qreal radius = cornerSize+1;
			GetPainter()->drawRoundedRect(rect, radius, radius);
		} else {
			GetPainter()->drawRect(rect);
		}
	}
}

void SurfaceImpl::GradientRectangle(PRectangle rc, const std::vector<ColourStop> &stops, GradientOptions options) {
	QRectF rect = QRectFFromPRect(rc);
	QLinearGradient linearGradient;
	switch (options) {
	case GradientOptions::leftToRight:
		linearGradient = QLinearGradient(rc.left, rc.top, rc.right, rc.top);
		break;
	case GradientOptions::topToBottom:
	default:
		linearGradient = QLinearGradient(rc.left, rc.top, rc.left, rc.bottom);
		break;
	}
	linearGradient.setSpread(QGradient::RepeatSpread);
	for (const ColourStop &stop : stops) {
		linearGradient.setColorAt(stop.position, QColorFromColourRGBA(stop.colour));
	}
	QBrush brush = QBrush(linearGradient);
	GetPainter()->fillRect(rect, brush);
}

static std::vector<unsigned char> ImageByteSwapped(int width, int height, const unsigned char *pixelsImage)
{
	// Input is RGBA, but Format_ARGB32 is BGRA, so swap the red bytes and blue bytes
	size_t bytes = width * height * 4;
	std::vector<unsigned char> imageBytes(pixelsImage, pixelsImage+bytes);
	for (size_t i=0; i<bytes; i+=4)
		std::swap(imageBytes[i], imageBytes[i+2]);
	return imageBytes;
}

void SurfaceImpl::DrawRGBAImage(PRectangle rc, int width, int height, const unsigned char *pixelsImage)
{
	std::vector<unsigned char> imageBytes = ImageByteSwapped(width, height, pixelsImage);
	QImage image(&imageBytes[0], width, height, QImage::Format_ARGB32);
	QPoint pt(rc.left, rc.top);
	GetPainter()->drawImage(pt, image);
}

void SurfaceImpl::Ellipse(PRectangle rc, FillStroke fillStroke)
{
	PenColourWidth(fillStroke.stroke.colour, fillStroke.stroke.width);
	BrushColour(fillStroke.fill.colour);
	const QRectF rect = QRectFFromPRect(rc.Inset(fillStroke.stroke.width / 2));
	GetPainter()->drawEllipse(rect);
}

void SurfaceImpl::Stadium(PRectangle rc, FillStroke fillStroke, Ends ends) {
	const XYPOSITION halfStroke = fillStroke.stroke.width / 2.0f;
	const XYPOSITION radius = rc.Height() / 2.0f - halfStroke;
	PRectangle rcInner = rc;
	rcInner.left += radius;
	rcInner.right -= radius;
	const XYPOSITION arcHeight = rc.Height() - fillStroke.stroke.width;

	PenColourWidth(fillStroke.stroke.colour, fillStroke.stroke.width);
	BrushColour(fillStroke.fill.colour);

	QPainterPath path;

	const Ends leftSide = static_cast<Ends>(static_cast<unsigned int>(ends) & 0xfu);
	const Ends rightSide = static_cast<Ends>(static_cast<unsigned int>(ends) & 0xf0u);
	switch (leftSide) {
		case Ends::leftFlat:
			path.moveTo(rc.left + halfStroke, rc.top + halfStroke);
			path.lineTo(rc.left + halfStroke, rc.bottom - halfStroke);
			break;
		case Ends::leftAngle:
			path.moveTo(rcInner.left + halfStroke, rc.top + halfStroke);
			path.lineTo(rc.left + halfStroke, rc.Centre().y);
			path.lineTo(rcInner.left + halfStroke, rc.bottom - halfStroke);
			break;
		case Ends::semiCircles:
		default:
			path.moveTo(rcInner.left + halfStroke, rc.top + halfStroke);
			QRectF rectangleArc(rc.left + halfStroke, rc.top + halfStroke,
					    arcHeight, arcHeight);
			path.arcTo(rectangleArc, 90, 180);
			break;
	}

	switch (rightSide) {
		case Ends::rightFlat:
			path.lineTo(rc.right - halfStroke, rc.bottom - halfStroke);
			path.lineTo(rc.right - halfStroke, rc.top + halfStroke);
			break;
		case Ends::rightAngle:
			path.lineTo(rcInner.right - halfStroke, rc.bottom - halfStroke);
			path.lineTo(rc.right - halfStroke, rc.Centre().y);
			path.lineTo(rcInner.right - halfStroke, rc.top + halfStroke);
			break;
		case Ends::semiCircles:
		default:
			path.lineTo(rcInner.right - halfStroke, rc.bottom - halfStroke);
			QRectF rectangleArc(rc.right - arcHeight - halfStroke, rc.top + halfStroke,
					    arcHeight, arcHeight);
			path.arcTo(rectangleArc, 270, 180);
			break;
	}

	// Close the path to enclose it for stroking and for filling, then draw it
	path.closeSubpath();
	GetPainter()->drawPath(path);
}

void SurfaceImpl::Copy(PRectangle rc, Point from, Surface &surfaceSource)
{
	SurfaceImpl *source = dynamic_cast<SurfaceImpl *>(&surfaceSource);
	QPixmap *pixmap = static_cast<QPixmap *>(source->GetPaintDevice());

	GetPainter()->drawPixmap(rc.left, rc.top, *pixmap, from.x, from.y, -1, -1);
}

std::unique_ptr<IScreenLineLayout> SurfaceImpl::Layout(const IScreenLine *)
{
	return {};
}

void SurfaceImpl::DrawTextNoClip(PRectangle rc,
				 const Font *font,
                                 XYPOSITION ybase,
				 std::string_view text,
				 ColourRGBA fore,
				 ColourRGBA back)
{
	SetFont(font);
	PenColour(fore);

	GetPainter()->setBackground(QColorFromColourRGBA(back));
	GetPainter()->setBackgroundMode(Qt::OpaqueMode);
	QString su = UnicodeFromText(codec, text);
	mrst::scintilla::drawTextWithMask(*GetPainter(), QPointF(rc.left, ybase), su);
}

void SurfaceImpl::DrawTextClipped(PRectangle rc,
				  const Font *font,
                                  XYPOSITION ybase,
				  std::string_view text,
				  ColourRGBA fore,
				  ColourRGBA back)
{
	SetClip(rc);
	DrawTextNoClip(rc, font, ybase, text, fore, back);
	PopClip();
}

void SurfaceImpl::DrawTextTransparent(PRectangle rc,
				      const Font *font,
                                      XYPOSITION ybase,
				      std::string_view text,
	ColourRGBA fore)
{
	SetFont(font);
	PenColour(fore);

	GetPainter()->setBackgroundMode(Qt::TransparentMode);
	QString su = UnicodeFromText(codec, text);
	mrst::scintilla::drawTextWithMask(*GetPainter(), QPointF(rc.left, ybase), su);
}

void SurfaceImpl::SetClip(PRectangle rc)
{
	GetPainter()->save();
	GetPainter()->setClipRect(QRectFFromPRect(rc), Qt::IntersectClip);
}

void SurfaceImpl::PopClip()
{
	GetPainter()->restore();
}


// ── 우리가 더한 것: ASCII + 전각 CJK 만으로 된 텍스트는 곱셈으로 끝낸다 ──
//
// 실측한 안전 범위. 이 밖은 폰트마다 폭이 갈려서 넣을 수 없다(위 MonospaceWidths
// 주석에 범위별 편차를 적어 두었다).
namespace {

constexpr bool IsFullWidthCJK(unsigned int cp) noexcept
{
	return (cp >= 0xAC00 && cp <= 0xD7A3)    // 한글 음절
	    || (cp >= 0x3130 && cp <= 0x318E)    // 호환 자모
	    || (cp >= 0x4E00 && cp <= 0x9FA5);   // 한자
}

/// 문자별 advance 가 전부 같으면 그 폭을 돌려준다. 문턱은 Scintilla 의
/// ViewStyle 이 등폭을 판정할 때 쓰는 것과 같다(최소폭의 1e-6).
bool UniformAdvance(const QTextLine &tl, int n, XYPOSITION &widthOut)
{
	if (n <= 0)
		return false;
	qreal minW = 0.0;
	qreal maxW = 0.0;
	qreal prev = 0.0;
	for (int i = 0; i < n; i++) {
		const qreal x = tl.cursorToX(i + 1);
		const qreal w = x - prev;
		prev = x;
		if (i == 0) {
			minW = w;
			maxW = w;
		} else {
			minW = std::min(minW, w);
			maxW = std::max(maxW, w);
		}
	}
	if (minW <= 0.0 || (maxW - minW) >= minW * 1e-6)
		return false;
	widthOut = static_cast<XYPOSITION>(minW);
	return true;
}

/// 폰트당 한 번만 잰다. ViewStyle::Refresh 가 폰트를 다시 만들면 캐시도 함께
/// 사라지므로 무효화 로직이 따로 필요 없다.
MonospaceWidths &MonospaceWidthsFor(const Font *font, QPaintDevice *device)
{
	const FontAndCharacterSet *pfcs = AsFontAndCharacterSet(font);
	MonospaceWidths &w = pfcs->widths;
	if (w.probed)
		return w;
	w.probed = true;

	const auto measure = [&](const QString &sample, XYPOSITION &out) {
		QTextLayout tlay(sample, *pfcs->pfont, device);
		tlay.beginLayout();
		const QTextLine tl = tlay.createLine();
		tlay.endLayout();
		return UniformAdvance(tl, sample.size(), out);
	};

	// "Ay" 는 커닝이, "fi" 는 리가처가 있는 폰트를 걸러내려는 것이다
	// (Scintilla 의 ViewStyle 프로브와 같은 수법).
	QString ascii = QStringLiteral("Ayfi");
	for (int c = 0x20; c <= 0x7E; c++)
		ascii.append(QChar(c));
	// 세 범위에서 고르게 뽑는다. 한쪽에 몰리면 폰트 폴백이 범위마다 다를 때
	// 그것을 놓친다.
	QString cjk;
	for (int cp = 0xAC00; cp <= 0xD7A3; cp += 397)
		cjk.append(QChar(cp));
	for (int cp = 0x3131; cp <= 0x318E; cp += 11)
		cjk.append(QChar(cp));
	for (int cp = 0x4E00; cp <= 0x9FA5; cp += 599)
		cjk.append(QChar(cp));

	XYPOSITION asciiWidth = 0;
	XYPOSITION cjkWidth = 0;
	if (measure(ascii, asciiWidth) && measure(cjk, cjkWidth)) {
		w.usable = true;
		w.ascii = asciiWidth;
		w.cjk = cjkWidth;
	}
	return w;
}

unsigned int CodePointFromUTF8(const char *s, unsigned int bytes) noexcept
{
	const unsigned char b0 = static_cast<unsigned char>(s[0]);
	switch (bytes) {
	case 1:
		return b0;
	case 2:
		return ((b0 & 0x1Fu) << 6) | (static_cast<unsigned char>(s[1]) & 0x3Fu);
	case 3:
		return ((b0 & 0x0Fu) << 12) |
		       ((static_cast<unsigned char>(s[1]) & 0x3Fu) << 6) |
		       (static_cast<unsigned char>(s[2]) & 0x3Fu);
	case 4:
		return ((b0 & 0x07u) << 18) |
		       ((static_cast<unsigned char>(s[1]) & 0x3Fu) << 12) |
		       ((static_cast<unsigned char>(s[2]) & 0x3Fu) << 6) |
		       (static_cast<unsigned char>(s[3]) & 0x3Fu);
	default:
		return 0;
	}
}

/// UTF-8 텍스트의 모든 문자 폭을 아는 경우에만 positions 를 채우고 true.
/// 하나라도 모르면 false — 그때는 호출자가 원래 경로로 간다 (부분적으로 쓴
/// 값은 그 경로가 덮어쓴다).
bool FillMonospacePositions(std::string_view text, XYPOSITION *positions,
			    const MonospaceWidths &w)
{
	XYPOSITION x = 0;
	size_t i = 0;
	const size_t len = text.length();
	while (i < len) {
		const unsigned char b0 = static_cast<unsigned char>(text[i]);
		if (b0 >= 0x20 && b0 <= 0x7E) {
			x += w.ascii;
			positions[i++] = x;
			continue;
		}
		const unsigned int bytes = UTF8BytesOfLead[b0];
		if (bytes < 2 || (i + bytes) > len)
			return false;
		const unsigned int cp = CodePointFromUTF8(&text[i], bytes);
		XYPOSITION width = 0;
		if (bytes == 3 && IsFullWidthCJK(cp)) {
			width = w.cjk;
		} else {
			const auto found = w.learned.constFind(cp);
			if (found == w.learned.constEnd() || *found < 0)
				return false;
			width = *found;
		}
		x += width;
		// 계약: 한 문자의 모든 바이트가 그 문자의 오른쪽 끝을 갖는다.
		for (unsigned int b = 0; b < bytes; b++)
			positions[i++] = x;
	}
	return true;
}

/// 폴백 경로가 계산해 둔 positions 에서 문자 폭을 주워 담는다.
///
/// 이미 값이 있고 다르면 음수로 덮어 "못 믿는다" 고 표시한다. 같은 문자가
/// 문맥에 따라 다른 폭을 갖는다는 뜻이고(커닝 따위), 그런 문자는 지름길로
/// 처리하면 안 된다.
void LearnWidths(std::string_view text, const XYPOSITION *positions, MonospaceWidths &w)
{
	XYPOSITION prev = 0;
	size_t i = 0;
	const size_t len = text.length();
	while (i < len) {
		const unsigned char b0 = static_cast<unsigned char>(text[i]);
		const unsigned int bytes = UTF8BytesOfLead[b0];
		if (bytes == 0 || (i + bytes) > len)
			return;
		const XYPOSITION x = positions[i + bytes - 1];
		const XYPOSITION width = x - prev;
		prev = x;
		i += bytes;

		// ASCII 와 전각 CJK 는 프로브에서 이미 안다. 서로게이트(4바이트)는
		// ZWJ 시퀀스처럼 이웃에 따라 합쳐지므로 배우지 않는다.
		if (bytes < 2 || bytes > 3)
			continue;
		const unsigned int cp = CodePointFromUTF8(&text[i - bytes], bytes);
		if (bytes == 3 && IsFullWidthCJK(cp))
			continue;
		// 폭 0 은 앞 글자에 결합했다는 뜻이라 문맥에 달렸다.
		const XYPOSITION value = (width > 0) ? width : static_cast<XYPOSITION>(-1);
		const auto found = w.learned.find(cp);
		if (found == w.learned.end()) {
			w.learned.insert(cp, value);
		} else if (*found != value) {
			*found = -1;
		}
	}
}

}  // namespace

// 이 파일 전체가 namespace Scintilla::Internal 안이라, 카운터를 전역
// mrst::scintilla 에 두려면 블록을 잠깐 닫았다 다시 열어야 한다. 그러지 않으면
// Scintilla::Internal::mrst::scintilla 가 되어 테스트가 링크되지 않는다.
}  // namespace Scintilla::Internal

namespace mrst::scintilla {

std::atomic<unsigned long long> &measureWidthsCallCount() noexcept
{
	static std::atomic<unsigned long long> counter{0};
	return counter;
}

namespace {
std::atomic<bool> &fastPathFlag() noexcept
{
	// 환경변수는 처음 물어볼 때 한 번만 읽는다. 그 뒤로는 테스트가 바꿀 수 있다.
	static std::atomic<bool> flag{qgetenv("MRST_PLATQT_LEGACY_MEASURE").trimmed().isEmpty()};
	return flag;
}
}  // namespace

void setMonospaceFastPathEnabled(bool enabled) noexcept
{
	fastPathFlag().store(enabled, std::memory_order_relaxed);
}

bool monospaceFastPathEnabled() noexcept
{
	return fastPathFlag().load(std::memory_order_relaxed);
}

}  // namespace mrst::scintilla

namespace Scintilla::Internal {

void SurfaceImpl::MeasureWidths(const Font *font,
				std::string_view text,
                                XYPOSITION *positions)
{
	mrst::scintilla::measureWidthsCallCount().fetch_add(1, std::memory_order_relaxed);
	if (!font) {
		// 계약상 배열 전체를 채워야 한다. 원래 코드는 그냥 반환해서 호출자가
		// 초기화되지 않은 값을 읽었다.
		std::fill(positions, positions + text.length(), static_cast<XYPOSITION>(0));
		return;
	}
	SetCodec(font);
	// 문자 폭을 전부 아는 텍스트면 QTextLayout 을 아예 만들지 않는다. Scintilla
	// 코어의 지름길은 세그먼트가 **전부 ASCII** 일 때만 걸리므로, 한글이 한 글자만
	// 섞여도 여기로 온다. 한국어 문서에서는 그게 대부분이다.
	MonospaceWidths *mw = nullptr;
	if (mode.codePage == SC_CP_UTF8 && ::mrst::scintilla::monospaceFastPathEnabled()) {
		mw = &MonospaceWidthsFor(font, GetPaintDevice());
		if (mw->usable && FillMonospacePositions(text, positions, *mw))
			return;
	}
	QString su = UnicodeFromText(codec, text);
	QTextLayout tlay(su, *FontPointer(font), GetPaintDevice());
	tlay.beginLayout();
	QTextLine tl = tlay.createLine();
	tlay.endLayout();
	// 세그먼트는 BreakFinder 가 300 바이트를 넘으면 100 안팎으로 재분할하므로
	// 사실상 언제나 스택에 든다.
	if (mode.codePage == SC_CP_UTF8) {
		int fit = su.size();
		int ui=0;
		size_t i=0;
		while (ui<fit) {
			const unsigned char uch = text[i];
			const unsigned int byteCount = UTF8BytesOfLead[uch];
			const int codeUnits = UTF16LengthFromUTF8ByteCount(byteCount);
			const qreal xPosition = tl.cursorToX(ui+codeUnits);
			for (size_t bytePos=0; (bytePos<byteCount) && (i<text.length()); bytePos++) {
				positions[i++] = xPosition;
			}
			ui += codeUnits;
		}
		XYPOSITION lastPos = 0;
		if (i > 0)
			lastPos = positions[i-1];
		while (i<text.length()) {
			positions[i++] = lastPos;
		}
	} else if (mode.codePage) {
		// DBCS — 이 앱에서는 도달하지 않는다. SCI_SETCODEPAGE 에 CpUtf8 이외의
		// 값을 보내는 코드가 없어서(설정의 useUtf8 은 항상 참) 테스트로 덮을
		// 수도 없다. 상류와 어긋나지 않게 형태만 맞춰 둔다.
		int ui = 0;
		for (size_t i=0; i<text.length();) {
			size_t lenChar = DBCSIsLeadByte(mode.codePage, text[i]) ? 2 : 1;
			const qreal xPosition = tl.cursorToX(ui+1);
			for (unsigned int bytePos=0; (bytePos<lenChar) && (i<text.length()); bytePos++) {
				positions[i++] = xPosition;
			}
			ui++;
		}
	} else {
		// Single byte encoding
		for (int i=0; i<static_cast<int>(text.length()); i++) {
			positions[i] = tl.cursorToX(i+1);
		}
	}
	// 방금 계산한 결과에서 처음 보는 문자의 폭을 주워 둔다. 다음번에는 이 세그먼트가
	// 지름길을 탄다 — 한국어 문서의 ※ ○ → ℃ 같은 기호가 여기서 학습된다.
	if (mw && mw->usable)
		LearnWidths(text, positions, *mw);
}

XYPOSITION SurfaceImpl::WidthText(const Font *font, std::string_view text)
{
	QFontMetricsF metrics(*FontPointer(font), device);
	SetCodec(font);
	QString su = UnicodeFromText(codec, text);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return metrics.horizontalAdvance(su);
#else
	return metrics.width(su);
#endif
}

void SurfaceImpl::DrawTextNoClipUTF8(PRectangle rc,
				 const Font *font,
				 XYPOSITION ybase,
				 std::string_view text,
				 ColourRGBA fore,
				 ColourRGBA back)
{
	SetFont(font);
	PenColour(fore);

	GetPainter()->setBackground(QColorFromColourRGBA(back));
	GetPainter()->setBackgroundMode(Qt::OpaqueMode);
	QString su = QString::fromUtf8(text.data(), static_cast<int>(text.length()));
	mrst::scintilla::drawTextWithMask(*GetPainter(), QPointF(rc.left, ybase), su);
}

void SurfaceImpl::DrawTextClippedUTF8(PRectangle rc,
				  const Font *font,
				  XYPOSITION ybase,
				  std::string_view text,
				  ColourRGBA fore,
				  ColourRGBA back)
{
	SetClip(rc);
	DrawTextNoClipUTF8(rc, font, ybase, text, fore, back);
	PopClip();
}

void SurfaceImpl::DrawTextTransparentUTF8(PRectangle rc,
				      const Font *font,
				      XYPOSITION ybase,
				      std::string_view text,
	ColourRGBA fore)
{
	SetFont(font);
	PenColour(fore);

	GetPainter()->setBackgroundMode(Qt::TransparentMode);
	QString su = QString::fromUtf8(text.data(), static_cast<int>(text.length()));
	mrst::scintilla::drawTextWithMask(*GetPainter(), QPointF(rc.left, ybase), su);
}

void SurfaceImpl::MeasureWidthsUTF8(const Font *font,
				std::string_view text,
				XYPOSITION *positions)
{
	mrst::scintilla::measureWidthsCallCount().fetch_add(1, std::memory_order_relaxed);
	if (!font) {
		std::fill(positions, positions + text.length(), static_cast<XYPOSITION>(0));
		return;
	}
	MonospaceWidths *mw = nullptr;
	if (::mrst::scintilla::monospaceFastPathEnabled()) {
		mw = &MonospaceWidthsFor(font, GetPaintDevice());
		if (mw->usable && FillMonospacePositions(text, positions, *mw))
			return;
	}
	QString su = QString::fromUtf8(text.data(), static_cast<int>(text.length()));
	QTextLayout tlay(su, *FontPointer(font), GetPaintDevice());
	tlay.beginLayout();
	QTextLine tl = tlay.createLine();
	tlay.endLayout();
	int fit = su.size();
	int ui=0;
	size_t i=0;
	while (ui<fit) {
		const unsigned char uch = text[i];
		const unsigned int byteCount = UTF8BytesOfLead[uch];
		const int codeUnits = UTF16LengthFromUTF8ByteCount(byteCount);
		const qreal xPosition = tl.cursorToX(ui+codeUnits);
		for (size_t bytePos=0; (bytePos<byteCount) && (i<text.length()); bytePos++) {
			positions[i++] = xPosition;
		}
		ui += codeUnits;
	}
	XYPOSITION lastPos = 0;
	if (i > 0)
		lastPos = positions[i-1];
	while (i<text.length()) {
		positions[i++] = lastPos;
	}
	// 방금 계산한 결과에서 처음 보는 문자의 폭을 주워 둔다. 다음번에는 이 세그먼트가
	// 지름길을 탄다 — 한국어 문서의 ※ ○ → ℃ 같은 기호가 여기서 학습된다.
	//
	// UTF-8 문서는 PositionCache 가 MeasureWidths 가 아니라 이쪽을 부른다
	// (PositionCache.cxx 의 unicode 분기). 학습을 여기 두지 않으면 아무것도 배우지
	// 못한다 — 실제로 그렇게 만들어 놓고 한참 헤맸다.
	if (mw && mw->usable)
		LearnWidths(text, positions, *mw);
}

XYPOSITION SurfaceImpl::WidthTextUTF8(const Font *font, std::string_view text)
{
	QFontMetricsF metrics(*FontPointer(font), device);
	QString su = QString::fromUtf8(text.data(), static_cast<int>(text.length()));
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return metrics.horizontalAdvance(su);
#else
	return metrics.width(su);
#endif
}

XYPOSITION SurfaceImpl::Ascent(const Font *font)
{
	QFontMetricsF metrics(*FontPointer(font), device);
	return metrics.ascent();
}

XYPOSITION SurfaceImpl::Descent(const Font *font)
{
	QFontMetricsF metrics(*FontPointer(font), device);
	// Qt returns 1 less than true descent
	// See: QFontEngineWin::descent which says:
	// ### we subtract 1 to even out the historical +1 in QFontMetrics's
	// ### height=asc+desc+1 equation. Fix in Qt5.
	return metrics.descent() + 1;
}

XYPOSITION SurfaceImpl::InternalLeading(const Font * /* font */)
{
	return 0;
}

XYPOSITION SurfaceImpl::Height(const Font *font)
{
	QFontMetricsF metrics(*FontPointer(font), device);
	return metrics.height();
}

XYPOSITION SurfaceImpl::AverageCharWidth(const Font *font)
{
	QFontMetricsF metrics(*FontPointer(font), device);
	return metrics.averageCharWidth();
}

void SurfaceImpl::FlushCachedState()
{
	if (device->paintingActive()) {
		GetPainter()->setPen(QPen());
		GetPainter()->setBrush(QBrush());
	}
}

void SurfaceImpl::FlushDrawing()
{
}

QPaintDevice *SurfaceImpl::GetPaintDevice()
{
	return device;
}

QPainter *SurfaceImpl::GetPainter()
{
	Q_ASSERT(device);
	if (!painter) {
		if (device->paintingActive()) {
			painter = device->paintEngine()->painter();
		} else {
			painterOwned = true;
			painter = new QPainter(device);
		}

		// Set text antialiasing unconditionally.
		// The font's style strategy will override.
		painter->setRenderHint(QPainter::TextAntialiasing, true);

		painter->setRenderHint(QPainter::Antialiasing, true);

		if (scale) {
			painter->scale(1.0 / scale, 1.0 / scale);
		}
	}

	return painter;
}

std::unique_ptr<Surface> Surface::Allocate(Technology)
{
	return std::make_unique<SurfaceImpl>();
}


//----------------------------------------------------------------------

namespace {

QRect ScreenRectangleForPoint(QPoint posGlobal)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
	const QScreen *screen = QGuiApplication::screenAt(posGlobal);
	if (!screen) {
		screen = QGuiApplication::primaryScreen();
	}
	return screen->availableGeometry();
#else
	const QDesktopWidget *desktop = QApplication::desktop();
	return desktop->availableGeometry(posGlobal);
#endif
}

}

Window::~Window() noexcept = default;

void Window::Destroy() noexcept
{
	if (wid)
		delete window(wid);
	wid = nullptr;
}
PRectangle Window::GetPosition() const
{
	// Before any size allocated pretend its 1000 wide so not scrolled
	return wid ? PRectFromQRect(window(wid)->frameGeometry()) : PRectangle(0, 0, 1000, 1000);
}

void Window::SetPosition(PRectangle rc)
{
	if (wid)
		window(wid)->setGeometry(QRectFromPRect(rc));
}

void Window::SetPositionRelative(PRectangle rc, const Window *relativeTo)
{
	const qreal scale = ScaleToMultiply(relativeTo->wid);
	rc = rc / scale;
	QPoint oPos = window(relativeTo->wid)->mapToGlobal(QPoint(0,0));
	int ox = oPos.x();
	int oy = oPos.y();
	ox += rc.left;
	oy += rc.top;

	const QRect rectDesk = ScreenRectangleForPoint(QPoint(ox, oy));
	/* do some corrections to fit into screen */
	int sizex = rc.right - rc.left;
	int sizey = rc.bottom - rc.top;
	int screenWidth = rectDesk.width();
	if (ox < rectDesk.x())
		ox = rectDesk.x();
	if (sizex > screenWidth)
		ox = rectDesk.x(); /* the best we can do */
	else if (ox + sizex > rectDesk.right())
		ox = rectDesk.right() - sizex;
	if (oy + sizey > rectDesk.bottom())
		oy = rectDesk.bottom() - sizey;
	if (oy < rectDesk.top())
		oy = rectDesk.top();

	Q_ASSERT(wid);
	window(wid)->move(ox, oy);
	window(wid)->resize(sizex, sizey);
}

PRectangle Window::GetClientPosition() const
{
	// The client position is the window position
	const qreal scale = ScaleToMultiply(wid);
	const PRectangle rc = GetPosition();
	return rc * scale;
}

void Window::Show(bool show)
{
	if (wid)
		window(wid)->setVisible(show);
}

void Window::InvalidateAll()
{
	if (wid)
		window(wid)->update();
}

void Window::InvalidateRectangle(PRectangle rc)
{
	if (wid) {
		const qreal scale = ScaleOfWindow(wid);
		QRect upd;
		
		if (!scale) {
			upd = QRectFromPRect(rc);
		} else {
#if !defined(Q_OS_WIN) && !defined(Q_OS_APPLE) && !defined(SCINTILLA_QT_BACKINGSTORE_FIXED)
			// Using X11 or Wayland, likely Linux but may be a BSD or similar
			if (scale != 1.0 && scale != 2.0) {
				window(wid)->update();
				return;
			}
#endif
			upd = QRectFFromPRect(rc / scale).toAlignedRect();
		}

#if defined(Q_OS_APPLE) && !defined(SCINTILLA_QT_BACKINGSTORE_FIXED)
		// macOS: snap the update to the backing store's device-pixel grid.
		const QRect snapped = SnapToDevicePixelGrid(upd, scale, window(wid)->size());
		if (snapped.isNull()) {
			window(wid)->update();
			return;
		}
		upd = snapped;
#endif

		window(wid)->update(upd);
	}
}

void Window::SetCursor(Cursor curs)
{
	if (wid) {
		Qt::CursorShape shape;

		switch (curs) {
			case Cursor::text:  shape = Qt::IBeamCursor;        break;
			case Cursor::arrow: shape = Qt::ArrowCursor;        break;
			case Cursor::up:    shape = Qt::UpArrowCursor;      break;
			case Cursor::wait:  shape = Qt::WaitCursor;         break;
			case Cursor::horizontal: shape = Qt::SizeHorCursor; break;
			case Cursor::vertical:  shape = Qt::SizeVerCursor;  break;
			case Cursor::hand:  shape = Qt::PointingHandCursor; break;
			default:            shape = Qt::ArrowCursor;        break;
		}

		QCursor cursor = QCursor(shape);

		if (curs != cursorLast) {
			window(wid)->setCursor(cursor);
			cursorLast = curs;
		}
	}
}

/* Returns rectangle of monitor pt is on, both rect and pt are in Window's
   window coordinates */
PRectangle Window::GetMonitorRect(Point pt)
{
	const QPoint posGlobal = window(wid)->mapToGlobal(QPoint(pt.x, pt.y));
	const QPoint originGlobal = window(wid)->mapToGlobal(QPoint(0, 0));
	QRect rectScreen = ScreenRectangleForPoint(posGlobal);
	rectScreen.translate(-originGlobal.x(), -originGlobal.y());
	return PRectFromQRect(rectScreen);
}

//----------------------------------------------------------------------
class ListWidget : public QListWidget {
public:
	explicit ListWidget(QWidget *parent_);

	void setDelegate(IListBoxDelegate *lbDelegate);

	int currentSelection();

protected:
	void showEvent(QShowEvent *event) override;
	void selectionChanged(const QItemSelection &selected, const QItemSelection &deselected) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	void initViewItemOption(QStyleOptionViewItem *option) const override;
#else
	QStyleOptionViewItem viewOptions() const override;
#endif

private:
	QWidget *parent = nullptr;
	IListBoxDelegate *delegate;
};

class ListBoxImpl : public ListBox {
public:
	ListBoxImpl() noexcept;

	void SetFont(const Font *font) override;
	void Create(Window &parent, int ctrlID, Point location,
						int lineHeight, bool unicodeMode_, Technology technology) override;
	void SetAverageCharWidth(int width) override;
	void SetVisibleRows(int rows) override;
	int GetVisibleRows() const override;
	PRectangle GetDesiredRect() override;
	int CaretFromEdge() override;
	void Clear() noexcept override;
	void Append(char *s, int type) override;
	int Length() override;
	void Select(int n) override;
	int GetSelection() override;
	int Find(const char *prefix) override;
	std::string GetValue(int n) override;
	void RegisterImage(int type, const char *xpmData) override;
	void RegisterRGBAImage(int type, int width, int height,
		const unsigned char *pixelsImage) override;
	virtual void RegisterQPixmapImage(int type, const QPixmap& pm);
	void ClearRegisteredImages() override;
	void SetDelegate(IListBoxDelegate *lbDelegate) override;
	void SetList(const char *list, char separator, char typesep) override;
	void SetOptions(ListOptions options_) override;

	[[nodiscard]] ListWidget *GetWidget() const noexcept;
private:
	bool unicodeMode{false};
	int visibleRows{5};
	QMap<int,QPixmap> images;
	float imageScale{1.0};
	QWidget *owner = nullptr;
};
ListBoxImpl::ListBoxImpl() noexcept = default;

void ListBoxImpl::Create(Window &parent,
                         int /*ctrlID*/,
                         Point location,
                         int /*lineHeight*/,
                         bool unicodeMode_,
			 Technology)
{
	unicodeMode = unicodeMode_;

	QWidget *qparent = static_cast<QWidget *>(parent.GetID());
	owner = qparent;
	ListWidget *list = new ListWidget(qparent);
	const qreal scale = ScaleOfWindow(parent.GetID());
	list->setProperty("ScintillaScale", scale);

#if defined(Q_OS_WIN)
	// On Windows, Qt::ToolTip causes a crash when the list is clicked on
	// so Qt::Tool is used.
	list->setParent(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
		| Qt::WindowDoesNotAcceptFocus
#endif
	);
#else
	// On macOS, Qt::Tool takes focus so main window loses focus so
	// keyboard stops working. Qt::ToolTip works but its only really
	// documented for tooltips.
	// On Linux / X this setting allows clicking on list items.
	list->setParent(nullptr, static_cast<Qt::WindowFlags>(Qt::ToolTip | Qt::FramelessWindowHint));
#endif
	list->setAttribute(Qt::WA_ShowWithoutActivating);
	list->setFocusPolicy(Qt::NoFocus);
	list->setUniformItemSizes(true);
	list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	list->move(location.x, location.y);

	int maxIconWidth = 0;
	int maxIconHeight = 0;
	foreach (QPixmap im, images) {
		im.setDevicePixelRatio(imageScale);
		if (maxIconWidth < im.width() / im.devicePixelRatio())
			maxIconWidth = im.width() / im.devicePixelRatio();
		if (maxIconHeight < im.height() / im.devicePixelRatio())
			maxIconHeight = im.height() / im.devicePixelRatio();
	}
	list->setIconSize(QSize(maxIconWidth, maxIconHeight));

	wid = list;
}
void ListBoxImpl::SetFont(const Font *font)
{
	ListWidget *list = GetWidget();
	const FontAndCharacterSet *pfacs = AsFontAndCharacterSet(font);
	if (pfacs && pfacs->pfont) {
		QFont fontDeScaled = *(pfacs->pfont);
		const qreal scaleOwner = ScaleOfWindow(owner);
		if (scaleOwner) {
			const qreal scale = window(owner)->devicePixelRatioF();
			qreal pointSize = fontDeScaled.pointSizeF() / scale;
			fontDeScaled.setPointSizeF(pointSize);
		}
		list->setFont(fontDeScaled);
	}
}
void ListBoxImpl::SetAverageCharWidth(int /*width*/) {}

void ListBoxImpl::SetVisibleRows(int rows)
{
	visibleRows = rows;
}

int ListBoxImpl::GetVisibleRows() const
{
	return visibleRows;
}
PRectangle ListBoxImpl::GetDesiredRect()
{
	ListWidget *list = GetWidget();
	int rows = Length();
	if (rows == 0 || rows > visibleRows) {
		rows = visibleRows;
	}
	int rowHeight = list->sizeHintForRow(0);
	int height = (rows * rowHeight) + (2 * list->frameWidth());

	QStyle *style = QApplication::style();
	int width = list->sizeHintForColumn(0) + (2 * list->frameWidth());
	if (Length() > rows) {
		width += style->pixelMetric(QStyle::PM_ScrollBarExtent) + 1;
	}

	const qreal scale = ScaleOfWindow(wid);
	return PRectangle(0, 0, width, height) * (scale ? scale : 1.0);
}
int ListBoxImpl::CaretFromEdge()
{
	ListWidget *list = GetWidget();
	int maxIconWidth = 0;
	foreach (QPixmap im, images) {
		if (maxIconWidth < im.width() / im.devicePixelRatio())
			maxIconWidth = im.width() / im.devicePixelRatio();
	}

	int extra;
	// The 12 is from trial and error on macOS and the 7
	// is from trial and error on Windows - there may be
	// a better programmatic way to find any padding factors.
#ifdef Q_OS_DARWIN
	extra = 12;
#else
	extra = 7;
#endif
	return maxIconWidth + (2 * list->frameWidth()) + extra;
}
void ListBoxImpl::Clear() noexcept
{
	ListWidget *list = GetWidget();
	list->clear();
}
void ListBoxImpl::Append(char *s, int type)
{
	ListWidget *list = GetWidget();
	QString str = unicodeMode ? QString::fromUtf8(s) : QString::fromLocal8Bit(s);
	QIcon icon;
	if (type >= 0) {
		Q_ASSERT(images.contains(type));
		icon = images.value(type);
	}
	new QListWidgetItem(icon, str, list);
}
int ListBoxImpl::Length()
{
	ListWidget *list = GetWidget();
	return list->count();
}
void ListBoxImpl::Select(int n)
{
	ListWidget *list = GetWidget();
	QModelIndex index = list->model()->index(n, 0);
	if (index.isValid()) {
		QRect row_rect = list->visualRect(index);
		if (!list->viewport()->rect().contains(row_rect)) {
			list->scrollTo(index, QAbstractItemView::PositionAtTop);
		}
	}
	list->setCurrentRow(n);
}
int ListBoxImpl::GetSelection()
{
	ListWidget *list = GetWidget();
	return list->currentSelection();
}
int ListBoxImpl::Find(const char *prefix)
{
	ListWidget *list = GetWidget();
	QString sPrefix = unicodeMode ? QString::fromUtf8(prefix) : QString::fromLocal8Bit(prefix);
	QList<QListWidgetItem *> ms = list->findItems(sPrefix, Qt::MatchStartsWith);
	int result = -1;
	if (!ms.isEmpty()) {
		result = list->row(ms.first());
	}

	return result;
}
std::string ListBoxImpl::GetValue(int n)
{
	ListWidget *list = GetWidget();
	QListWidgetItem *item = list->item(n);
	QString str = item->data(Qt::DisplayRole).toString();
	QByteArray bytes = unicodeMode ? str.toUtf8() : str.toLocal8Bit();
	return std::string(bytes.constData());
}

void ListBoxImpl::RegisterQPixmapImage(int type, const QPixmap& pm)
{
	images[type] = pm;
	ListWidget *list = GetWidget();
	if (list) {
		QSize iconSize = list->iconSize();
		if (pm.width() / pm.devicePixelRatio() > iconSize.width() || pm.height() / pm.devicePixelRatio() > iconSize.height())
			list->setIconSize(QSize(qMax(qFloor(pm.width() / pm.devicePixelRatio()), iconSize.width()),
						 qMax(qFloor(pm.height() / pm.devicePixelRatio()), iconSize.height())));
	}

}

void ListBoxImpl::RegisterImage(int type, const char *xpmData)
{
	XPM xpmImage(xpmData);
	RGBAImage rgbaImage(xpmImage);
	RegisterRGBAImage(type, rgbaImage.GetWidth(), rgbaImage.GetHeight(), rgbaImage.Pixels());
}

void ListBoxImpl::RegisterRGBAImage(int type, int width, int height, const unsigned char *pixelsImage)
{
	std::vector<unsigned char> imageBytes = ImageByteSwapped(width, height, pixelsImage);
	QImage image(&imageBytes[0], width, height, QImage::Format_ARGB32);
	RegisterQPixmapImage(type, QPixmap::fromImage(image));
}

void ListBoxImpl::ClearRegisteredImages()
{
	images.clear();
	ListWidget *list = GetWidget();
	if (list)
		list->setIconSize(QSize(0, 0));
}
void ListBoxImpl::SetDelegate(IListBoxDelegate *lbDelegate)
{
	ListWidget *list = GetWidget();
	list->setDelegate(lbDelegate);
}
void ListBoxImpl::SetList(const char *list, char separator, char typesep)
{
	// This method is *not* platform dependent.
	// It is borrowed from the GTK implementation.
	Clear();
	size_t count = strlen(list) + 1;
	std::vector<char> words(list, list+count);
	char *startword = &words[0];
	char *numword = nullptr;
	int i = 0;
	for (; words[i]; i++) {
		if (words[i] == separator) {
			words[i] = '\0';
			if (numword)
				*numword = '\0';
			Append(startword, numword?atoi(numword + 1):-1);
			startword = &words[0] + i + 1;
			numword = nullptr;
		} else if (words[i] == typesep) {
			numword = &words[0] + i;
		}
	}
	if (startword) {
		if (numword)
			*numword = '\0';
		Append(startword, numword?atoi(numword + 1):-1);
	}
}
void ListBoxImpl::SetOptions(ListOptions options_)
{
	imageScale = options_.imageScale;
}
ListWidget *ListBoxImpl::GetWidget() const noexcept
{
	return static_cast<ListWidget *>(wid);
}

ListBox::ListBox() noexcept = default;
ListBox::~ListBox() noexcept = default;

std::unique_ptr<ListBox> ListBox::Allocate()
{
	return std::make_unique<ListBoxImpl>();
}
ListWidget::ListWidget(QWidget *parent_)
: QListWidget(parent_), parent(parent_), delegate(nullptr)
{}

void ListWidget::setDelegate(IListBoxDelegate *lbDelegate)
{
	delegate = lbDelegate;
}

void ListWidget::showEvent(QShowEvent *)
{
	windowHandle()->setTransientParent(parent->window()->windowHandle());
}

void ListWidget::selectionChanged(const QItemSelection &selected, const QItemSelection &deselected) {
	QListWidget::selectionChanged(selected, deselected);
	if (delegate) {
		const int selection = currentSelection();
		if (selection >= 0) {
			ListBoxEvent event(ListBoxEvent::EventType::selectionChange);
			delegate->ListNotify(&event);
		}
	}
}

int ListWidget::currentSelection() {
	const QModelIndexList indices = selectionModel()->selectedRows();
	foreach (const QModelIndex ind, indices) {
		return ind.row();
	}
	return -1;
}

void ListWidget::mouseDoubleClickEvent(QMouseEvent * /* event */)
{
	if (delegate) {
		ListBoxEvent event(ListBoxEvent::EventType::doubleClick);
		delegate->ListNotify(&event);
	}
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void ListWidget::initViewItemOption(QStyleOptionViewItem *option) const
{
	QListWidget::initViewItemOption(option);
	option->state |= QStyle::State_Active;
}
#else
QStyleOptionViewItem ListWidget::viewOptions() const
{
	QStyleOptionViewItem result = QListWidget::viewOptions();
	result.state |= QStyle::State_Active;
	return result;
}
#endif
//----------------------------------------------------------------------
Menu::Menu() noexcept : mid(nullptr) {}
void Menu::CreatePopUp()
{
	Destroy();
	mid = new QMenu();
}

void Menu::Destroy() noexcept
{
	if (mid) {
		QMenu *menu = static_cast<QMenu *>(mid);
		delete menu;
	}
	mid = nullptr;
}
void Menu::Show(Point pt, const Window & /*w*/)
{
	QMenu *menu = static_cast<QMenu *>(mid);
	menu->exec(QPoint(pt.x, pt.y));
	Destroy();
}

//----------------------------------------------------------------------

ColourRGBA Platform::Chrome()
{
	QColor c(Qt::gray);
	return ColourRGBA(c.red(), c.green(), c.blue());
}

ColourRGBA Platform::ChromeHighlight()
{
	QColor c(Qt::lightGray);
	return ColourRGBA(c.red(), c.green(), c.blue());
}

const char *Platform::DefaultFont()
{
	static char fontNameDefault[200] = "";
	if (!fontNameDefault[0]) {
		QFont font = QApplication::font();
		strcpy(fontNameDefault, font.family().toUtf8());
	}
	return fontNameDefault;
}

int Platform::DefaultFontSize()
{
	QFont font = QApplication::font();
	return font.pointSize();
}

unsigned int Platform::DoubleClickTime()
{
	return QApplication::doubleClickInterval();
}

void Platform::DebugDisplay(const char *s) noexcept
{
	qWarning("Scintilla: %s", s);
}

void Platform::DebugPrintf(const char *format, ...) noexcept
{
	char buffer[2000];
	va_list pArguments{};
	va_start(pArguments, format);
	vsnprintf(buffer, std::size(buffer), format, pArguments);
	va_end(pArguments);
	Platform::DebugDisplay(buffer);
}

bool Platform::ShowAssertionPopUps(bool /*assertionPopUps*/) noexcept
{
	return false;
}

void Platform::Assert(const char *c, const char *file, int line) noexcept
{
	char buffer[2000];
	snprintf(buffer, std::size(buffer), "Assertion [%s] failed at %s %d", c, file, line);
	if (Platform::ShowAssertionPopUps(false)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
		QMessageBox mb(QMessageBox::Icon::NoIcon, "Assertion Failure", buffer,
				QMessageBox::StandardButton::Ok);
#else
		QMessageBox mb("Assertion Failure", buffer, QMessageBox::NoIcon,
			QMessageBox::Ok, QMessageBox::NoButton, QMessageBox::NoButton);
#endif
		mb.exec();
	} else {
		strcat(buffer, "\n");
		Platform::DebugDisplay(buffer);
	}
}

}
