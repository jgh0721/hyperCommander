#include <QtWidgets>
#include <QtTest>
#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#endif
#include <oclero/qlementine/style/QlementineStyle.hpp>
#include <oclero/qlementine/style/Theme.hpp>
#include <QtitanGrid.h>
#include <QtnCommonStyle.h>
#include "externalLibs/QtitanDataGrid/src/src/grid/QtnGridColumnHeaderLayout.h"
#include "externalLibs/QtitanDataGrid/src/src/grid/QtnGridViewGraphics.h"
#include "externalLibs/QtitanDataGrid/src/src/grid/QtnGridModelController.h"

namespace
{
    struct HeaderColors
    {
        QColor background_ = QColor("#e8edf5");
        QColor foreground_ = QColor("#20242b");
        QColor separator_ = QColor("#788395");
        QColor hover_ = QColor("#dbe6f6");
        QColor pressed_ = QColor("#c9d9f1");
        QColor sorted_ = QColor("#dce8ff");
        QColor indicator_ = QColor("#2457c5");
    };

    // Qtitan keeps its extension points; ordinary controls delegate to Qlementine.
    // One owner chain avoids deleting/replacing the base style under existing widgets.
    class ReadinessStyle final : public Qtitan::CommonStyle
    {
    public:
        explicit ReadinessStyle(oclero::qlementine::QlementineStyle* Base)
            : Qtitan::CommonStyle(Base), qlementine_(Base) {}

        void SetDark(bool Dark)
        {
            colors_ = Dark ? HeaderColors{QColor("#293241"), QColor("#e8edf5"), QColor("#6c788a"),
                QColor("#34445c"), QColor("#405575"), QColor("#334767"), QColor("#9bbeff")} : HeaderColors{};
            const QJsonObject Tokens{
                {"meta", QJsonObject{{"name", Dark ? "HC P0 Dark" : "HC P0 Light"}, {"version", "0.1"}, {"author", "HyperCommander"}}},
                {"useSystemFonts", true},
                {"backgroundColorMain1", Dark ? "#181c23" : "#ffffff"},
                {"backgroundColorMain2", Dark ? "#20252e" : "#f4f6fa"},
                {"backgroundColorMain3", colors_.background_.name()},
                {"backgroundColorWorkspace", Dark ? "#181c23" : "#ffffff"},
                {"backgroundColorTabBar", colors_.background_.name()},
                {"secondaryColor", colors_.foreground_.name()},
                {"secondaryAlternativeColor", Dark ? "#aab5c5" : "#586174"},
                {"primaryColor", colors_.indicator_.name()},
                {"primaryColorForeground", Dark ? "#102342" : "#ffffff"},
                {"focusColor", colors_.indicator_.name()}, {"borderColor", colors_.separator_.name()}};
            auto Merged = (Dark ? oclero::qlementine::Theme::makeDark() : oclero::qlementine::Theme::makeLight()).toJson().object();
            for (auto It = Tokens.begin(); It != Tokens.end(); ++It) Merged.insert(It.key(), It.value());
            const auto Theme = oclero::qlementine::Theme::fromJsonDoc(QJsonDocument(Merged));
            if (!Theme) qFatal("P0 theme JSON is invalid");
            qlementine_->setTheme(*Theme);
            QApplication::setPalette(Theme->palette);
            auto Font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
            Font.setPointSizeF(11);
            QApplication::setFont(Font);
            for (const auto Pair : {qMakePair(Qtitan::GridThemeManager::SortAscendingIcon, false),
                                   qMakePair(Qtitan::GridThemeManager::SortDescendingIcon, true)})
            {
                QPixmap Pixmap(16, 16); Pixmap.fill(Qt::transparent);
                QPainter Painter(&Pixmap); Painter.setPen(Qt::NoPen); Painter.setBrush(colors_.indicator_);
                Painter.drawPolygon(Pair.second ? QPolygon{{3, 5}, {13, 5}, {8, 11}} : QPolygon{{3, 11}, {13, 11}, {8, 5}});
                Painter.end();
                Qtitan::GridBase::themeManager()->setIcon(Pair.first, QIcon(Pixmap));
            }
            Qtitan::GridBase::themeManager()->updateViews();
        }

