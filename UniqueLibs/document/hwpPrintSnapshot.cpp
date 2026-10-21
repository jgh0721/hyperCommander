#include "hwpPrintSnapshot.hpp"
#include <QBuffer>
#include <QImageReader>
#include <QPainter>
#include <QPdfWriter>
#include <QPageSize>
#include <cmath>
namespace hc::document {
    namespace {
        class BoundedBuffer final:public QBuffer {
        public: bool exceeded_ = false;
        protected:
            qint64 writeData(const char* Data,qint64 Length) override {
                if(Length<0 || Length>std::numeric_limits<qint64>::max()-pos()){exceeded_=true;return -1;}
                return QBuffer::writeData(Data,Length);
            }
        };
        storage::Error Invalid(const char* Code){return {storage::ErrorCategory::InvalidRequest,Code};}
    }
    storage::Result<QByteArray> BuildHwpPdf(const DocumentOperationTicket& Expected,
        const std::vector<int>& RequestedPages,const HwpPrintFrames& Frames) {
        if(Expected.operation_!=DocumentOperation::Printing || Expected.documentId_.empty() || !Expected.generation_
            || !Expected.token_ || !Expected.revision_ || !Expected.model_.epoch_ || Expected.model_.savepoint_.empty()
            || !(Frames.ticket_==Expected) || Frames.totalPages_<1
            || RequestedPages.empty() || Frames.pages_.size()!=RequestedPages.size())
            return Invalid("rhwp-print-identity");
        quint64 Pixels=0,Bytes=0;int Previous=-1;
        std::vector<QImage> Images;Images.reserve(Frames.pages_.size());
        for(size_t Index=0;Index<Frames.pages_.size();++Index) {
            const auto& Page=Frames.pages_[Index];
            if(RequestedPages[Index]<=Previous || RequestedPages[Index]>=Frames.totalPages_ || Page.index_!=RequestedPages[Index])
                return Invalid("rhwp-print-page-range");
            Previous=RequestedPages[Index];const auto Paper=Page.paperPixels_;
            if(!std::isfinite(Paper.width()) || !std::isfinite(Paper.height()) || Paper.width()<=0 || Paper.height()<=0
                || Page.png_.isEmpty())return Invalid("rhwp-print-budget");
            QBuffer Input;Input.setData(Page.png_);Input.open(QIODevice::ReadOnly);QImageReader Reader(&Input,"PNG");
            Reader.setDecideFormatFromContent(false);const auto Size=Reader.size();
            if(Size.isEmpty())return Invalid("rhwp-print-pixel-budget");
            auto Image=Reader.read();if(Image.isNull() || Image.size()!=Size)return Invalid("rhwp-print-image");
            Images.push_back(std::move(Image));
        }
        BoundedBuffer Output;Output.open(QIODevice::WriteOnly);bool Ok=true;
        {
            QPdfWriter Writer(&Output);Writer.setResolution(96);Writer.setCreator("HyperCommander RHWP");
            QPainter Painter;
            for(size_t Index=0;Index<Frames.pages_.size();++Index) {
                const auto Paper=Frames.pages_[Index].paperPixels_;
                Ok=Writer.setPageSize(QPageSize(Paper*(25.4/96.0),QPageSize::Millimeter,QString(),QPageSize::ExactMatch))
                    && Writer.setPageMargins(QMarginsF());
                if(!Ok)break;
                if(Index==0)Ok=Painter.begin(&Writer);else Ok=Writer.newPage();
                if(!Ok)break;
                const QRectF Page(0,0,Writer.width(),Writer.height());Painter.fillRect(Page,Qt::white);
                Painter.drawImage(Page,Images[Index]);if(Output.exceeded_){Ok=false;break;}
            }
            if(Painter.isActive())Ok=Painter.end() && Ok;
        }
        if(!Ok || Output.exceeded_ || !Output.data().startsWith("%PDF-"))return Invalid("rhwp-print-pdf");
        return Output.data();
    }
}
