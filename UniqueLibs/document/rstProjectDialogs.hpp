#pragma once

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDialog>
#include <QKeySequence>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QTabWidget;
class QTreeWidget;

namespace hc::document
{
    /// 새 Sphinx 프로젝트의 내용. conf.py 와 index.rst 는 제품이 직접 쓴다 — sphinx-quickstart 를 부르지 않으므로
    /// Python 환경이 없어도 만들 수 있다.
    struct SphinxProjectSpec
    {
        QString name_;                                   // conf.py 의 project. 필수
        QString author_;
        QString directory_;                              // 프로젝트 폴더가 놓일 위치
        QString folder_;                                 // 프로젝트 폴더 이름
        QString language_ = QStringLiteral( "ko" );
        QString theme_ = QStringLiteral( "alabaster" );
        bool    separate_ = false;                       // source · build 폴더로 나눈다
        bool    myst_ = false;                           // Markdown 문서도 쓴다( myst_parser )

        /// 프로젝트 폴더.
        [[nodiscard]] QString Root() const;
        /// conf.py 와 문서가 놓이는 폴더: Root() 또는 Root()/source.
        [[nodiscard]] QString SourceDirectory() const;
        /// 만들 수 없으면 그 이유, 만들 수 있으면 빈 문자열.
        [[nodiscard]] QString Error() const;
    };

    /// 프로젝트 이름에서 만든 폴더 이름: ASCII 영숫자와 '-'. 남는 글자가 없으면 "docs".
    [[nodiscard]] QString SphinxFolderName( const QString& ProjectName );
    [[nodiscard]] QString SphinxConfText( const SphinxProjectSpec& Spec );
    [[nodiscard]] QString SphinxIndexText( const SphinxProjectSpec& Spec );
    /// 새 프로젝트에 고를 수 있는 테마. 가상 프로젝트 테마 목록과 같다.
    [[nodiscard]] QStringList SphinxThemeNames();
    /// 폴더와 파일을 만든다. 성공하면 빈 문자열을 돌려주고 IndexPath 에 index.rst 의 경로를 적는다.
    /// 이미 있는 파일은 덮어쓰지 않는다.
    [[nodiscard]] QString CreateSphinxProject( const SphinxProjectSpec& Spec, QString* IndexPath );

    /// ‘새 Sphinx 프로젝트’ 대화상자. 왼쪽은 입력, 오른쪽은 만들어질 파일과 그 내용이다.
    /// 호스트 펌프를 막지 않도록 open() 으로 연다. 만들면 Created() 로 index.rst 의 경로를 알린다.
    class NewSphinxProjectDialog final : public QDialog
    {
        Q_OBJECT

    public:
        struct Options
        {
            QString      directory_;   // 처음 보일 위치
            QString      author_;      // 지난번에 쓴 저자
            QKeySequence repeatKey_;   // 열린 채 누르면 만든다( file.new 의 키 )
        };

        NewSphinxProjectDialog( QWidget* Parent, const Options& Value );
        [[nodiscard]] SphinxProjectSpec Spec() const;

    signals:
        void Created( const QString& IndexPath, const hc::document::SphinxProjectSpec& Spec );

    public slots:
        void accept() override;

    protected:
        void keyPressEvent( QKeyEvent* Event ) override;

    private:
        void refresh();

        Options         options_;
        QLineEdit*      name_ = nullptr;
        QLineEdit*      author_ = nullptr;
        QLineEdit*      directory_ = nullptr;
        QLineEdit*      folder_ = nullptr;
        QComboBox*      language_ = nullptr;
        QComboBox*      theme_ = nullptr;
        QRadioButton*   flat_ = nullptr;
        QRadioButton*   separate_ = nullptr;
        QCheckBox*      myst_ = nullptr;
        QTabWidget*     preview_ = nullptr;
        QTreeWidget*    tree_ = nullptr;
        QPlainTextEdit* conf_ = nullptr;
        QPlainTextEdit* index_ = nullptr;
        QLabel*         message_ = nullptr;
        QPushButton*    create_ = nullptr;
        bool            folderEdited_ = false;
    };

    /// ‘빌드’ 대화상자: 빌더와 출력 위치를 고른다. 마지막으로 고른 빌더와 ‘끝나면 폴더 열기’를 기억한다.
    class SphinxBuildDialog final : public QDialog
    {
        Q_OBJECT

    public:
        SphinxBuildDialog( const QString& ProjectLabel, const QString& ProjectRoot, QWidget* Parent );
        [[nodiscard]] QString Builder() const;
        [[nodiscard]] QString OutputDirectory() const;

    public slots:
        void accept() override;

    private:
        void refresh();
        [[nodiscard]] QString defaultOutput() const;

        QString      projectRoot_;
        QComboBox*   builder_ = nullptr;
        QLineEdit*   output_ = nullptr;
        QCheckBox*   open_ = nullptr;
        QPushButton* build_ = nullptr;
        bool         outputEdited_ = false;
    };
}