        const HeaderColors& Colors() const { return colors_; }
        int HeaderPaints() const { return headerPaints_; }
        void SetHeaderBackground(const QColor& Color) { colors_.background_ = Color; }

        QRect subElementRect(SubElement Element, const QStyleOption* Option,
            const QWidget* Widget = nullptr) const override
        {
            // Qtitan asks for the cell's content box before assigning its text.
            // Qlementine's item-view layout returns an empty text box in that case.
            if (Element == SE_ItemViewItemText && isGridWidget(Widget)) return Option->rect;
            return Qtitan::CommonStyle::subElementRect(Element, Option, Widget);
        }

        void drawControl(ControlElement Element, const QStyleOption* Option, QPainter* Painter,
            const QWidget* Widget = nullptr) const override
        {
            const auto* Header = qstyleoption_cast<const QStyleOptionHeader*>(Option);
            if (Header && Widget && Widget->property("hc.p0.grid").toBool())
            {
                if (Element == CE_HeaderSection)
                {
                    ++headerPaints_;
                    const auto Color = Header->state.testFlag(State_Sunken) ? colors_.pressed_ :
                        Header->state.testFlag(State_MouseOver) ? colors_.hover_ :
                        Header->sortIndicator != QStyleOptionHeader::None ? colors_.sorted_ : colors_.background_;
                    Painter->fillRect(Header->rect, Color);
                    Painter->save(); Painter->setPen(colors_.separator_);
                    Painter->drawLine(Header->rect.bottomLeft(), Header->rect.bottomRight());
                    Painter->drawLine(Header->rect.topRight(), Header->rect.bottomRight()); Painter->restore();
                    return;
                }
                if (Element == CE_HeaderLabel)
                {
                    QStyleOptionHeader Copy = *Header;
                    Copy.palette.setColor(QPalette::ButtonText, colors_.foreground_);
                    Copy.palette.setColor(QPalette::Text, colors_.foreground_);
                    Qtitan::CommonStyle::drawControl(Element, &Copy, Painter, Widget);
                    return;
                }
            }
            Qtitan::CommonStyle::drawControl(Element, Option, Painter, Widget);
        }
    private:
        static bool isGridWidget(const QWidget* Widget)
        {
            for (auto* Current = Widget; Current; Current = Current->parentWidget())
                if (Current->property("hc.p0.grid").toBool()) return true;
            return false;
        }
        QPointer<oclero::qlementine::QlementineStyle> qlementine_;
        HeaderColors colors_;
        mutable int headerPaints_ = 0;
    };

