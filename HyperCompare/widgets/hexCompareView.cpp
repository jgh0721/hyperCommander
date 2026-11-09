#include "stdafx.h"
#include "hexCompareView.hpp"
/// 모듈
#include "comparePalette.hpp"
#include "compareSettings.hpp"
#include "runtime/saveRecovery.hpp"
#include "runtime/requestFile.hpp"
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/qtMessageDialog.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QtWidgets>
#include <QtConcurrent>
#include <QPrinter>
#include <QPrintDialog>
#include <cstring>
#include <fmstyle/ThemeManager.h>
Q_LOGGING_CATEGORY(hexViewLog, "hc.compare.hexview")
namespace hc::compare
{
    namespace
    {
        constexpr int PageRows = 96;
        qint64 fromPixel(int Pixel, int Height, qint64 Length)
        {
            if (Height <= 0 || Length <= 0) return 0; Pixel = std::clamp(Pixel, 0, Height);
            return (Length / Height) * Pixel + (Length % Height) * Pixel / Height;
        }
        QString address(qint64 Offset) { return QString::number(Offset, 16).toUpper().rightJustified(16, '0'); }
        std::pair<qint64, qint64> span(const BinaryComparison& Comparison, int Side, qint64 First, qint64 Last)
        {
            First = std::clamp(First, qint64(0), Comparison.length_); Last = std::clamp(Last, First, Comparison.length_);
            const auto Map = [&](qint64 Position) {
                if (Position == Comparison.length_) return Comparison.documents_[Side].size_;
                const auto R = Comparison.At(Position); return R.begin_[Side] + std::min(R.size_[Side], Position - R.position_);
            };
            return {Map(First), Map(Last)};
        }
        HexWork page(std::shared_ptr<BinaryComparison> Comparison, qint64 Top, int Width, int Filter, const Budget& Limits)
        {
            HexWork Result; Result.comparison_ = Comparison; Result.top_ = std::clamp(Top, qint64(0), Comparison->length_);
            qint64 Position = Result.top_; HexRun Run; Run.position_ = -1;
            std::array<QByteArray, 2> Cache; std::array<qint64, 2> CacheAt{-1, -1};
            while (Position < Comparison->length_ && Result.cells_.size() < size_t(PageRows * Width)) {
                Limits.Check(); if (Run.position_ < 0 || Position >= Run.position_ + Run.length_) Run = Comparison->At(Position);
                if ((Filter == 1 && !Run.changed_) || (Filter == 2 && Run.changed_)) { Position = Run.position_ + Run.length_; continue; }
                HexCell Cell; Cell.position_ = Position;
                for (int Side = 0; Side < 2; ++Side) {
                    const qint64 Within = Position - Run.position_;
                    Cell.marks_[Side] = !Run.changed_ ? 0 : Within >= Run.size_[Side] ? 8 : Within >= Run.size_[1-Side] ? (Side ? 2 : 4) : 1;
                    if (Within < Run.size_[Side]) {
                        const qint64 Offset = Run.begin_[Side] + Within; Cell.offsets_[Side] = Offset;
                        if (CacheAt[Side] < 0 || Offset < CacheAt[Side] || Offset >= CacheAt[Side] + Cache[Side].size()) {
                            CacheAt[Side] = Offset; Cache[Side] = ReadBinary(Comparison->documents_[Side], Offset, 65536, Limits);
                        }
                        Cell.bytes_[Side] = quint8(Cache[Side][Offset - CacheAt[Side]]);
                    }
                }
                Result.cells_.push_back(Cell); ++Position;
            }
            qint64 Low=0,High=Comparison->changeCount_;
            while(Low<High){Limits.Check();auto Mid=Low+(High-Low)/2;if(Comparison->Change(Mid).position_<Result.top_)Low=Mid+1;else High=Mid;}
            Result.changeFirst_=std::max(qint64(0),Low-1);
            for(qint64 I=Result.changeFirst_;I<std::min(Comparison->changeCount_,Result.changeFirst_+128);++I){Limits.Check();Result.changes_.push_back(Comparison->Change(I));}
            return Result;
        }
        QColor markColor(int Mark) { return CompareColor(Mark == 8 ? "barGap" : Mark == 2 ? "barAdded" : Mark == 4 ? "barRemoved" : "barModified", CompareIsDark()); }
        bool sameDocument(const BinaryDocument& A, const BinaryDocument& B)
        {
            if (A.size_ != B.size_ || A.pieces_.size() != B.pieces_.size()) return false;
            for (size_t I = 0; I < A.pieces_.size(); ++I) if (A.pieces_[I].backing_ != B.pieces_[I].backing_ || A.pieces_[I].offset_ != B.pieces_[I].offset_ || A.pieces_[I].size_ != B.pieces_[I].size_) return false;
            return true;
        }
    }
    class HexPane final : public QAbstractScrollArea
    {
    public:
        HexPane(HexCompareView* Owner, int Side) : QAbstractScrollArea(Owner), owner_(Owner), side_(Side)
        {
            setObjectName(QString("hexPane%1").arg(Side)); setAccessibleName(Side ? tr("오른쪽 Hex 바이트") : tr("왼쪽 Hex 바이트"));
            setFocusPolicy(Qt::StrongFocus); setFont(QFont("Cascadia Mono", 10)); setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
        }
    protected:
        void resizeEvent(QResizeEvent* Event) override { QAbstractScrollArea::resizeEvent(Event); geometry(); }
        void paintEvent(QPaintEvent*) override
        {
            geometry(); QPainter Painter(viewport()); Painter.fillRect(viewport()->rect(), palette().base()); Painter.setFont(font());
            Painter.translate(-horizontalScrollBar()->value(), 0); const int W = owner_->bytesPerRow(), H = rowHeight(), C = charWidth(), X = owner_->showAddresses_?18*C:0;
            Painter.fillRect(QRect(0, 0, (21+W*4)*C, H), palette().alternateBase()); Painter.setPen(palette().text().color());
            if(owner_->showAddresses_)Painter.drawText(4, fontMetrics().ascent()+3, "Offset");
            for (int I = 0; I < W; ++I) Painter.drawText(X+I*3*C, fontMetrics().ascent()+3, QString::number(I, 16).toUpper().rightJustified(2, '0'));
            Painter.drawText(X+(W*3+2)*C, fontMetrics().ascent()+3, "ASCII");
            const qint64 First = std::min(owner_->anchor_, owner_->cursor_), Last = std::max(owner_->anchor_, owner_->cursor_);
            for (size_t I = 0; I < owner_->cells_.size(); ++I) {
                int Row = int(I)/W, Col = int(I)%W, Y = (Row+1)*H; if (Y > viewport()->height()) break;
                const auto& Cell = owner_->cells_[I];
                if (!Col) {
                    if (Row%2) Painter.fillRect(QRect(0, Y, (21+W*4)*C, H), palette().alternateBase());
                    Painter.setPen(palette().text().color()); if(owner_->showAddresses_)Painter.drawText(4, Y+fontMetrics().ascent()+3, Cell.offsets_[side_] < 0 ? QString(16, '-') : address(Cell.offsets_[side_]));
                }
                const QRect Hex(X+Col*3*C-2,Y,3*C,H), Ascii(X+(W*3+2+Col)*C,Y,C,H);
                if (Cell.marks_[side_]) { QColor Color=markColor(Cell.marks_[side_]); Color.setAlpha(65); Painter.fillRect(Hex, Color); Painter.fillRect(Ascii, Color); }
                if (Cell.position_ >= First && Cell.position_ <= Last) {
                    auto Color=palette().highlight().color(); Color.setAlpha(owner_->active_==side_ ? 140:55); Painter.fillRect(Hex, Color); Painter.fillRect(Ascii, Color);
                }
                Painter.setPen(palette().text().color()); const int B=Cell.bytes_[side_];
                Painter.drawText(Hex.x()+2,Y+fontMetrics().ascent()+3,B<0 ? "--" : QString::number(B,16).toUpper().rightJustified(2,'0'));
                Painter.drawText(Ascii.x(),Y+fontMetrics().ascent()+3,B<0 ? " " : B>=32 && B<127 ? QString(QChar(B)) : ".");
                if (Cell.marks_[side_]) { Painter.setPen(QPen(markColor(Cell.marks_[side_]),2)); Painter.drawLine(Hex.bottomLeft(),Hex.bottomRight()); }
                if (Cell.position_==owner_->cursor_ && owner_->active_==side_) { Painter.setPen(palette().highlight().color()); Painter.drawRect(Hex.adjusted(0,0,-1,-1)); }
            }
            if (owner_->cells_.empty()) { Painter.setPen(palette().text().color()); Painter.drawText(8,H*2,owner_->Busy()?tr("읽는 중…"):tr("표시할 바이트 없음 / EOF")); }
        }
        void focusOutEvent(QFocusEvent* Event) override { nibble_=-1; QAbstractScrollArea::focusOutEvent(Event); }
        void focusInEvent(QFocusEvent* Event) override { QAbstractScrollArea::focusInEvent(Event); owner_->active_=side_; owner_->refresh(); }
        void wheelEvent(QWheelEvent* Event) override { owner_->showPage(owner_->top_ - qint64(Event->angleDelta().y()/120)*owner_->bytesPerRow()*3); Event->accept(); }
        void mousePressEvent(QMouseEvent* Event) override { setFocus(); hit(Event->position().toPoint(),Event->modifiers().testFlag(Qt::ShiftModifier)); }
        void mouseMoveEvent(QMouseEvent* Event) override { if(Event->buttons().testFlag(Qt::LeftButton)) hit(Event->position().toPoint(),true); }
        void mouseDoubleClickEvent(QMouseEvent*) override
        {
            if (owner_->Busy() || owner_->readOnly_->isChecked()) return;
            bool Ok=false; auto Text=QInputDialog::getText(this,tr("바이트 편집"),tr("Hex 바이트 (예: 01 FF 20)"),QLineEdit::Normal,{},&Ok);
            if (Ok) { try { owner_->editBytes(owner_->searchBytes("hex:"+Text)); } catch(const std::exception& E) { owner_->status_->setText(E.what()); } }
        }
        void keyPressEvent(QKeyEvent* Event) override
        {
            qint64 Next=owner_->cursor_; int Width=owner_->bytesPerRow(); bool Handled=true;
            switch(Event->key()) {
            case Qt::Key_Left: --Next; break; case Qt::Key_Right: ++Next; break;
            case Qt::Key_Up: Next-=Width; break; case Qt::Key_Down: Next+=Width; break;
            case Qt::Key_PageUp: Next-=Width*std::max(1,viewport()->height()/rowHeight()-2); break;
            case Qt::Key_PageDown: Next+=Width*std::max(1,viewport()->height()/rowHeight()-2); break;
            case Qt::Key_Home: Next=0; break; case Qt::Key_End: Next=owner_->comparison_->length_; break;
            case Qt::Key_Delete: owner_->editBytes({},true); return;
            default: Handled=false; break;
            }
            if (Event->matches(QKeySequence::SelectAll)) { owner_->anchor_=0; owner_->cursor_=std::max(qint64(0),owner_->comparison_->length_-1); owner_->refresh(); return; }
            if (Event->matches(QKeySequence::Copy)) { owner_->copyClipboard(false); return; }
            if (Event->matches(QKeySequence::Cut)) { owner_->copyClipboard(true); return; }
            if (Event->matches(QKeySequence::Paste)) { owner_->pasteClipboard(); return; }
            if (Handled) { nibble_=-1; Next=std::clamp(Next,qint64(0),owner_->comparison_->length_); owner_->select(Next,side_,Event->modifiers().testFlag(Qt::ShiftModifier)); owner_->showPage(std::max(qint64(0),Next-Width*3)); return; }
            if(!owner_->readOnly_->isChecked()&&!owner_->Busy()&&!(Event->modifiers()&(Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier))&&!Event->text().isEmpty()){
                if(ascii_){owner_->editBytes(Event->text().toUtf8());return;}
                const auto Text=Event->text().toLower();if(Text.size()==1&&QString("0123456789abcdef").contains(Text[0])){int Value=QString("0123456789abcdef").indexOf(Text[0]);if(nibble_<0){nibble_=Value;owner_->status_->setText(tr("상위 nibble %1 · 다음 16진수 자릿수를 입력하세요").arg(Text));}else{owner_->editBytes(QByteArray(1,char(nibble_*16+Value)));nibble_=-1;}return;}
            }
            QAbstractScrollArea::keyPressEvent(Event);
        }
    private:
        HexCompareView* owner_; int side_; bool ascii_ = false; int nibble_ = -1;
        int charWidth() const { return std::max(1,fontMetrics().horizontalAdvance('0')); }
        int rowHeight() const { return fontMetrics().height()+6; }
        void geometry() { horizontalScrollBar()->setRange(0,std::max(0,(21+owner_->bytesPerRow()*4)*charWidth()-viewport()->width())); }
        void hit(QPoint Point,bool Extend)
        {
            const int W=owner_->bytesPerRow(), C=charWidth(), X=Point.x()+horizontalScrollBar()->value()-(owner_->showAddresses_?18*C:0), Row=Point.y()/rowHeight()-1;
            int Col=X>=(W*3+2)*C ? X/C-(W*3+2) : X/(3*C);
            ascii_=X>=(W*3+2)*C;nibble_=-1;
            if(X<0 || Row<0 || Col<0 || Col>=W) return; const size_t Index=size_t(Row*W+Col);
            if(Index<owner_->cells_.size()) owner_->select(owner_->cells_[Index].position_,side_,Extend);
        }
    };
    class HexOverviewBar final : public QWidget
    {
    public:
        explicit HexOverviewBar(HexCompareView* Owner):QWidget(Owner),owner_(Owner)
        { setObjectName("hexOverview");setFixedWidth(72);setFocusPolicy(Qt::StrongFocus);setAccessibleName(tr("Hex 원본 1 / 2 차이 개요"));setToolTip(tr("클릭·드래그: 위치 이동 · 좌우: 원본 선택 · 위아래/PageUp/Down/Home/End: 이동 · Escape: 본문")); }
    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter P(this);P.fillRect(rect(),palette().window()); const auto& C=*owner_->comparison_; const int H=std::max(1,height()-24);
            for(int Side=0;Side<2;++Side){int X=Side*34+5; P.setPen(palette().text().color()); P.drawText(X,16,QString::number(Side+1));P.fillRect(X,24,25,H,CompareColor("barTrack",CompareIsDark()));
                for(int Y=0;Y<H;++Y){int Begin=int(qint64(Y)*4096/H),End=std::min(4096,int((qint64(Y+1)*4096+H-1)/H));int Mask=0;for(int I=Begin;I<End;++I)Mask|=C.overview_[Side][I];
                    int N=0;for(int Bit:{1,2,4,8})if(Mask&Bit)++N;int Column=0;
                    for(int Bit:{1,2,4,8})if(Mask&Bit){QRect R(X+Column*25/N,Y+24,std::max(1,25/N),1);P.fillRect(R,QBrush(markColor(Bit),Bit==8?Qt::BDiagPattern:Qt::SolidPattern));++Column;}
                }
                int Visible=std::max(1,owner_->panes_[Side]->viewport()->height()/(owner_->panes_[Side]->fontMetrics().height()+6)-1)*owner_->bytesPerRow();
                // Separate segments keep filtered-out spans out of the viewport overlay.
                qint64 Start=-1,End=-1; const auto Draw=[&]{if(Start<0)return;int A=24+int(HexScale(Start,std::max(qint64(1),C.length_),H-1));int Z=24+int(HexScale(End,std::max(qint64(1),C.length_),H-1));P.setPen(QPen(CompareColor("barViewport",CompareIsDark()),owner_->active_==Side?2:1));P.drawRect(QRect(X-2,A,29,std::max(2,Z-A)));};
                for(size_t I=0;I<std::min(size_t(Visible),owner_->cells_.size());++I){qint64 At=owner_->cells_[I].position_;if(End!=At){Draw();Start=At;}End=At+1;}Draw();
            }
        }
        void mousePressEvent(QMouseEvent* E) override {setFocus();navigate(E->position().toPoint());}
        void mouseMoveEvent(QMouseEvent* E) override {if(E->buttons().testFlag(Qt::LeftButton))navigate(E->position().toPoint());}
        void keyPressEvent(QKeyEvent* E) override
        {
            if(E->key()==Qt::Key_Escape){owner_->panes_[owner_->active_]->setFocus();return;}
            if(E->key()==Qt::Key_Left||E->key()==Qt::Key_Right){owner_->active_=E->key()==Qt::Key_Left?0:1;update();return;}
            qint64 At=owner_->top_;switch(E->key()){case Qt::Key_Home:At=0;break;case Qt::Key_End:At=owner_->comparison_->length_;break;case Qt::Key_Up:At-=owner_->bytesPerRow();break;case Qt::Key_Down:At+=owner_->bytesPerRow();break;case Qt::Key_PageUp:At-=owner_->bytesPerRow()*20;break;case Qt::Key_PageDown:At+=owner_->bytesPerRow()*20;break;default:QWidget::keyPressEvent(E);return;}owner_->showPage(At);
        }
    private:
        HexCompareView* owner_;
        void navigate(QPoint P){owner_->active_=P.x()<36?0:1;owner_->filter_->blockSignals(true);owner_->filter_->setCurrentIndex(0);owner_->filter_->blockSignals(false);owner_->showPage(fromPixel(P.y()-24,std::max(1,height()-25),owner_->comparison_->length_));}
    };
    void HexCompareView::bindAction(QMenu* Menu,const QString& Text,const QString& Key,std::function<void()> Slot)
    { auto* Action=Menu->addAction(Text);if(!Key.isEmpty()){Action->setShortcut(QKeySequence(Key));Action->setShortcutContext(Qt::WidgetWithChildrenShortcut);}connect(Action,&QAction::triggered,this,std::move(Slot)); }
    HexCompareView::HexCompareView(const Request& Input,std::shared_ptr<BinaryComparison> Comparison,QWidget* Parent)
        :ExtensionView(Parent),request_(Input),comparison_(std::move(Comparison)),saved_(comparison_->documents_),canceled_(std::make_shared<std::atomic_bool>(false))
    {
        unlockedMiB_=Preferences{}.Contains("hex/unlockedMiB")?Preferences{}.Value("hex/unlockedMiB").toLongLong():-1;
        setObjectName("hc.compare.hex"); auto* Root=new QVBoxLayout(this);Root->setContentsMargins(6,6,6,6);Root->setSpacing(5);
        auto* Menus=new QMenuBar;Root->addWidget(Menus);auto* File=Menus->addMenu(tr("Hex 파일"));auto* Edit=Menus->addMenu(tr("편집"));auto* Search=Menus->addMenu(tr("검색"));auto* Session=Menus->addMenu(tr("세션"));auto* View=Menus->addMenu(tr("보기"));
        bindAction(File,tr("왼쪽 파일 형식…"),{},[this]{format(0);});bindAction(File,tr("오른쪽 파일 형식…"),{},[this]{format(1);});
        bindAction(File,tr("왼쪽 열기…"),{},[this]{open(0);});bindAction(File,tr("오른쪽 열기…"),{},[this]{open(1);});bindAction(File,tr("클립보드 열기"),{},[this]{open(active_,true);});
        bindAction(File,tr("저장"),{},[this]{save(false);});bindAction(File,tr("다른 이름으로 저장…"),"Ctrl+Shift+S",[this]{save(true);});bindAction(File,tr("파일 새로 읽기"),{},[this]{reload();});
        bindAction(File,tr("외부 프로그램으로 열기"),{},[this]{QDesktopServices::openUrl(QUrl::fromLocalFile(comparison_->documents_[active_].original_.path_));});
        bindAction(File,tr("부모 폴더 비교"),{},[this]{if(!openComparison_||Busy())return;Request Input;Input.mode_="directory";Input.file1_=QFileInfo(comparison_->documents_[0].original_.path_).absolutePath();Input.file2_=QFileInfo(comparison_->documents_[1].original_.path_).absolutePath();openComparison_(Input);});
        bindAction(File,tr("자동 형식으로 비교"),{},[this]{if(!openComparison_||Busy()||Dirty())return;Request Input;Input.file1_=comparison_->documents_[0].original_.path_;Input.file2_=comparison_->documents_[1].original_.path_;Input.readOnlyInputs_=request_.readOnlyInputs_;Input.resourceLifetime_=request_.resourceLifetime_;openComparison_(Input);});
        if(request_.nextDifferenceFile_){
            bindAction(File,tr("다음 차이 파일"),{},[this]{if(!Busy()&&!Dirty())request_.nextDifferenceFile_(1);});
            bindAction(File,tr("이전 차이 파일"),{},[this]{if(!Busy()&&!Dirty())request_.nextDifferenceFile_(-1);});
            bindAction(File,tr("왼쪽 파일을 오른쪽에 복사하고 다음 차이"),{},[this]{copyFileAndNext(0);});
            bindAction(File,tr("오른쪽 파일을 왼쪽에 복사하고 다음 차이"),{},[this]{copyFileAndNext(1);});
        }
        bindAction(File,tr("Hex 보고서…"),{},[this]{ExportReport();});
        bindAction(File,tr("Hex 보고서 인쇄…"),"Ctrl+P",[this]{PrintReport();});
        bindAction(Edit,tr("실행 취소"),{},[this]{Undo(false);});bindAction(Edit,tr("다시 실행"),{},[this]{Undo(true);});bindAction(Edit,tr("잘라내기"),{},[this]{copyClipboard(true);});bindAction(Edit,tr("복사"),{},[this]{copyClipboard(false);});bindAction(Edit,tr("붙여넣기"),{},[this]{pasteClipboard();});bindAction(Edit,tr("삭제"),{},[this]{editBytes({},true);});
        bindAction(Search,tr("다음 차이"),{},[this]{NavigateDifference(1);});bindAction(Search,tr("이전 차이"),{},[this]{NavigateDifference(-1);});bindAction(Search,tr("찾기"),{},[this]{ShowFind();});bindAction(Search,tr("다음 찾기"),{},[this]{findNext(false,false);});bindAction(Search,tr("이전 찾기"),{},[this]{findNext(true,false);});bindAction(Search,tr("주소 이동"),"Ctrl+G",[this]{address_->setFocus();address_->selectAll();});
        bindAction(Session,tr("형식 감지 파일 마스크…"),{},[this]{
            for(const auto& Kind:{QString("hex"),QString("base64")}){bool Ok=false;auto Masks=QInputDialog::getText(this,tr("파일 마스크"),Kind+tr(" 형식 (예: *.hex;*.hextext)"),QLineEdit::Normal,Preferences{}.Value("hex/masks/"+Kind).toString(),&Ok);if(!Ok)return;Preferences{}.SetValue("hex/masks/"+Kind,Masks);}
        });
        bindAction(Session,tr("원본 파일 잠금 정책…"),{},[this]{if(Busy())return;bool Ok=false;auto N=QInputDialog::getInt(this,tr("원본 파일 잠금"),tr("이 크기(MiB) 이하의 파일은 잠그지 않음\n-1: 모든 파일을 사본으로 비교하고 저장 시 충돌 검사"),int(unlockedMiB_),-1,1048576,1,&Ok);if(Ok){unlockedMiB_=N;Preferences{}.SetValue("hex/unlockedMiB",N);showPage(top_);}});
        bindAction(Session,tr("세션 저장…"),{},[this]{session(false);});bindAction(Session,tr("세션 열기…"),{},[this]{session(true);});
        bindAction(Session,tr("좌우 교환"),{},[this]{if(Busy()||Dirty()||locked_)return;auto D=comparison_->documents_;std::swap(D[0],D[1]);auto Mode=alignment_->currentText();work([D,Mode](const Budget& B){HexWork W;W.comparison_=CompareBinary(D,Mode,B);W.saved_=true;W.savedSide_=2;return W;});});
        bindAction(Session,tr("세션 비우기"),{},[this]{if(Busy()||locked_)return;if(Dirty()&&hc::ui::MessageDialog::question(this,tr("세션 비우기"),tr("저장하지 않은 변경을 버리시겠습니까?"))!=QMessageBox::Yes)return;work([](const Budget& B){HexWork W;W.comparison_=CompareBinary({BinaryFromBytes({}),BinaryFromBytes({})},"none",B);W.saved_=true;W.savedSide_=2;return W;});});
        bindAction(Session,tr("재비교 (편집 유지)"),{},[this]{recompare();});
        auto* Lock=Session->addAction(tr("세션 잠금"));Lock->setCheckable(true);connect(Lock,&QAction::toggled,this,[this](bool On){locked_=On;readOnly_->setChecked(On||request_.readOnlyInputs_);readOnly_->setEnabled(!On&&!request_.readOnlyInputs_);});
        auto* Addresses=View->addAction(tr("바이트 주소"));Addresses->setCheckable(true);Addresses->setChecked(true);connect(Addresses,&QAction::toggled,this,[this](bool On){showAddresses_=On;refresh();});
        auto* Info=View->addAction(tr("파일 정보"));Info->setCheckable(true);Info->setChecked(true);connect(Info,&QAction::toggled,this,[this](bool On){for(auto* Label:paths_)Label->setVisible(On);});
        auto* Differences=View->addAction(tr("차이 목록"));Differences->setCheckable(true);connect(Differences,&QAction::toggled,this,[this](bool On){differenceList_->setVisible(On);Preferences{}.SetValue("hex/differenceList",On);});
        bindAction(View,tr("글꼴…"),{},[this]{bool Ok=false;auto Font=QFontDialog::getFont(&Ok,panes_[0]->font(),this);if(Ok)SetTextFont(Font);});
        bindAction(View,tr("확대"),"Ctrl++",[this]{auto F=panes_[0]->font();F.setPointSize(std::min(32,F.pointSize()+1));SetTextFont(F);});bindAction(View,tr("축소"),"Ctrl+-",[this]{auto F=panes_[0]->font();F.setPointSize(std::max(7,F.pointSize()-1));SetTextFont(F);});
        bindAction(View,tr("글꼴 크기 초기화"),{},[this]{auto F=panes_[0]->font();F.setPointSize(10);SetTextFont(F);});
        auto* Toolbar=View->addAction(tr("도구 모음"));Toolbar->setCheckable(true);Toolbar->setChecked(true);connect(Toolbar,&QAction::toggled,this,[this](bool On){toolbar_->setVisible(On);});
        toolbar_=new QToolBar;Root->addWidget(toolbar_);
        const auto Tool=[&](const QString& Text,std::function<void()> Slot){auto* A=toolbar_->addAction(Text);connect(A,&QAction::triggered,this,std::move(Slot));};
        Tool(tr("이전 차이"),[this]{NavigateDifference(-1);});Tool(tr("다음 차이"),[this]{NavigateDifference(1);});Tool(tr("← 복사"),[this]{copyAcross(1);});Tool(tr("복사 →"),[this]{copyAcross(0);});Tool(tr("저장"),[this]{save(false);});Tool(tr("취소"),[this]{Cancel();});
        alignment_=new QComboBox;alignment_->setObjectName("hexAlignment");alignment_->addItems({"none","fast","complete"});alignment_->setCurrentText(comparison_->mode_);alignment_->setToolTip(tr("None: 동일 오프셋 · Fast: 빠른 앵커 정렬 · Complete: 조밀한 앵커 정렬"));toolbar_->addWidget(alignment_);
        width_=new QComboBox;width_->addItems({"8 bytes","16 bytes","32 bytes"});width_->setCurrentIndex(Preferences{}.Contains("hex/width")?std::clamp(Preferences{}.Value("hex/width").toInt(),0,2):1);toolbar_->addWidget(width_);
        filter_=new QComboBox;filter_->setObjectName("hexFilter");filter_->addItems({tr("전체"),tr("차이"),tr("동일")});toolbar_->addWidget(filter_);
        layout_=new QComboBox;layout_->addItems({tr("좌우"),tr("상하")});toolbar_->addWidget(layout_);
        readOnly_=new QCheckBox(tr("읽기 전용"));readOnly_->setObjectName("hexReadOnly");readOnly_->setChecked(true);readOnly_->setEnabled(!request_.readOnlyInputs_);toolbar_->addWidget(readOnly_);insert_=new QCheckBox(tr("삽입"));toolbar_->addWidget(insert_);
        auto* Body=new QHBoxLayout;Root->addLayout(Body,1);overview_=new HexOverviewBar(this);Body->addWidget(overview_);splitter_=new QSplitter;Body->addWidget(splitter_,1);
        for(int S=0;S<2;++S){auto* Pane=new QWidget;auto* Box=new QVBoxLayout(Pane);Box->setContentsMargins(0,0,0,0);paths_[S]=new QLabel;paths_[S]->setTextInteractionFlags(Qt::TextSelectableByMouse);Box->addWidget(paths_[S]);panes_[S]=new HexPane(this,S);Box->addWidget(panes_[S]);splitter_->addWidget(Pane);}
        differenceList_=new QListWidget;differenceList_->setObjectName("hexDifferenceList");differenceList_->setAccessibleName(tr("차이 목록 (현재 위치부터 128구간)"));differenceList_->setMaximumWidth(310);Body->addWidget(differenceList_);differenceList_->setVisible(Preferences{}.Value("hex/differenceList").toBool());Differences->setChecked(!differenceList_->isHidden());
        connect(differenceList_,&QListWidget::itemActivated,this,[this](QListWidgetItem* Item){if(Busy())return;auto At=Item->data(Qt::UserRole).toLongLong();select(At,active_,false);showPage(At);});
        scroll_=new QScrollBar(Qt::Vertical);scroll_->setRange(0,1000000);Body->addWidget(scroll_);
        auto* FindRow=new QHBoxLayout;Root->addLayout(FindRow);searchMode_=new QComboBox;searchMode_->addItems({"Hex","UTF-8"});FindRow->addWidget(searchMode_);find_=new QLineEdit;find_->setObjectName("hexFind");find_->setPlaceholderText(tr("검색"));FindRow->addWidget(find_);replace_=new QLineEdit;replace_->setPlaceholderText(tr("치환"));FindRow->addWidget(replace_);
        const auto Button=[&](const QString& Text,std::function<void()> Slot){auto* B=new QPushButton(Text);FindRow->addWidget(B);connect(B,&QPushButton::clicked,this,std::move(Slot));};
        Button(tr("이전"),[this]{findNext(true,false);});Button(tr("다음"),[this]{findNext(false,false);});Button(tr("치환"),[this]{findNext(false,true);});Button(tr("모두 치환"),[this]{findNext(false,true,true);});
        address_=new QLineEdit;address_->setMaximumWidth(180);address_->setPlaceholderText(tr("0x주소 / 10진수"));FindRow->addWidget(address_);Button(tr("이동"),[this]{gotoAddress();});connect(address_,&QLineEdit::returnPressed,this,&HexCompareView::gotoAddress);
        status_=new QLabel;status_->setWordWrap(true);status_->setTextInteractionFlags(Qt::TextSelectableByMouse);Root->addWidget(status_);
        connect(alignment_,&QComboBox::currentTextChanged,this,[this]{if(!rebuilding_)recompare();});connect(width_,&QComboBox::currentIndexChanged,this,[this](int I){Preferences{}.SetValue("hex/width",I);showPage(top_);});connect(filter_,&QComboBox::currentIndexChanged,this,[this]{showPage(top_);});
        connect(layout_,&QComboBox::currentIndexChanged,this,[this](int I){splitter_->setOrientation(I?Qt::Vertical:Qt::Horizontal);Preferences{}.SetValue("hex/layout",I);});layout_->setCurrentIndex(Preferences{}.Value("hex/layout").toInt()==1?1:0);
        connect(scroll_,&QScrollBar::valueChanged,this,[this](int Value){if(!rebuilding_)showPage(fromPixel(Value,1000000,comparison_->length_));});
        connect(readOnly_,&QCheckBox::toggled,this,[this]{if(stateChanged_)stateChanged_();});
        connect(&watcher_,&QFutureWatcher<HexWork>::finished,this,[this]{
            working_=false;auto Result=watcher_.result();if(!Result.error_.isEmpty()&&!Result.saved_)status_->setText(Result.error_);else{
                if(Result.history_==1){undo_.push_back(comparison_->documents_);redo_.clear();if(undo_.size()>64)undo_.erase(undo_.begin());}
                else if(Result.history_==2){redo_.push_back(comparison_->documents_);undo_.pop_back();}
                else if(Result.history_==3){undo_.push_back(comparison_->documents_);redo_.pop_back();}
                if(Result.comparison_){comparison_=std::move(Result.comparison_);if(Result.saved_&&Result.savedSide_>=0){if(Result.savedSide_==2)saved_=comparison_->documents_;else saved_[Result.savedSide_]=comparison_->documents_[Result.savedSide_];undo_.clear();redo_.clear();}}
                cells_=std::move(Result.cells_);changes_=std::move(Result.changes_);changeFirst_=Result.changeFirst_;top_=Result.top_;if(Result.select_&&Result.found_>=0)cursor_=anchor_=Result.found_;
                cursor_=std::min(cursor_,comparison_->length_);anchor_=std::min(anchor_,comparison_->length_);
                refresh();if(!Result.error_.isEmpty())status_->setText(tr("저장 완료. 화면 갱신 실패: ")+Result.error_);else if(!Result.message_.isEmpty())status_->setText(Result.message_);
                if(Result.saved_&&Result.message_=="hex.copy.next"&&request_.nextDifferenceFile_)request_.nextDifferenceFile_(1);
            }
            if(Result.error_.isEmpty()&&Result.clipboardReady_){auto* Mime=new QMimeData;Mime->setData("application/octet-stream",Result.clipboard_);Mime->setText(QString::fromLatin1(Result.clipboard_.toHex(' ')));QApplication::clipboard()->setMimeData(Mime);if(Result.cut_)editBytes({},true);}
            if(stateChanged_)stateChanged_();if(pendingTop_>=0){const auto Next=std::exchange(pendingTop_,qint64(-1));showPage(Next);}
        });
        connect(&fm::style::ThemeManager::instance(),&fm::style::ThemeManager::changed,this,[this]{refresh();});
        SetOverviewVisible(Preferences{}.Value("view/overview").toBool());showPage(0);
    }
    HexCompareView::~HexCompareView(){Cancel();watcher_.waitForFinished();}
    int HexCompareView::bytesPerRow()const{return 8<<width_->currentIndex();}
    bool HexCompareView::Busy()const{return working_;}
    bool HexCompareView::Dirty()const{return !sameDocument(comparison_->documents_[0],saved_[0])||!sameDocument(comparison_->documents_[1],saved_[1]);}
    void HexCompareView::Cancel(){canceled_->store(true);pendingTop_=-1;}
    bool HexCompareView::CanUndo(bool Redo)const{return !Busy()&&!readOnly_->isChecked()&&!(Redo?redo_:undo_).empty();}
    bool HexCompareView::CanSave()const{return !Busy()&&!locked_&&!request_.readOnlyInputs_&&Dirty();}
    void HexCompareView::SaveActive(){save(false);}
    void HexCompareView::SetTextFont(const QFont& Font){for(auto* Pane:panes_)if(Pane){Pane->setFont(Font);Pane->viewport()->update();}}
    void HexCompareView::SetOverviewVisible(bool Visible){if(!Visible&&overview_->hasFocus())panes_[active_]->setFocus();overview_->setVisible(Visible);}
    void HexCompareView::work(std::function<HexWork(const Budget&)> Operation)
    {
        if(Busy())return;working_=true;canceled_=std::make_shared<std::atomic_bool>(false);auto Canceled=canceled_;int Timeout=request_.timeoutMs_;int Width=bytesPerRow(),Filter=filter_->currentIndex();auto Current=comparison_;auto Unlocked=unlockedMiB_;
        status_->setText(tr("처리 중… (취소 가능)"));
        watcher_.setFuture(QtConcurrent::run([Operation=std::move(Operation),Canceled,Timeout,Width,Filter,Current,Unlocked]{
            HexWork Result;try{Budget B;B.canceled_=Canceled.get();B.deadline_=std::chrono::steady_clock::now()+std::chrono::milliseconds(Timeout);Result=Operation(B);
                if(!Result.comparison_)Result.comparison_=Current;Result.comparison_=std::make_shared<BinaryComparison>(*Result.comparison_);for(auto& Doc:Result.comparison_->documents_)SetBinarySourceLock(Doc,Unlocked,B);auto PageBudget=B;if(Result.saved_){PageBudget.canceled_=nullptr;PageBudget.deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(10);}auto Page=page(Result.comparison_,Result.top_,Width,Filter,PageBudget);Result.cells_=std::move(Page.cells_);Result.changes_=std::move(Page.changes_);Result.changeFirst_=Page.changeFirst_;Result.top_=Page.top_;
            }catch(const SaveRecoveryFailure& E){Result.error_=QString::fromUtf8(E.what())+"\n"+QString::fromUtf8(QJsonDocument(E.recovery_).toJson(QJsonDocument::Compact));}catch(const std::exception& E){Result.error_=QString::fromUtf8(E.what());}return Result;
        }));if(stateChanged_)stateChanged_();
    }
    void HexCompareView::showPage(qint64 Top){Top=std::clamp(Top,qint64(0),comparison_->length_);if(Busy()){pendingTop_=Top;return;}work([Top](const Budget&){HexWork W;W.top_=Top;return W;});}
    void HexCompareView::refresh()
    {
        rebuilding_=true;scroll_->setValue(int(HexScale(top_,std::max(qint64(1),comparison_->length_),1000000)));alignment_->setCurrentText(comparison_->mode_);rebuilding_=false;
        for(int S=0;S<2;++S){const auto& D=comparison_->documents_[S];paths_[S]->setText(tr("%1 · %2%3 · %4 bytes").arg(S+1).arg(D.original_.path_.isEmpty()?tr("클립보드"):D.original_.path_+" ["+(D.format_.startsWith("external:")?QString("external"):D.format_)+"]",sameDocument(D,saved_[S])?"":" *").arg(D.size_));panes_[S]->viewport()->update();}
        differenceList_->clear();for(size_t I=0;I<changes_.size();++I){const auto& R=changes_[I];auto* Item=new QListWidgetItem(tr("#%1 · %2 / %3 bytes\nL %4  R %5").arg(changeFirst_+qint64(I)+1).arg(R.size_[0]).arg(R.size_[1]).arg(address(R.begin_[0]),address(R.begin_[1])),differenceList_);Item->setData(Qt::UserRole,R.position_);}
        QString Values;for(const auto& C:cells_)if(C.position_==cursor_){for(int S=0;S<2;++S){Values+=tr("  %1: %2").arg(S+1).arg(C.offsets_[S]<0?tr("gap"):"0x"+address(C.offsets_[S]));if(C.bytes_[S]>=0)Values+=QString(" UInt8=%1").arg(C.bytes_[S]);}break;}
        quint64 LE=0,BE=0;int N=0;qint64 NextOffset=-1;for(const auto& C:cells_)if(C.position_>=cursor_){if(N==8||C.position_-cursor_!=N||C.bytes_[active_]<0||(NextOffset>=0&&C.offsets_[active_]!=NextOffset))break;LE|=quint64(C.bytes_[active_])<<(N*8);BE=(BE<<8)|quint32(C.bytes_[active_]);NextOffset=C.offsets_[active_]+1;++N;}
        status_->setText(tr("차이 %1구간 · %2 bytes · %3 · 선택 %4 bytes%5   UInt%6 LE=%7 / BE=%8").arg(comparison_->changeCount_).arg(comparison_->different_).arg(comparison_->mode_).arg(std::abs(cursor_-anchor_)+1).arg(Values).arg(N*8).arg(LE).arg(BE));
        QString Detail=tr("선택 바이트 해석\nUInt8: %1 / Int8: %2\nUInt16 LE: %3 / UInt32 LE: %4\nUInt64 LE: %5 / BE: %6").arg(LE&255).arg(qint8(LE&255)).arg(LE&65535).arg(LE&0xffffffff).arg(LE).arg(BE);
        if(N>=4){quint32 Bits=quint32(LE);float Value;std::memcpy(&Value,&Bits,sizeof(Value));Detail+=QString("\nFloat32 LE: %1").arg(double(Value));}
        if(N==8){double Value;std::memcpy(&Value,&LE,sizeof(Value));Detail+=QString("\nFloat64 LE: %1").arg(Value);}
        status_->setToolTip(Detail);for(auto* Pane:panes_)Pane->setAccessibleDescription(status_->text());
        overview_->setAccessibleDescription(status_->text());overview_->update();
    }
    void HexCompareView::select(qint64 Position,int Side,bool Extend){if(Busy())return;active_=Side;cursor_=Position;if(!Extend)anchor_=Position;refresh();}
    void HexCompareView::Undo(bool Redo)
    {
        if(!CanUndo(Redo))return;auto& From=Redo?redo_:undo_;auto Documents=From.back();auto Mode=alignment_->currentText();auto Top=top_;
        work([Documents,Mode,Top,Redo](const Budget& B){HexWork W;W.history_=Redo?3:2;W.comparison_=CompareBinary(Documents,Mode,B);W.top_=Top;return W;});
    }
    void HexCompareView::recompare(){if(Busy())return;auto Documents=comparison_->documents_;auto Mode=alignment_->currentText();auto Top=top_;work([Documents,Mode,Top](const Budget& B){HexWork W;W.comparison_=CompareBinary(Documents,Mode,B);W.top_=Top;return W;});}
    void HexCompareView::NavigateDifference(int Direction)
    {
        if(Busy()||!comparison_->changeCount_)return;auto C=comparison_;auto Cursor=cursor_;filter_->blockSignals(true);filter_->setCurrentIndex(0);filter_->blockSignals(false);
        work([C,Cursor,Direction](const Budget& B){qint64 Low=0,High=C->changeCount_;while(Low<High){B.Check();qint64 Mid=Low+(High-Low)/2;auto R=C->Change(Mid);if(R.position_<(Direction>0?Cursor+1:Cursor))Low=Mid+1;else High=Mid;}
            qint64 Index=Direction>0?(Low==C->changeCount_?0:Low):(Low?Low-1:C->changeCount_-1);HexWork W;W.top_=W.found_=C->Change(Index).position_;W.select_=true;return W;});
    }
    void HexCompareView::ShowFind(){find_->setFocus();find_->selectAll();}
    QByteArray HexCompareView::searchBytes(const QString& Text)const
    {
        if(searchMode_->currentIndex()==1&&!Text.startsWith("hex:"))return Text.toUtf8();QString Hex=Text.startsWith("hex:")?Text.mid(4):Text;Hex.remove(QRegularExpression("\\s"));
        if(Hex.size()%2||Hex.contains(QRegularExpression("[^a-fA-F0-9]")))throw Failure("InvalidHexBytes");return QByteArray::fromHex(Hex.toLatin1());
    }
    void HexCompareView::editBytes(const QByteArray& Bytes,bool Delete)
    {
        if(Busy()||readOnly_->isChecked()||locked_)return;auto C=comparison_;int Side=active_;qint64 First=std::min(cursor_,anchor_),Last=std::max(cursor_,anchor_);bool Insert=insert_->isChecked()&&!Delete;auto Mode=alignment_->currentText();

        work([C,Side,First,Last,Insert,Bytes,Delete,Mode](const Budget& B){auto D=C->documents_;auto [Begin,End]=span(*C,Side,First,std::min(C->length_,Last+1));if(Insert)End=Begin;else if(First==Last&&!Delete)End=std::min(D[Side].size_,Begin+Bytes.size());
            const auto Source=BinaryFromBytes(Bytes);D[Side]=ReplaceBinary(D[Side],Begin,End,Source,0,Source.size_);HexWork W;W.history_=1;W.comparison_=CompareBinary(D,Mode,B);W.top_=W.comparison_->Position(Side,Begin);W.found_=W.comparison_->Position(Side,std::min(W.comparison_->documents_[Side].size_,Begin+Bytes.size()));W.select_=true;return W;});
    }
    void HexCompareView::copyAcross(int Source)
    {
        if(Busy()||readOnly_->isChecked()||locked_)return;auto C=comparison_;qint64 First=std::min(cursor_,anchor_),Last=std::min(C->length_,std::max(cursor_,anchor_)+1);auto Mode=alignment_->currentText();
        work([C,Source,First,Last,Mode](const Budget& B){auto D=C->documents_;auto A=span(*C,Source,First,Last),Z=span(*C,1-Source,First,Last);D[1-Source]=ReplaceBinary(D[1-Source],Z.first,Z.second,D[Source],A.first,A.second);HexWork W;W.history_=1;W.comparison_=CompareBinary(D,Mode,B);W.top_=First;return W;});
    }
    void HexCompareView::copyClipboard(bool Cut)
    {
        if(Busy())return;auto C=comparison_;int Side=active_;auto First=std::min(cursor_,anchor_),Last=std::min(C->length_,std::max(cursor_,anchor_)+1);auto Top=top_;
        work([C,Side,First,Last,Cut,Top](const Budget& B){auto R=span(*C,Side,First,Last);if(R.second-R.first>16*1024*1024)throw Failure("BinaryClipboardLimit16MiB");HexWork W;W.clipboard_=ReadBinary(C->documents_[Side],R.first,R.second-R.first,B);W.clipboardReady_=true;W.cut_=Cut;W.top_=Top;return W;});
    }
    void HexCompareView::pasteClipboard(){try{const auto* Mime=QApplication::clipboard()->mimeData();editBytes(Mime->hasFormat("application/octet-stream")?Mime->data("application/octet-stream"):searchBytes(Mime->text()));}catch(const std::exception& E){status_->setText(E.what());}}
    void HexCompareView::gotoAddress()
    {
        if(Busy())return;bool Ok=false;auto Text=address_->text().trimmed();qint64 Offset=Text.toLongLong(&Ok,Text.startsWith("0x",Qt::CaseInsensitive)?16:10);if(!Ok||Offset<0||Offset>comparison_->documents_[active_].size_){status_->setText(tr("유효한 주소를 입력하세요"));return;}
        auto C=comparison_;int S=active_;filter_->blockSignals(true);filter_->setCurrentIndex(0);filter_->blockSignals(false);work([C,S,Offset](const Budget&){HexWork W;W.top_=W.found_=C->Position(S,Offset);W.select_=true;return W;});
    }
    void HexCompareView::findNext(bool Backward,bool Replace,bool All)
    {
        if(Busy()||(Replace&&(readOnly_->isChecked()||locked_)))return;
        try{auto Pattern=searchBytes(find_->text()),Replacement=searchBytes(replace_->text());if(Pattern.isEmpty())throw Failure("EmptySearchPattern");auto C=comparison_;int Side=active_;qint64 Cursor=cursor_;auto Mode=alignment_->currentText();
            filter_->blockSignals(true);filter_->setCurrentIndex(0);filter_->blockSignals(false);
            work([C,Pattern,Replacement,Side,Cursor,Mode,Backward,Replace,All](const Budget& B){auto D=C->documents_;auto Range=span(*C,Side,Cursor,std::min(C->length_,Cursor+1));qint64 Start=All?0:std::clamp(Range.first+(Backward?-1:Replace?0:1),qint64(0),D[Side].size_),Found=-1,Count=0;
                do{B.Check();auto At=FindBinary(D[Side],Pattern,Start,Backward,B);if(At<0)break;Found=At;if(!Replace)break;const auto Source=BinaryFromBytes(Replacement);D[Side]=ReplaceBinary(D[Side],At,At+Pattern.size(),Source,0,Source.size_);++Count;Start=At+Replacement.size();if(Start>=D[Side].size_)break;}while(All);
                HexWork W;W.history_=Replace&&Count?1:0;if(Replace)W.comparison_=CompareBinary(D,Mode,B);else W.comparison_=C;
                if(Found>=0){W.top_=W.found_=W.comparison_->Position(Side,std::min(Found,D[Side].size_));W.select_=true;}W.message_=Replace?QObject::tr("%1곳 치환").arg(Count):Found<0?QObject::tr("찾을 수 없습니다"):QString{};return W;});
        }catch(const std::exception& E){status_->setText(E.what());}
    }
    void HexCompareView::save(bool As)
    {
        if(Busy()||locked_||request_.readOnlyInputs_)return;int Side=active_;auto D=comparison_->documents_;QString Path=D[Side].original_.path_;storage::DestinationExpectation Expected{D[Side].original_.exists_,D[Side].original_.entry_.id_.revision_};
        if(D[Side].format_!="raw")As=true;
        if(As||Path.isEmpty()){Path=QFileDialog::getSaveFileName(this,tr("바이너리 저장"),Path);if(Path.isEmpty())return;try{Expected=InspectBinaryTarget(Path,{});}catch(const std::exception& E){status_->setText(E.what());return;}}
        auto Input=request_;auto C=comparison_;auto Top=top_;
        work([D,Side,Path,Expected,Input,C,Top](const Budget& B)mutable{for(auto& Doc:D)if(Doc.lock_)Doc.lock_->Release();D[Side]=SaveBinary(Input,D[Side],Path,Expected,B);HexWork W;W.comparison_=std::make_shared<BinaryComparison>(*C);W.comparison_->documents_=D;W.saved_=true;W.savedSide_=Side;W.top_=Top;return W;});
    }
    void HexCompareView::open(int Side,bool Clipboard)
    {
        if(Busy()||locked_)return;if(Dirty()&&hc::ui::MessageDialog::question(this,tr("파일 열기"),tr("저장하지 않은 변경을 버리시겠습니까?"))!=QMessageBox::Yes)return;
        QString Path;QByteArray Bytes;if(Clipboard){try{const auto* M=QApplication::clipboard()->mimeData();Bytes=M->hasFormat("application/octet-stream")?M->data("application/octet-stream"):searchBytes(M->text());}catch(const std::exception& E){status_->setText(E.what());return;}}else{Path=QFileDialog::getOpenFileName(this,tr("바이너리 열기"));if(Path.isEmpty())return;}
        auto D=comparison_->documents_;auto Mode=alignment_->currentText();work([D,Mode,Path,Bytes,Side,Clipboard](const Budget& B)mutable{D[Side]=Clipboard?BinaryFromBytes(Bytes):OpenBinary(Path,B);HexWork W;W.comparison_=CompareBinary(D,Mode,B);W.saved_=true;W.savedSide_=Side;return W;});
    }
    void HexCompareView::reload()
    {
        if(Busy()||locked_)return;if(Dirty()&&hc::ui::MessageDialog::question(this,tr("새로 읽기"),tr("저장하지 않은 변경을 버리시겠습니까?"))!=QMessageBox::Yes)return;
        auto D=comparison_->documents_;auto Mode=alignment_->currentText();work([D,Mode](const Budget& B)mutable{for(auto& Doc:D)if(!Doc.original_.path_.isEmpty())Doc=ConvertBinary(OpenBinary(Doc.original_.path_,B),Doc.format_,B);HexWork W;W.comparison_=CompareBinary(D,Mode,B);W.saved_=true;W.savedSide_=2;return W;});
    }
    void HexCompareView::session(bool Load)
    {
        if(Busy())return;
        if(Load){if(locked_)return;if(Dirty()){status_->setText(tr("세션을 열기 전에 수정 파일을 저장하십시오."));return;}auto Path=QFileDialog::getOpenFileName(this,tr("Hex 세션 열기"),{},"Hex session (*.hchex)");if(Path.isEmpty())return;QFile F(Path);if(!F.open(QIODevice::ReadOnly)||F.size()>1024*1024)return;auto O=QJsonDocument::fromJson(F.readAll()).object();if(O["schema"]!="hc.hex.session.1"){status_->setText(tr("잘못된 세션"));return;}auto A=O["files"].toArray();if(A.size()!=2)return;QStringList Paths{A[0].toString(),A[1].toString()};QString Mode=O["alignment"].toString("none");auto Formats=O["formats"].toArray();if(Formats.size()!=2)Formats=QJsonArray{"raw","raw"};sessionPath_=Path;
            for(const auto& Value:Formats)if(Value.toString().startsWith("external:")){auto Config=QJsonDocument::fromJson(Value.toString().mid(9).toUtf8()).object();if(hc::ui::MessageDialog::question(this,tr("세션의 변환 프로그램 실행"),Config["program"].toString()+"\n"+QString::fromUtf8(QJsonDocument(Config).toJson())+tr("\n이 프로그램을 실행하시겠습니까?"))!=QMessageBox::Yes)return;}
            width_->blockSignals(true);width_->setCurrentIndex(std::clamp(O["width"].toInt(1),0,2));width_->blockSignals(false);layout_->setCurrentIndex(O["layout"].toInt()==1?1:0);
            work([Paths,Mode,Formats](const Budget& B){HexWork W;W.comparison_=CompareBinary({ConvertBinary(OpenBinary(Paths[0],B),Formats[0].toString(),B),ConvertBinary(OpenBinary(Paths[1],B),Formats[1].toString(),B)},Mode,B);W.saved_=true;W.savedSide_=2;return W;});
        }else{if(Dirty()){status_->setText(tr("세션 경로를 저장하기 전에 수정 파일을 저장하십시오."));return;}auto Path=QFileDialog::getSaveFileName(this,tr("Hex 세션 저장"),sessionPath_,"Hex session (*.hchex)");if(Path.isEmpty())return;QJsonArray Files;for(const auto& D:comparison_->documents_){if(D.original_.path_.isEmpty()){status_->setText(tr("클립보드 파일을 먼저 저장하십시오."));return;}Files.append(D.original_.path_);}try { Request Guard=request_;for(const auto& D:comparison_->documents_)Guard.protectedInputs_.append(D.original_.path_);CheckProtectedInputs(Guard,{Path},{}); } catch(const std::exception& E){status_->setText(E.what());return;}QSaveFile F(Path);if(!F.open(QIODevice::WriteOnly))return;F.write(QJsonDocument(QJsonObject{{"schema","hc.hex.session.1"},{"files",Files},{"formats",QJsonArray{comparison_->documents_[0].format_,comparison_->documents_[1].format_}},{"alignment",comparison_->mode_},{"width",width_->currentIndex()},{"layout",layout_->currentIndex()}}).toJson());if(F.commit())sessionPath_=Path;}
    }
    void HexCompareView::ExportReport(const QString& RequestedPath)
    {
        if(Busy())return;const auto Path=RequestedPath.isEmpty()?QFileDialog::getSaveFileName(this,tr("Hex 비교 보고서"),{},"HTML (*.html);;Text (*.txt)"):RequestedPath;if(Path.isEmpty())return;auto C=comparison_;
        work([C,Path](const Budget& B){Request Protected;for(const auto& D:C->documents_)if(!D.original_.path_.isEmpty())Protected.protectedInputs_.append(D.original_.path_);CheckProtectedInputs(Protected,{Path},B);QSaveFile F(Path);if(!F.open(QIODevice::WriteOnly))throw Failure("HexReportOpenFailed");const bool Html=Path.endsWith(".html",Qt::CaseInsensitive);const auto Write=[&](const QString& Text){auto Bytes=Text.toUtf8();if(F.write(Bytes)!=Bytes.size())throw Failure("HexReportWriteFailed");};
            if(Html)Write("<!doctype html><meta charset=\"utf-8\"><title>Hex Compare</title><style>body{font-family:monospace}pre{white-space:pre-wrap}.diff{background:#fff0e8}</style><h1>Hex Compare</h1><pre>");
            for(const auto& D:C->documents_)Write((Html?D.original_.path_.toHtmlEscaped():D.original_.path_)+"  "+QString::number(D.size_)+" bytes\n");Write("Alignment: "+C->mode_+"\n");
            for(qint64 I=0;I<C->changeCount_;++I){B.Check();auto R=C->Change(I);Write(QString("\n#%1  left 0x%2 +%3 / right 0x%4 +%5\n").arg(I+1).arg(address(R.begin_[0])).arg(R.size_[0]).arg(address(R.begin_[1])).arg(R.size_[1]));if(Html)Write("<span class=\"diff\">");
                for(qint64 At=0;At<R.length_;At+=16){B.Check();QStringList Lines;for(int S=0;S<2;++S){const qint64 N=std::min(qint64(16),std::max(qint64(0),R.size_[S]-At));auto Bytes=ReadBinary(C->documents_[S],R.begin_[S]+std::min(At,R.size_[S]),N,B);Lines<<QString::fromLatin1(Bytes.toHex(' ')).leftJustified(47,' ');}Write(Lines.join(" | ")+"\n");}if(Html)Write("</span>");}
            if(Html)Write("</pre>");if(!F.commit())throw Failure("HexReportCommitFailed");HexWork W;W.message_=QObject::tr("보고서를 저장했습니다: %1").arg(Path);return W;});
    }
    void HexCompareView::PrintReport()
    {
        if(Busy())return;auto Printer=std::make_shared<QPrinter>(QPrinter::HighResolution);
        QPrintDialog Dialog(Printer.get(),this);if(Dialog.exec()!=QDialog::Accepted)return;auto C=comparison_;
        work([Printer,C](const Budget& B){QPainter P;if(!P.begin(Printer.get()))throw Failure("HexPrintFailed");
            QFont Font("Cascadia Mono",9);P.setFont(Font);QFontMetrics Metrics(Font,Printer.get());
            const int Height=Metrics.height()+Metrics.leading(), Bottom=Printer->pageLayout().paintRectPixels(Printer->resolution()).height();int Y=Height;
            const auto Line=[&](const QString& Text){B.Check();if(Y+Height>Bottom){if(!Printer->newPage())throw Failure("HexPrintPageFailed");Y=Height;}P.drawText(0,Y,Text);Y+=Height;};
            Line("HyperCompare Hex / "+C->mode_);for(const auto& D:C->documents_)Line(D.original_.path_+"  "+QString::number(D.size_)+" bytes");
            for(qint64 I=0;I<C->changeCount_;++I){auto R=C->Change(I);Line(QString("#%1 left 0x%2 / right 0x%3").arg(I+1).arg(address(R.begin_[0]),address(R.begin_[1])));
                for(qint64 At=0;At<R.length_;At+=16){QStringList Values;for(int S=0;S<2;++S){qint64 N=std::min(qint64(16),std::max(qint64(0),R.size_[S]-At));Values<<QString::fromLatin1(ReadBinary(C->documents_[S],R.begin_[S]+std::min(At,R.size_[S]),N,B).toHex(' ')).leftJustified(47,' ');}Line(Values.join(" | "));}}
            P.end();HexWork W;W.message_=QObject::tr("인쇄 작업을 전송했습니다.");return W;});
    }
    void HexCompareView::format(int Side)
    {
        if(Busy()||locked_)return;if(Dirty()){status_->setText(tr("형식을 변경하기 전에 수정 파일을 저장하십시오."));return;}
        if(comparison_->documents_[Side].original_.path_.isEmpty())return;
        bool Ok=false;auto Format=QInputDialog::getItem(this,tr("파일 형식"),tr("원본 읽기 변환 (변환 결과는 다른 이름으로 저장)"),{"raw","hex","base64","external","detected"},0,false,&Ok);if(!Ok)return;
        if(Format=="detected"){const auto Name=QFileInfo(comparison_->documents_[Side].original_.path_).fileName();Format="raw";for(const auto& Kind:{QString("hex"),QString("base64")}){const auto Masks=Preferences{}.Value("hex/masks/"+Kind).toString().split(';',Qt::SkipEmptyParts);if(QDir::match(Masks,Name))Format=Kind;}}
        if(Format=="external"){
            auto Program=QFileDialog::getOpenFileName(this,tr("읽기 변환 프로그램 선택"),Preferences{}.Value("hex/converter/program").toString(),"Programs (*.exe)");if(Program.isEmpty())return;
            bool Accepted=false;auto Arguments=QInputDialog::getText(this,tr("변환 인수"),tr("%s: 읽기용 사본, %t: 출력, %o: 원본 경로\n선택한 프로그램을 실행합니다. 변환 결과는 별도 저장합니다."),QLineEdit::Normal,(Preferences{}.Contains("hex/converter/arguments")?Preferences{}.Value("hex/converter/arguments").toString():QString("%s %t")),&Accepted);if(!Accepted)return;
            auto Parts=QProcess::splitCommand(Arguments);if(!Arguments.contains("%t")){status_->setText(tr("출력 인수 %t가 필요합니다."));return;}QJsonArray Args;for(const auto& Part:Parts)Args.append(Part);
            Preferences{}.SetValue("hex/converter/program",Program);Preferences{}.SetValue("hex/converter/arguments",Arguments);
            Format="external:"+QString::fromUtf8(QJsonDocument(QJsonObject{{"program",Program},{"arguments",Args}}).toJson(QJsonDocument::Compact));
        }
        auto D=comparison_->documents_;auto Mode=alignment_->currentText();
        work([D,Side,Format,Mode](const Budget& B)mutable{D[Side]=ConvertBinary(OpenBinary(D[Side].original_.path_,B),Format,B);HexWork W;W.comparison_=CompareBinary(D,Mode,B);W.saved_=true;W.savedSide_=Side;return W;});
    }
    void HexCompareView::copyFileAndNext(int Source)
    {
        if(Busy()||Dirty()||locked_||request_.readOnlyInputs_||!request_.nextDifferenceFile_)return;
        if(hc::ui::MessageDialog::question(this,tr("전체 파일 복사"),tr("원본 %1의 전체 내용으로 원본 %2를 바꾸고 다음 차이 파일을 여시겠습니까?").arg(Source+1).arg(2-Source))!=QMessageBox::Yes)return;
        auto C=comparison_;auto Input=request_;auto Mode=comparison_->mode_;
        work([C,Input,Source,Mode](const Budget& B){auto D=C->documents_;int Target=1-Source;
            for(auto& Doc:D)if(Doc.lock_)Doc.lock_->Release();const auto Original=D[Target].original_;D[Target]=ReplaceBinary(D[Target],0,D[Target].size_,D[Source],0,D[Source].size_);
            D[Target]=SaveBinary(Input,D[Target],Original.path_,{Original.exists_,Original.entry_.id_.revision_},B);
            HexWork W;auto VerifyBudget=B;VerifyBudget.canceled_=nullptr;VerifyBudget.deadline_=std::chrono::steady_clock::now()+std::chrono::minutes(10);W.comparison_=CompareBinary(D,Mode,VerifyBudget);W.saved_=true;W.savedSide_=Target;W.message_="hex.copy.next";return W;});
    }
}