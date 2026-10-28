#include "stdafx.h"
#include "compareCommands.hpp"
#include "compareSettings.hpp"
#include <QAction>
#include <QShortcut>
#include <QVariant>
#include <QWidget>
#include <QKeyEvent>

namespace hc::compare
{
    QList<CompareCommand> CompareCommands()
    {
        const auto tr = [](const char* Text) { return QObject::tr(Text); };
        QList<CompareCommand> result;
        const auto add = [&](const char* Id, const char* Label, const char* Category, const char* Key = "", const char* Scope = "비교창") {
            result.append({QString::fromLatin1(Id), tr(Label), tr(Category), tr(Scope), *Key ? QList<QKeySequence>{QKeySequence(QString::fromLatin1(Key))} : QList<QKeySequence>{}});
        };
        add("navigate.next", "다음 차이", "비교·이동", "F7"); add("navigate.previous", "이전 차이", "비교·이동", "Shift+F7");
        add("find.open", "찾기", "찾기", "Ctrl+F"); add("find.next", "다음 찾기", "찾기", "F3");
        add("find.previous", "이전 찾기", "찾기", "Shift+F3"); add("find.all", "모두 찾기", "찾기", "Ctrl+Shift+F");
        add("file.save", "결과 저장", "파일·작업", "Ctrl+S"); add("work.save", "병합 작업 저장", "파일·작업", "Ctrl+Shift+S");
        add("work.open", "병합 작업 열기", "파일·작업", "Ctrl+Shift+O");
        add("edit.undo", "실행 취소", "편집"); result.last().defaults_ = QKeySequence::keyBindings(QKeySequence::Undo);
        add("edit.redo", "다시 실행", "편집"); result.last().defaults_ = QKeySequence::keyBindings(QKeySequence::Redo);
        add("directory.rename", "디렉터리 이름 변경", "파일·작업", "F2", "디렉터리 트리");
        add("file.open", "파일 열기", "파일·작업"); add("directory.open", "디렉터리 열기", "파일·작업");
        add("compare.again", "다시 비교", "비교·이동"); add("compare.cancel", "비교 취소", "비교·이동");
        add("merge.left", "블록 LEFT", "병합"); add("merge.right", "블록 RIGHT", "병합"); add("merge.base", "블록 BASE", "병합");
        add("merge.leftRight", "LEFT → RIGHT", "병합"); add("merge.rightLeft", "RIGHT → LEFT", "병합");
        add("merge.omit", "블록 제거", "병합"); add("merge.manual", "블록 수동 완료", "병합");
        add("view.alignAll", "전체 패널 표시 줄 정렬", "보기·설정"); add("view.alignRows", "왼쪽·오른쪽 표시 줄 정렬", "보기·설정");
        add("view.sync", "왼쪽·오른쪽 스크롤 동기화", "보기·설정"); add("view.inline", "줄 내부 차이 강조", "보기·설정");
        add("view.overview", "차이 개요 막대", "보기·설정"); add("app.settings", "설정", "보기·설정", "Ctrl+,");
        add("view.syntax.auto", "구문 강조 · 자동", "보기·설정"); add("view.syntax.c", "구문 강조 · C", "보기·설정");
        add("view.syntax.cpp", "구문 강조 · C++", "보기·설정"); add("view.syntax.python", "구문 강조 · Python", "보기·설정");
        add("view.syntax.json", "구문 강조 · JSON", "보기·설정"); add("view.syntax.xml", "구문 강조 · XML", "보기·설정");
        add("view.syntax.none", "구문 강조 · 일반 텍스트", "보기·설정");
        return result;
    }
    QList<QKeySequence> CompareKeys(const QString& Id, const QJsonObject& Overrides)
    {
        if (Overrides.contains(Id)) { const auto value = Overrides[Id].toString(); return value.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{QKeySequence::fromString(value, QKeySequence::PortableText)}; }
        for (const auto& command : CompareCommands()) if (command.id_ == Id) return command.defaults_;
        return {};
    }
    QString CompareKeyError(const QKeySequence& Key)
    {
        if (Key.isEmpty()) return {};
        if (Key.count() != 1) return QObject::tr("한 번의 키 조합만 지정할 수 있습니다.");
        const auto key = Key[0].key(); const auto modifiers = Key[0].keyboardModifiers();
        if (key == Qt::Key_unknown || key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta ||
            modifiers.testFlag(Qt::MetaModifier) || (modifiers.testFlag(Qt::ControlModifier) && modifiers.testFlag(Qt::AltModifier)) ||
            key == Qt::Key_Escape || key == Qt::Key_Tab || key == Qt::Key_Backtab || key == Qt::Key_Return || key == Qt::Key_Enter ||
            key == Qt::Key_Delete || key == Qt::Key_Backspace || (key >= Qt::Key_Home && key <= Qt::Key_PageDown) ||
            (key == Qt::Key_F4 && modifiers.testFlag(Qt::AltModifier)) || (key == Qt::Key_Space && modifiers.testFlag(Qt::AltModifier)) ||
            (!(modifiers & (Qt::ControlModifier | Qt::AltModifier)) && !(key >= Qt::Key_F1 && key <= Qt::Key_F35)))
            return QObject::tr("입력·탐색 또는 운영체제 예약 키입니다. 다른 조합을 사용하십시오.");
        for (const auto standard : {QKeySequence::Copy, QKeySequence::Cut, QKeySequence::Paste, QKeySequence::SelectAll})
            if (QKeySequence::keyBindings(standard).contains(Key)) return QObject::tr("기본 편집에 사용하는 키입니다.");
        return {};
    }
    QString ValidateCompareShortcuts(const QJsonObject& Overrides)
    {
        QMap<QKeySequence, QString> owners;
        // Directory child shortcuts overlap window shortcuts; validate conservatively across every command.
        for (const auto& command : CompareCommands()) for (const auto& key : CompareKeys(command.id_, Overrides))
        {
            if (Overrides.contains(command.id_)) if (const auto error = CompareKeyError(key); !error.isEmpty()) return command.label_ + ": " + error;
            if (key.isEmpty()) continue;
            if (owners.contains(key) && owners[key] != command.label_)
                return QObject::tr("%1: ‘%2’와 ‘%3’에서 사용 중입니다.").arg(key.toString(QKeySequence::NativeText), owners[key], command.label_);
            owners[key] = command.label_;
        }
        return {};
    }
    namespace
    {
        void apply(QObject* Object)
        {
            const auto id = Object->property("compareCommand").toString(); if (id.isEmpty()) return;
            const auto keys = CompareKeys(id, ActiveCompareSettings().shortcuts_);
            if (auto* action = qobject_cast<QAction*>(Object))
            {
                action->setShortcuts(keys);
                const auto base = action->property("compareBaseTip").toString(); QStringList labels;
                for (const auto& key : keys) labels.append(key.toString(QKeySequence::NativeText));
                action->setToolTip(base + (labels.isEmpty() ? QString{} : " (" + labels.join(", ") + ")"));
            }
            else if (auto* shortcut = qobject_cast<QShortcut*>(Object)) shortcut->setKeys(keys);
        }
    }
    void BindCompareCommand(QObject* Object, const QString& Id)
    {
        Object->setProperty("compareCommand", Id);
        if (auto* action = qobject_cast<QAction*>(Object)) action->setProperty("compareBaseTip", action->toolTip());
        apply(Object);
    }
    void ApplyCompareShortcuts(QObject* Root) { apply(Root); for (auto* child : Root->findChildren<QObject*>()) apply(child); }
    bool HandleCompareEditorKey(QWidget* Editor, QKeyEvent* Event, bool Execute)
    {
        const QKeySequence key(QKeyCombination(Event->modifiers() & ~Qt::KeypadModifier, Qt::Key(Event->key())));
        for (auto* action : Editor->window()->findChildren<QAction*>())
        {
            if (action->property("compareCommand").toString().isEmpty() || !action->isEnabled() || !action->shortcuts().contains(key)) continue;
            if (Execute && (!Event->isAutoRepeat() || action->autoRepeat())) action->trigger();
            Event->accept(); return true;
        }
        // A remapped or cleared Undo/Redo must also stop Scintilla's old default binding.
        for (const auto& entry : {std::pair{"edit.undo", QKeySequence::Undo}, {"edit.redo", QKeySequence::Redo}})
            if (ActiveCompareSettings().shortcuts_.contains(entry.first) && Event->matches(entry.second) &&
                !CompareKeys(entry.first, ActiveCompareSettings().shortcuts_).contains(key)) { Event->accept(); return true; }
        return false;
    }
}