    class ReadinessWindow final : public QMainWindow
    {
    public:
        explicit ReadinessWindow(ReadinessStyle* Style) : style_(Style)
        {
            setWindowTitle(QStringLiteral("HyperCommander P0 — Qlementine / Qtitan"));
            auto* FileMenu = menuBar()->addMenu(QStringLiteral("파일(&F)"));
            FileMenu->addAction(QStringLiteral("닫기"), this, &QWidget::close);
            auto* Central = new QWidget(this); setCentralWidget(Central);
            auto* Layout = new QVBoxLayout(Central);
            auto* Controls = new QHBoxLayout;
            theme_ = new QComboBox(Central); theme_->addItems({"Light", "Dark"});
            mode_ = new QComboBox(Central); mode_->addItems({QStringLiteral("한 줄"), QStringLiteral("두 줄: 속성 → 이름"), QStringLiteral("두 줄: 이름 → 속성")});
            auto* HeaderButton = new QPushButton(QStringLiteral("헤더 배경…"), Central);
            Controls->addWidget(new QLabel(QStringLiteral("테마"), Central)); Controls->addWidget(theme_);
            Controls->addWidget(new QLabel(QStringLiteral("헤더 배치"), Central)); Controls->addWidget(mode_);
            Controls->addWidget(HeaderButton); Controls->addStretch(); Layout->addLayout(Controls);
            auto* Tabs = new QTabWidget(Central);
            auto* Page = new QWidget(Tabs); auto* PageLayout = new QVBoxLayout(Page);
            input_ = new QLineEdit(QStringLiteral("한글 파일명 · 이름 변경 입력 시험"), Page);
            input_->setAccessibleName(QStringLiteral("입력 시험")); PageLayout->addWidget(input_);
            grid_ = new Qtitan::Grid(Page); grid_->setProperty("hc.p0.grid", true);
            grid_->setViewType(Qtitan::Grid::BandedTableView); PageLayout->addWidget(grid_);
            Tabs->addTab(Page, QStringLiteral("파일 목록")); Tabs->addTab(new QTextEdit(Tabs), QStringLiteral("일반 위젯")); Layout->addWidget(Tabs);
            model_ = new QStandardItemModel(3, 4, grid_);
            const QStringList Names{QStringLiteral("프로젝트 자료"), QStringLiteral("2026년_하반기_사업계획_부서별_검토의견_및_예산조정_반영_최종확인_회의제출용_v12.xlsx"), QStringLiteral("HyperCommander_화면구성_검토결과_최종본.pdf")};
            for (int Row = 0; Row < 3; ++Row)
            {
                model_->setData(model_->index(Row, 0), Names[Row]);
                model_->setData(model_->index(Row, 1), Row == 0 ? QStringLiteral("<폴더>") : QStringLiteral("128 KB"));
                model_->setData(model_->index(Row, 2), QStringLiteral("2026-09-20 10:10"));
                model_->setData(model_->index(Row, 3), Row == 0 ? "D" : "A");
            }
            view_ = grid_->view<Qtitan::GridBandedTableView>();
            view_->beginUpdate();
            view_->options().setAutoCreateColumns(false); view_->options().setGroupsHeader(false);
            view_->options().setRowAutoHeight(false);
            view_->options().setSelectionPolicy(Qtitan::GridViewOptions::MultiRowSelection);
            view_->setFileManagerMode(true);
            view_->tableOptions().setColumnAutoWidth(false);
            view_->bandedOptions().setBandsHeader(false);
            view_->bandedOptions().setChangeColumnRowOnMoving(false);
            view_->addBand(QStringLiteral("파일"));
            view_->setModel(model_);
            const QStringList Captions{QStringLiteral("파일 이름"), QStringLiteral("크기"), QStringLiteral("수정한 날짜"), QStringLiteral("속성")};
            for (int Index = 0; Index < 4; ++Index)
            {
                auto* Column = view_->addColumn(Index, Captions[Index]);
                Column->setBandIndex(0); Column->setRowSpan(1); Column->setRowIndex(0);
                Column->setBackgroundColor(Qt::transparent); Column->setFilterButtonVisible(false);
                Column->setMinWidth(24); Column->setMaxWidth(10000); Column->setAutoWidth(false);
                Column->setEditorType(Qtitan::GridEditor::String);
                const auto Alignment = (Index == 0 ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter;
                Column->setTextAlignment(Alignment); Column->editorRepository()->setAlignment(Alignment);
                columns_.append(Column);
            }
            view_->endUpdate();
            statusBar()->showMessage(QStringLiteral("P0 시험 화면 · 제품 설정은 읽거나 저장하지 않습니다."));
            connect(theme_, &QComboBox::currentIndexChanged, this, [this](int Index) { SetDark(Index == 1); });
            connect(mode_, &QComboBox::currentIndexChanged, this, &ReadinessWindow::SetLayout);
            connect(HeaderButton, &QPushButton::clicked, this, [this] {
                const auto Color = QColorDialog::getColor(style_->Colors().background_, this, QStringLiteral("헤더 배경"));
                if (Color.isValid()) { style_->SetHeaderBackground(Color); grid_->viewport()->update(); }
            });
            resize(1000, 560); SetDark(false);
        }

        Qtitan::GridBandedTableView* View() const { return view_; }
        Qtitan::Grid* Grid() const { return grid_; }
        QStandardItemModel* Model() const { return model_; }
        Qtitan::GridBandedTableColumn* Column(int Index) const { return columns_[Index]; }
        QRect HeaderRect(int Index) const { return static_cast<Qtitan::GridTableViewPrimitive*>(view_->Qtitan::GridViewBase::layout())->headerLayout()->getItem(columns_[Index])->rect(); }
        QRect CellRect(int Index) const { return static_cast<Qtitan::GridTableViewPrimitive*>(view_->Qtitan::GridViewBase::layout())->headerLayout()->getItem(columns_[Index])->cellRect(); }
        QLineEdit* Input() const { return input_; }

        void SetDark(bool Dark)
        {
            const QSignalBlocker ThemeBlock(theme_); theme_->setCurrentIndex(Dark ? 1 : 0);
            style_->SetDark(Dark);
            for (auto* Column : columns_) { Column->setTextColor(style_->Colors().foreground_); Column->setFont(QApplication::font()); }
            view_->options().setBackgroundColor(Dark ? QColor("#181c23") : QColor("#ffffff"));
            view_->options().setSelectedBackgroundColor(Dark ? QColor("#9bbeff") : QColor("#2457c5"));
            view_->options().setRowFocusColor(style_->Colors().indicator_);
            view_->requestUpdateLayout(true); grid_->viewport()->update();
        }

        void SetLayout(int Mode)
        {
            const QSignalBlocker ModeBlock(mode_); mode_->setCurrentIndex(Mode);
            layoutMode_ = Mode;
            view_->beginUpdate();
            for (int Index = 0; Index < columns_.size(); ++Index)
                columns_[Index]->setRowIndex(Mode == 0 ? 0 : (Index == 0 ? (Mode == 1 ? 1 : 0) : (Mode == 1 ? 0 : 1)));
            view_->endUpdate();
            applyWidths();
        }
    protected:
        void resizeEvent(QResizeEvent* Event) override
        {
            QMainWindow::resizeEvent(Event);
            QTimer::singleShot(0, this, [this] { applyWidths(); });
        }
    private:
        void applyWidths()
        {
            const auto Width = std::max(360, grid_->viewport()->width() - 32);
            const int NameWidth = layoutMode_ == 0 ? Width - 340 : Width;
            const int SizeWidth = layoutMode_ == 0 ? 96 : Width / 4;
            const int DateWidth = layoutMode_ == 0 ? 172 : Width / 2;
            const int AttrWidth = layoutMode_ == 0 ? 72 : Width - SizeWidth - DateWidth;
            view_->beginUpdate();
            for (int Index = 0; Index < columns_.size(); ++Index)
                columns_[Index]->setWidth(QVector<int>{NameWidth, SizeWidth, DateWidth, AttrWidth}[Index]);
            view_->endUpdate(); view_->requestUpdateLayout(true);
        }
        ReadinessStyle* style_;
        Qtitan::Grid* grid_ = nullptr;
        Qtitan::GridBandedTableView* view_ = nullptr;
        QStandardItemModel* model_ = nullptr;
        QVector<Qtitan::GridBandedTableColumn*> columns_;
        QComboBox* theme_ = nullptr;
        QComboBox* mode_ = nullptr;
        QLineEdit* input_ = nullptr;
        int layoutMode_ = 0;
    };
}

class ReadinessTests final : public QObject
{
    Q_OBJECT
public:
    explicit ReadinessTests(ReadinessStyle* Style) : style_(Style) {}
private slots:
    void styleOwnershipAndWidgets()
    {
        QCOMPARE(Qtitan::CommonStyle::ensureStyle(), style_);
        QPointer<QStyle> Base = style_->baseStyle();
        QVERIFY(qobject_cast<oclero::qlementine::QlementineStyle*>(Base.data()));
        { ReadinessWindow Window(style_); Window.show(); QTest::qWait(100);
          Window.SetDark(true); Window.SetDark(false);
          QCOMPARE(QApplication::style(), style_);
          Window.Input()->selectAll();
          QInputMethodEvent Input; Input.setCommitString(QStringLiteral("한글 입력 완료"));
          QApplication::sendEvent(Window.Input(), &Input);
          QCOMPARE(Window.Input()->text(), QStringLiteral("한글 입력 완료"));
          auto* Menu = Window.menuBar()->actions().first()->menu();
          Menu->popup(Window.mapToGlobal(QPoint(12, 24))); QTest::qWait(30);
          QTest::keyClick(Menu, Qt::Key_Down); QVERIFY(Menu->activeAction());
          QTest::keyClick(Menu, Qt::Key_Escape); QVERIFY(!Menu->isVisible()); }
        QVERIFY(Base); QCOMPARE(Qtitan::CommonStyle::ensureStyle(), style_);
    }

