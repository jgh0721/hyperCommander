#include "stdafx.h"
#include "startupFontWarmup.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리
#include "commonLibs/cmnIncludeLibrary.hpp"

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QEvent>
#include <QLoggingCategory>
#include <QTimer>
#include <QWidget>
#include <Windows.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <cstring>

DECLARE_CMNLIBSV2_NAMESPACE

Q_LOGGING_CATEGORY(lcStartupFontWarmup, "hc.fonts.startup")

namespace hc::fonts
{
    struct StartupFontWarmupState
    {
        std::atomic_bool stop_{false};
        std::atomic_bool release_{false};
        std::atomic<UINT32> nextFamily_{0};
        std::mutex mutex_;
        std::condition_variable changed_;
    };

    namespace
    {
        using Microsoft::WRL::ComPtr;

        bool SkipWarmup(int Argc, char** Argv)
        {
            // Compare the opt-out verbatim: whitespace and other values enable it.
            wchar_t Value[128]{};
            if (GetEnvironmentVariableW(L"HC_FONT_WARMUP", Value, 128) == 1 && Value[0] == L'0') return true;
            const auto Length = GetEnvironmentVariableW(L"QT_QPA_PLATFORM", Value, 128);
            if (Length && Length < 128
                && (wcscmp(Value, L"offscreen") == 0 || wcscmp(Value, L"minimal") == 0
                    || wcsncmp(Value, L"offscreen:", 10) == 0 || wcsncmp(Value, L"minimal:", 8) == 0)) return true;
            for (int Index = 1; Index < Argc; ++Index) {
                if (strcmp(Argv[Index], "-platform") == 0 && Index + 1 < Argc
                    && (strcmp(Argv[Index + 1], "offscreen") == 0 || strcmp(Argv[Index + 1], "minimal") == 0)) return true;
                if (strcmp(Argv[Index], "--help") == 0 || strcmp(Argv[Index], "--version") == 0
                    || strcmp(Argv[Index], "--headless") == 0 || strcmp(Argv[Index], "--verify-bundle-resources") == 0) return true;
            }
            return false;
        }

