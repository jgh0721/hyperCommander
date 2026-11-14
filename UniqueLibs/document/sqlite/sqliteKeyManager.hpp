#pragma once

/// 고유 라이브러리
#include "sqliteKeyStore.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <functional>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace hc::sqlite
{
    // 키 보기 전에 Windows 로그인을 확인한다(설계 8.3). 확인했으면 true. 시험은 SetLoginCheck 로 바꿔 끼운다.
    bool ConfirmWindowsLogin(QWidget* Parent, const QString& Reason);
    void SetLoginCheck(std::function<bool(QWidget*, const QString&)> Check);

    // 설정 › SQLite › 암호화 (SEE) › 비밀번호 관리(시안 ‘설정 창 · SQLite’ 의 keys 탭): 저장한 키 목록과
    // 키 보기 · 경로 바꾸기 · 삭제 · 없는 파일 정리 · 모두 삭제. 키 저장소는 설정 파일이 아니라 바로 쓴다(적용 · 취소와 무관).
    class KeyManagerPage final : public QWidget
    {
        Q_OBJECT
    public:
        explicit KeyManagerPage(const QString& Directory, QWidget* Parent = nullptr);
        void Refresh();
        QTreeWidget* List() const { return list_; }
        QLineEdit* Search() const { return search_; }
        QLabel* Title() const { return title_; }
        // 시험 · 자동화: 고른 항목의 키를 확인 뒤 보인다(보인 대화는 열린 채로 둔다).
        void RevealSelected();
        void ChangeSelectedPath(const QString& Path);
        void RemoveSelected(bool Ask);
        int RemoveMissing();
        void RemoveAll(bool Ask);
    signals:
        void Changed();
    private:
        QString selectedPath() const;
        void refreshButtons();

        KeyStore store_;
        QLabel* title_ = nullptr;
        QLineEdit* search_ = nullptr;
        QTreeWidget* list_ = nullptr;
        QPushButton* reveal_ = nullptr;
        QPushButton* move_ = nullptr;
        QPushButton* remove_ = nullptr;
        QPushButton* clean_ = nullptr;
        QPushButton* clear_ = nullptr;
    };
}
