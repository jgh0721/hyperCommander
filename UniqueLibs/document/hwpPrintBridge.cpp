#include "hwpPrintBridge.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QBuffer>
#include <QImageReader>
#include <QUuid>
#include <utility>
#include <cmath>
namespace hc::document {
    QJsonObject HwpPrintIdentity(const DocumentOperationTicket& T, quint64 BindingGeneration) {
        return {{"documentId",QString::fromStdString(T.documentId_)},{"generation",QString::number(T.generation_)},
            {"bindingOwner",QString::number(T.model_.epoch_)},{"bindingGeneration",QString::number(BindingGeneration)},{"epoch",QString::number(T.model_.epoch_)},
            {"revision",QString::number(T.revision_)},{"token",QString::number(T.token_)},
            {"changeSequence",QString::number(T.model_.changeSequence_)},{"savepoint",QString::fromStdString(T.model_.savepoint_)}};
    }
    HwpPrintTransfer::HwpPrintTransfer(DocumentOperationTicket Ticket,std::vector<int> Pages,int TimeoutMs,quint64 BindingGeneration)
        :bindingGeneration_(BindingGeneration),pages_(std::move(Pages)),deadline_(TimeoutMs,Qt::PreciseTimer) { frames_.ticket_=std::move(Ticket); }
    QString HwpPrintTransfer::Error() const { return !ready_ && deadline_.hasExpired() ? "rhwp-print-timeout" : error_; }
    bool HwpPrintTransfer::fail(const char* Code) { error_=Code;pageBytes_.clear();frames_.pages_.clear();return false; }
    bool HwpPrintTransfer::Handle(const QJsonObject& M) {
        if(!Error().isEmpty() || ready_)return false;
        if(M["identity"]!=QJsonValue(HwpPrintIdentity(frames_.ticket_,bindingGeneration_)) || M["version"]!=QJsonValue(1))return fail("rhwp-print-stale");
        const auto Type=M["type"].toString();
        if(Type=="PrintFailed")return fail("rhwp-print-renderer-failure");
        if(Type=="PrintBegin" && !begun_ && M.size()==4) {
            const auto Count=M["totalPages"].toInt(-1);
            if(M["totalPages"]!=QJsonValue(Count) || Count<1)return fail("rhwp-print-page-count");
            frames_.totalPages_=Count;
            if(pages_.empty())for(int I=0;I<Count;++I)pages_.push_back(I);
            int Previous=-1;
            for(const auto Page:pages_) {if(Page<=Previous || Page>=Count)return fail("rhwp-print-page-range");Previous=Page;}
            begun_=true;return true;
        }
        if(!begun_)return fail("rhwp-print-order");
        if(Type=="PrintPageBegin" && !pageOpen_ && M.size()==8 && frames_.pages_.size()<pages_.size()) {
            page_={M["page"].toInt(-1),{M["width"].toDouble(-1),M["height"].toDouble(-1)},{}};
            const auto W=page_.paperPixels_.width(),H=page_.paperPixels_.height();
            const auto Length=M["byteLength"].toDouble(-1);pageHash_=M["sha256"].toString().toLatin1();
            if(M["page"]!=QJsonValue(page_.index_) || page_.index_!=pages_[frames_.pages_.size()]
                || !std::isfinite(W)||!std::isfinite(H)||W<=0||H<=0
                || Length<1||Length>9007199254740991.0||std::floor(Length)!=Length
                || pageHash_.size()!=64||QByteArray::fromHex(pageHash_).toHex()!=pageHash_)
                return fail("rhwp-print-page-metadata");
            pageLength_=qint64(Length);bytes_+=pageLength_;
            pageBytes_.clear();pageOpen_=true;return true;
        }
        if(Type=="PrintChunk" && pageOpen_ && M.size()==5) {
            const auto Encoded=M["data"].toString().toLatin1();
            const auto Bytes=QByteArray::fromBase64(Encoded,QByteArray::AbortOnBase64DecodingErrors);
            if(!M["offset"].isDouble()||M["offset"].toDouble(-1)!=pageBytes_.size()||Encoded.size()>256*1024
                ||Bytes.isEmpty()||Bytes.toBase64()!=Encoded||M["data"].toString()!=QString::fromLatin1(Encoded)
                ||Bytes.size()>pageLength_-pageBytes_.size())return fail("rhwp-print-chunk");
            pageBytes_+=Bytes;return true;
        }
        if(Type=="PrintPageEnd" && pageOpen_ && M.size()==3) {
            if(pageBytes_.size()!=pageLength_||QCryptographicHash::hash(pageBytes_,QCryptographicHash::Sha256).toHex()!=pageHash_)
                return fail("rhwp-print-page-hash");
            QBuffer Buffer(&pageBytes_);Buffer.open(QIODevice::ReadOnly);QImageReader Reader(&Buffer,"PNG");Reader.setDecideFormatFromContent(false);
            const auto Size=Reader.size();
            if(Size.isEmpty()
                ||Size.width()>std::ceil((page_.paperPixels_.width()+0.05)*2)
                ||Size.height()>std::ceil((page_.paperPixels_.height()+0.05)*2))return fail("rhwp-print-pixel-budget");
            auto Image=Reader.read();if(Image.isNull()||Image.size()!=Size)return fail("rhwp-print-png");
            page_.png_=std::move(pageBytes_);frames_.pages_.push_back(std::move(page_));pageOpen_=false;return true;
        }
        if(Type=="PrintCommit" && !pageOpen_ && M.size()==3 && frames_.pages_.size()==pages_.size()) {ready_=true;return true;}
        return fail("rhwp-print-order");
    }
    void HwpPrintBridge::Activate(QJsonObject Binding) {binding_=std::move(Binding);command_={};message_={};retired_={};action_=false;}
    QVariantMap HwpPrintBridge::State() const {return {{"version",1},{"binding",binding_}};}
    bool HwpPrintBridge::Begin(QJsonObject Identity,QJsonArray Pages) {
        if(binding_.isEmpty()||!command_.isEmpty()||Identity.size()!=9)return false;
        for(auto I=binding_.begin();I!=binding_.end();++I)if(Identity[I.key()]!=I.value())return false;
        for(const auto& Name:{"revision","token","changeSequence"}) {
            bool Ok=false;const auto S=Identity[Name].toString();const auto N=S.toULongLong(&Ok);
            if(!Ok||QString::number(N)!=S||(Name!=QString("changeSequence")&&!N))return false;
        }
        const auto Point=Identity["savepoint"].toString();if(Point.isEmpty()||Point.size()>128)return false;
        int Previous=-1;for(const auto& V:Pages){const auto Page=V.toInt(-1);if(V!=QJsonValue(Page)||Page<=Previous)return false;Previous=Page;}
        retired_={};command_={{"version",1},{"id",QUuid::createUuid().toString(QUuid::Id128)},{"identity",Identity},{"pages",Pages}};return true;
    }
    void HwpPrintBridge::Cancel() { message_={};action_=false;if(!command_.isEmpty())command_["cancel"]=true; }
    bool HwpPrintBridge::Cancel(const QJsonObject& Identity) {
        if(Identity.size()!=9)return false;
        for(auto I=binding_.begin();I!=binding_.end();++I)if(Identity[I.key()]!=I.value())return false;
        if(!command_.isEmpty()&&command_.value("identity")!=Identity)return false;
        Cancel();if(command_.isEmpty())retired_=Identity;return true;
    }
    bool HwpPrintBridge::TakeAction() {return std::exchange(action_,false);}
    QJsonObject HwpPrintBridge::TakeMessage() {
        auto Out=std::exchange(message_,{});if(Out.value("type")=="PrintCommit"||Out.value("type")=="PrintFailed"){retired_=command_.value("identity").toObject();command_={};}return Out;
    }
    bool HwpPrintBridge::Report(const QString& Text) {
        if(binding_.isEmpty()||Text.size()>270*1024)return false;
        QJsonParseError Error;const auto Parsed=QJsonDocument::fromJson(Text.toUtf8(),&Error);
        if(Error.error!=QJsonParseError::NoError||!Parsed.isObject())return false;
        const auto O=Parsed.object();if(O["version"]!=QJsonValue(1)||O["binding"]!=QJsonValue(binding_))return false;
        if(O["type"]=="UserPrint"&&O.size()==3&&command_.isEmpty()&&!action_){action_=true;return true;}
        if(O["type"]=="PrintRetired"&&O.size()==4&&O["id"]==command_.value("id")&&command_.value("cancel")==QJsonValue(true)) {retired_=command_.value("identity").toObject();command_={};message_={};return true;}
        if(command_.value("cancel")==QJsonValue(true))return false;
        if(O.size()!=5||O["type"]!="PrintMessage"||command_.isEmpty()||O["id"]!=command_.value("id")||!message_.isEmpty()||!O["message"].isObject())return false;
        const auto M=O["message"].toObject();if(M["version"]!=QJsonValue(1)||M["identity"]!=command_.value("identity"))return false;
        message_=M;return true;
    }
}