        unsigned WorkerCount()
        {
            // Count physical cores, so a two-core SMT CPU still uses one worker.
            DWORD Bytes = 0;
            GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &Bytes);
            if (!Bytes) return 1;
            std::vector<unsigned char> Buffer(Bytes);
            if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(Buffer.data()), &Bytes)) return 1;
            unsigned Cores = 0;
            for (DWORD Offset = 0; Offset < Bytes;) {
                const auto* Item = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(Buffer.data() + Offset);
                if (!Item->Size || Item->Size > Bytes - Offset) break;
                ++Cores;
                Offset += Item->Size;
            }
            return std::clamp(Cores / 2, 1u, 4u);
        }

        void ReadNames(IDWriteLocalizedStrings* Names)
        {
            if (!Names) return;
            wchar_t Locale[LOCALE_NAME_MAX_LENGTH]{};
            GetUserDefaultLocaleName(Locale, LOCALE_NAME_MAX_LENGTH);
            const wchar_t* Locales[] = {Locale, L"en-us"};
            for (const auto* Name : Locales) {
                UINT32 Index = 0, Length = 0;
                BOOL Exists = FALSE;
                if (FAILED(Names->FindLocaleName(Name, &Index, &Exists)) || !Exists
                    || FAILED(Names->GetStringLength(Index, &Length))) continue;
                std::vector<wchar_t> Text(static_cast<size_t>(Length) + 1);
                Names->GetString(Index, Text.data(), Length + 1);
            }
        }

        void ReadInformation(IDWriteFont* Font, DWRITE_INFORMATIONAL_STRING_ID Id)
        {
            ComPtr<IDWriteLocalizedStrings> Names;
            BOOL Exists = FALSE;
            if (SUCCEEDED(Font->GetInformationalStrings(Id, &Names, &Exists)) && Exists) ReadNames(Names.Get());
        }

        void WarmFamily(IDWriteFontFamily2* Family, const StartupFontWarmupState& State,
            std::vector<ComPtr<IDWriteFontFace>>& Faces)
        {
            ComPtr<IDWriteLocalizedStrings> Names;
            if (SUCCEEDED(Family->GetFamilyNames(&Names))) ReadNames(Names.Get());
            for (UINT32 Index = 0; Index < Family->GetFontCount() && !State.stop_.load(); ++Index) {
                ComPtr<IDWriteFont3> Font;
                if (SUCCEEDED(Family->GetFont(Index, &Font)))
                    ReadInformation(Font.Get(), DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES);
            }
            ComPtr<IDWriteFontList> Matching;
            // Call the same base-interface overload as Qt's populateFamily().
            if (FAILED(static_cast<IDWriteFontFamily*>(Family)->GetMatchingFonts(DWRITE_FONT_WEIGHT_REGULAR,
                DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &Matching))) return;
            for (UINT32 Index = 0; Index < Matching->GetFontCount() && !State.stop_.load(); ++Index) {
                ComPtr<IDWriteFont> Font;
                ComPtr<IDWriteFont2> Font2;
                if (FAILED(Matching->GetFont(Index, &Font)) || FAILED(Font.As(&Font2))) continue;
                ComPtr<IDWriteFontFamily> FontFamily;
                if (SUCCEEDED(Font2->GetFontFamily(&FontFamily))) {
                    Names.Reset();
                    if (SUCCEEDED(FontFamily->GetFamilyNames(&Names))) ReadNames(Names.Get());
                }
                ReadInformation(Font.Get(), DWRITE_INFORMATIONAL_STRING_WIN32_FAMILY_NAMES);
                Font2->GetStretch(); Font2->GetStyle(); Font2->GetWeight();
                Font2->IsMonospacedFont(); Font2->IsColorFont();
                ComPtr<IDWriteFontFace> Face;
                if (FAILED(Font->CreateFontFace(&Face))) continue;
                const void* Table = nullptr;
                UINT32 Size = 0;
                void* Context = nullptr;
                BOOL Exists = FALSE;
                if (SUCCEEDED(Face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('O', 'S', '/', '2'),
                    &Table, &Size, &Context, &Exists)) && Exists) Face->ReleaseFontTable(Context);
                Names.Reset();
                if (SUCCEEDED(Font2->GetFaceNames(&Names))) ReadNames(Names.Get());
                ReadInformation(Font.Get(), DWRITE_INFORMATIONAL_STRING_WIN32_SUBFAMILY_NAMES);
                // DirectWrite's useful face cache is weak. Retain these on the
                // worker until Qt has completed its startup family population.
                Faces.push_back(std::move(Face));
            }
        }

        void WarmCollection(const std::shared_ptr<StartupFontWarmupState>& State,
            const ComPtr<IDWriteFontCollection2>& Collection) noexcept
        {
            const auto Apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try {
                std::vector<ComPtr<IDWriteFontFace>> Faces;
                while (!State->stop_.load() && !State->release_.load()) {
                    const auto Index = State->nextFamily_.fetch_add(1);
                    if (Index >= Collection->GetFontFamilyCount()) break;
                    ComPtr<IDWriteFontFamily2> Family;
                    if (SUCCEEDED(Collection->GetFontFamily(Index, &Family))) WarmFamily(Family.Get(), *State, Faces);
                }
                std::unique_lock Lock(State->mutex_);
                State->changed_.wait(Lock, [&] { return State->stop_.load() || State->release_.load(); });
                // Faces and collection references are released here, never by a
                // GUI timer or a process-wide static destructor.
            } catch (...) { /* Optional cache warming must never fail startup. */ }
            if (SUCCEEDED(Apartment)) CoUninitialize();
        }

        void StartWorkers(const std::shared_ptr<StartupFontWarmupState>& State) noexcept
        {
            const auto Apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try {
                ComPtr<IDWriteFactory6> Factory;
                ComPtr<IDWriteFontCollection2> Collection;
                if (!State->stop_.load() && SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                    __uuidof(IDWriteFactory6), reinterpret_cast<IUnknown**>(Factory.GetAddressOf())))
                    && SUCCEEDED(Factory->GetSystemFontCollection(FALSE, DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC, &Collection))) {
                    const auto Count = WorkerCount();
                    for (unsigned Index = 1; Index < Count && !State->stop_.load(); ++Index) {
                        try { std::thread([State, Collection] { WarmCollection(State, Collection); }).detach(); }
                        catch (...) { break; }
                    }
                    WarmCollection(State, Collection);
                }
            } catch (...) { /* No warnings, Qt calls or shared static lifetime. */ }
            if (SUCCEEDED(Apartment)) CoUninitialize();
        }

        class FirstWindow final : public QObject
        {
        public:
            FirstWindow(QApplication* App, std::weak_ptr<StartupFontWarmupState> State)
                : QObject(App), state_(std::move(State))
            {
                App->installEventFilter(this);
                // Embedded native surfaces may never show a top-level QWidget.
                // A GUI timer cannot expire while Qt's startup font calls block
                // the UI thread; a worker wall-clock deadline could expire then.
                QTimer::singleShot(30000, this, [this] { if (!windowShown_) releaseFaces(); });
                // HyperCommander shows its window inside its QApplication
                // subclass constructor, before main regains control.
                for (const auto* Window : App->topLevelWidgets())
                    if (Window->isVisible()) { scheduleRelease(); break; }
            }
        protected:
            bool eventFilter(QObject* Object, QEvent* Event) override
            {
                if (Event->type() != QEvent::Show) return false;
                const auto* Window = qobject_cast<QWidget*>(Object);
                if (!Window || !Window->isWindow()) return false;
                scheduleRelease();
                return false;
            }
        private:
            void scheduleRelease()
            {
                windowShown_ = true;
                qApp->removeEventFilter(this);
                QTimer::singleShot(10000, this, [this] { releaseFaces(); });
            }
            void releaseFaces()
            {
                if (const auto Shared = state_.lock()) {
                    { std::lock_guard Lock(Shared->mutex_); Shared->release_.store(true); }
                    Shared->changed_.notify_all();
                }
            }
            std::weak_ptr<StartupFontWarmupState> state_;
            bool windowShown_ = false;
        };
    }

    StartupFontWarmup::StartupFontWarmup(int Argc, char** Argv) noexcept
    {
        try {
            if (SkipWarmup(Argc, Argv)) return;
            state_ = std::make_shared<StartupFontWarmupState>();
            std::thread([State = state_] { StartWorkers(State); }).detach();
        } catch (...) { state_.reset(); }
    }

    StartupFontWarmup::~StartupFontWarmup()
    {
        // Detached workers own all their state/COM objects and use no Qt or
        // destructible globals. Exit never waits for a font API or worker join.
        if (state_) {
            // This mutex protects only the wait predicate, never a font call or
            // COM release. Taking it also prevents a lost wake at early exit.
            { std::lock_guard Lock(state_->mutex_); state_->stop_.store(true); }
            state_->changed_.notify_all();
        }
    }

    void StartupFontWarmup::WatchFirstWindow()
    {
        if (state_ && qApp) new FirstWindow(qApp, state_);
    }
}
