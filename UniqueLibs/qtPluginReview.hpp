#pragma once
#include "pluginHost/manifestFile.hpp"
#include "qtMessageDialog.hpp"
#include <QMessageBox>
#include <QPushButton>
#include <functional>

namespace hc::plugins {
inline bool OfferPluginReview(QWidget* Parent, const QString& Code, const QString& Binary,
    std::function<void(const QString&)> Review)
{
    if (!Parent || !PluginPackageChanged(Code.toStdString()) || !Review) return false;
    if (auto* Existing = Parent->findChild<QMessageBox*>("hc.plugin.changed")) { Existing->raise(); return true; }
    auto* Notice = new hc::ui::MessageDialog(QMessageBox::Warning, QObject::tr("플러그인 파일 변경·재승인 필요"),
        (Binary.isEmpty() ? QString{} : Binary + '\n') + QObject::tr("플러그인 파일 또는 검증 정보가 변경되었거나 실행 승인이 필요합니다. 파일을 검사하고 다시 승인해 주세요.\n\n설정을 적용한 뒤 원래 작업을 다시 실행하세요."), QMessageBox::Cancel, Parent);
    Notice->setObjectName("hc.plugin.changed"); Notice->setTextFormat(Qt::PlainText); Notice->setAttribute(Qt::WA_DeleteOnClose);
    auto* Button = Notice->addButton(QObject::tr("검사·재승인…"), QMessageBox::ActionRole); Button->setObjectName("hc.plugin.changedReview");
    Notice->setDetailedText(Code); Notice->setDefaultButton(QMessageBox::Cancel);
    QObject::connect(Button, &QPushButton::clicked, Parent, [Review = std::move(Review), Binary] { Review(Binary); });
    Notice->open(); return true;
}
}