    void layouts_data()
    {
        QTest::addColumn<bool>("Dark"); QTest::addColumn<int>("Mode");
        for (const auto Dark : {false, true}) for (int Mode = 0; Mode < 3; ++Mode)
            QTest::newRow(qPrintable(QString("%1-%2").arg(Dark ? "dark" : "light").arg(Mode))) << Dark << Mode;
    }
    void layouts()
    {
        QFETCH(bool, Dark); QFETCH(int, Mode);
        ReadinessWindow Window(style_); Window.show(); Window.SetDark(Dark); Window.SetLayout(Mode);
        QTest::qWait(150);
        const auto Name = Window.HeaderRect(0); const auto Size = Window.HeaderRect(1);
        const auto Date = Window.HeaderRect(2); const auto Attributes = Window.HeaderRect(3);
        QVERIFY(Name.isValid()); QVERIFY(Size.isValid());
        if (Mode == 0) { QCOMPARE(Name.top(), Size.top()); QVERIFY(Name.right() < Size.left()); }
        else
        {
            QCOMPARE(Name.left(), Size.left());
            QVERIFY2(std::abs(Name.width() - (Size.width() + Date.width() + Attributes.width())) <= 2, qPrintable(QString("name=%1 metadata=%2").arg(Name.width()).arg(Size.width()+Date.width()+Attributes.width())));
            QVERIFY(Mode == 1 ? Name.top() > Size.top() : Name.top() < Size.top());
            const auto NameCell = Window.CellRect(0); const auto SizeCell = Window.CellRect(1);
            QVERIFY(Mode == 1 ? NameCell.top() > SizeCell.top() : NameCell.top() < SizeCell.top());
        }
        QCOMPARE(Window.Model()->rowCount(), 3);
        QVariant BoundValue;
        QVERIFY(Window.Column(0)->dataBinding()->getValue(Window.Model()->index(1, 0), Qt::DisplayRole, BoundValue));
        QCOMPARE(BoundValue.toString(), Window.Model()->data(Window.Model()->index(1, 0)).toString());
        QCOMPARE(Window.Column(0)->editorRepository()->valueToText(BoundValue), BoundValue.toString());
        Window.View()->setFocusedRowIndex(1); Window.View()->selectRow(1);
        const auto Selected = Window.View()->selection()->selectedRowIndexes();
        QCOMPARE(Selected.size(), 1);
        Window.SetDark(!Dark); Window.SetLayout((Mode + 1) % 3); QTest::qWait(80);
        QCOMPARE(Window.View()->selection()->selectedRowIndexes(), Selected);
        QCOMPARE(Window.View()->focusedRowIndex(), 1);
        Window.SetDark(Dark); Window.SetLayout(Mode); QTest::qWait(80);
        for (const int Width : {740, 1200})
        {
            Window.resize(Width, 560); QTest::qWait(40);
            if (Mode != 0)
                QVERIFY(std::abs(Window.HeaderRect(0).width() - (Window.HeaderRect(1).width() +
                    Window.HeaderRect(2).width() + Window.HeaderRect(3).width())) <= 2);
        }
        Window.resize(1000, 560); QTest::qWait(40);
        const auto Evidence = qEnvironmentVariable("HC_P0_EVIDENCE");
        if (!Evidence.isEmpty())
        {
            QVERIFY(QDir().mkpath(Evidence));
            const auto Path = QDir(Evidence).filePath(QString("%1-%2.png").arg(Dark ? "dark" : "light").arg(Mode));
            QVERIFY(Window.grab().save(Path));
        }
        QVERIFY(style_->HeaderPaints() > 0);
    }

