#pragma once
#include <QtWidgets>
#include <fmfilelist/RecordPainting.h>

namespace hc::ui {
// Draft-only samples. File states use the same painters as the product list;
// the other samples use the semantic tokens consumed by the product style.
class ThemePreview final : public QWidget {
public:
    explicit ThemePreview(QWidget* Parent = nullptr) : QWidget(Parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(tr("테마 미리보기: 보통, 선택, 커서, 역상 커서, 선택과 역상 커서, 역상 선택. 버튼, 링크, 진행, 상태 배지."));
    }
    void SetColors(const fm::style::ThemeColors& Colors) { colors_ = Colors; update(); }
    QSize sizeHint() const override { return {440, rowHeight() * 10 + 12}; }
    QSize minimumSizeHint() const override { return {260, sizeHint().height()}; }
protected:
    void paintEvent(QPaintEvent*) override {
        using namespace fm::style; using namespace fm::filelist; using namespace fm::filelist::detail;
        QPainter Painter(this); const auto& Colors = colors_; const int Pitch = rowHeight();
        Painter.fillRect(rect(), Colors[Token::Surface]);
        const auto Text = [&](QRect Area, const QString& Value, const QColor& Color, int Alignment = Qt::AlignVCenter | Qt::AlignLeft) {
            Painter.setPen(Color); Painter.drawText(Area, Alignment, fontMetrics().elidedText(Value, Qt::ElideRight, qMax(0, Area.width())));
        };
        Text(QRect(8, 0, width() - 16, Pitch), tr("이름 · 편집 중인 변형의 행 상태"), Colors[Token::Fg2]);
        const QStringList Names{"Projects", "qt-online-installer.exe", "release-notes.md", "IMG_2041.heic", "backup-0927.zip", "MainWindow.cpp"};
        const QStringList States{tr("보통"), tr("선택"), tr("커서"), tr("역상 커서"), tr("선택 + 역상 커서"), tr("역상 선택")};
        const std::array<Token, 6> Icons{Token::Folder, Token::KExe, Token::KDoc, Token::KImg, Token::KZip, Token::KCode};
        for (int I = 0; I < 6; ++I) {
            ListAppearance Appearance; Appearance.invertCursor = I == 3 || I == 4; Appearance.invertSelection = I == 5;
            const RecordState State{true, I == 1 || I == 4 || I == 5, I >= 2 && I <= 4, false};
            auto Geometry = RecordGeometry::make(true, false, Appearance); Geometry.pitch = Pitch; Geometry.nameHeight = Pitch;
            const QRect Record(0, (I + 1) * Pitch, width(), Pitch);
            paintRecordBackground(&Painter, Record, Geometry, Appearance, State, false, Colors);
            const auto Foregrounds = textColors(Colors, Appearance, State, false, Geometry.separator);
            Painter.fillRect(QRect(8, Record.y() + (Pitch - 12) / 2, 12, 12), Colors[Icons[I]]);
            Text(QRect(26, Record.y(), width() * 3 / 5 - 30, Pitch), Names[I], Foregrounds.name);
            Text(QRect(width() * 3 / 5, Record.y(), width() * 2 / 5 - 8, Pitch), States[I], Foregrounds.meta, Qt::AlignVCenter | Qt::AlignRight);
            paintRecordCursor(&Painter, Record, Geometry, Appearance, State, Colors);
        }
        const QStringList Buttons{tr("기본 버튼"), tr("보조"), tr("영구 삭제"), tr("강조 링크")};
        const std::array<Token, 4> Backgrounds{Token::Accent, Token::Btn, Token::DangerFill, Token::Surface};
        const std::array<Token, 4> Foregrounds{Token::OnAccent, Token::Fg, Token::OnDanger, Token::AccentFg};
        const int Cell = width() / 4;
        for (int I = 0; I < 4; ++I) {
            const QRect Area(I * Cell + 4, Pitch * 7 + 5, Cell - 8, Pitch - 4);
            Painter.fillRect(Area, Colors[Backgrounds[I]]); if (I < 3) { Painter.setPen(Colors[Token::BtnLine]); Painter.drawRect(Area); }
            auto Font = font(); Font.setUnderline(I == 3); Painter.setFont(Font); Text(Area.adjusted(3, 0, -3, 0), Buttons[I], Colors[Foregrounds[I]], Qt::AlignCenter);
        }
        Painter.setFont(font()); const int Half = width() / 2;
        for (int I = 0; I < 2; ++I) { const QRect Track(8 + Half * I, Pitch * 8 + Pitch / 2, Half - 16, 6); Painter.fillRect(Track, Colors[Token::Line]); Painter.fillRect(QRect(Track.x(), Track.y(), Track.width() / 2, Track.height()), Colors[I ? Token::Paused : Token::Accent]); }
        const QStringList Badges{tr("정상"), tr("권한 필요"), tr("충돌")};
        const std::array<Token, 3> BadgeBg{Token::OkBg, Token::WarnBg, Token::DangerBg}; const std::array<Token, 3> BadgeFg{Token::Ok, Token::Warn, Token::Danger};
        for (int I = 0; I < 3; ++I) { const QRect Area(I * width() / 3 + 4, Pitch * 9 + 3, width() / 3 - 8, Pitch - 4); Painter.fillRect(Area, Colors[BadgeBg[I]]); Text(Area, Badges[I], Colors[BadgeFg[I]], Qt::AlignCenter); }
        Painter.setPen(Colors[Token::Line]); Painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
private:
    int rowHeight() const { return fontMetrics().height() + 8; }
    fm::style::ThemeColors colors_;
};
}