    void headerStateColors()
    {
        ReadinessWindow Window(style_);
        for (const auto Dark : {false, true})
        {
            Window.SetDark(Dark);
            for (int State = 0; State < 4; ++State)
            {
                QImage Image(60, 30, QImage::Format_ARGB32_Premultiplied); Image.fill(Qt::magenta);
                QStyleOptionHeader Option; Option.rect = Image.rect(); Option.state = QStyle::State_Enabled;
                if (State == 1) Option.state |= QStyle::State_MouseOver;
                if (State == 2) Option.state |= QStyle::State_Sunken;
                if (State == 3) Option.sortIndicator = QStyleOptionHeader::SortUp;
                QPainter Painter(&Image); style_->drawControl(QStyle::CE_HeaderSection, &Option, &Painter, Window.Grid()); Painter.end();
                const auto Colors = style_->Colors();
                const QColor Expected[] = {Colors.background_, Colors.hover_, Colors.pressed_, Colors.sorted_};
                QCOMPARE(Image.pixelColor(3, 3), Expected[State]);
                QCOMPARE(Image.pixelColor(59, 3), Colors.separator_);
            }
            const auto Icon = Qtitan::GridBase::themeManager()->getIcon(Qtitan::GridThemeManager::SortAscendingIcon).pixmap(16, 16).toImage();
            QVERIFY(!Icon.isNull()); QCOMPARE(Icon.pixelColor(8, 8), style_->Colors().indicator_);
        }
    }

    void keyboardAndEditor()
    {
        ReadinessWindow Window(style_); Window.show(); Window.SetLayout(1); QTest::qWait(120);
        Window.View()->setFocusedRowIndex(0);
        Window.View()->setCellFocus(0, 0);
        auto* Primitive = Window.View()->Qtitan::GridViewBase::layout();
        Primitive->setFocus();
        QTest::keyClick(Primitive->control(), Qt::Key_Down);
        QCOMPARE(Window.View()->focusedRowIndex(), 1);
        Window.View()->setCellFocus(1, 0);
        QVERIFY(Window.View()->showEditor());
        QVERIFY(Window.View()->activeEditor());
        auto* Editor = qobject_cast<QLineEdit*>(Window.View()->activeEditor()->getCellWidget());
        QVERIFY(Editor); Editor->setText(QStringLiteral("이름 변경 완료.txt"));
        QTest::keyClick(Editor, Qt::Key_End); QTest::keyClick(Editor, Qt::Key_Space); QTest::keyClick(Editor, Qt::Key_Backspace);
        QVERIFY(Window.View()->postEditor()); Window.View()->hideEditor();
        QCOMPARE(Window.Model()->data(Window.Model()->index(1, 0)).toString(), QStringLiteral("이름 변경 완료.txt"));
    }
private:
    ReadinessStyle* style_;
};

int main(int Argc, char** Argv)
{
#ifdef _MSC_VER
    // Unattended tests must report assertions instead of waiting on CRT dialogs.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    QApplication App(Argc, Argv);
    QCoreApplication::setApplicationName("hc-readiness-p0");
    auto* Base = new oclero::qlementine::QlementineStyle;
    Base->setAnimationsEnabled(false);
    auto* Style = new ReadinessStyle(Base);
    QApplication::setStyle(Style);
    Style->SetDark(false);
    qInfo() << "P0 Qlementine commit" << HC_P0_QLEMENTINE_COMMIT << "Qt" << qVersion()
            << "scale" << qEnvironmentVariable("QT_SCALE_FACTOR", "1");
    if (App.arguments().contains("--interactive"))
    {
        ReadinessWindow Window(Style); Window.show(); return App.exec();
    }
    ReadinessTests Tests(Style);
    return QTest::qExec(&Tests, Argc, Argv);
}

#include "main.moc"
