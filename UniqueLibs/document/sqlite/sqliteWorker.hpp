#pragma once

/// 고유 라이브러리
#include "sqliteConnection.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <QtGlobal>

namespace hc::sqlite
{
    // 작업 스레드 하나(keyset 읽기 · 행 수 세기)가 연 연결의 수명을 GUI 쪽과 맞춘다.
    // - 취소된 뒤에 시작한 작업은 연결을 열지 않는다(Begin 이 false).
    // - GUI 는 Cancel 로 중단을 보내고, 창을 닫을 때 Wait 로 시작한 작업이 연결을 닫을 때까지 기다린다.
    //   탐색기는 열린 문서를 닫고 곧바로 이름을 바꾸므로, 닫힌 뒤 남은 연결이 파일을 쥐면 안 된다(설계 4.6).
    // - 결과는 Post 안에서 보낸다. Cancel 과 같은 잠금이라 GUI 가 주인을 지운 뒤에는 보내지 않는다.
    class WorkerJob
    {
    public:
        // 작업 스레드: 시작해도 되는가. true 면 끝에 반드시 Finish 를 부른다.
        bool Begin()
        {
            std::lock_guard Lock(mutex_);
            if (cancelled_) return false;
            started_ = true;
            return true;
        }
        // 작업 스레드: 연 연결을 걸어 둔다(Cancel 의 중단이 닿게). 이미 취소됐으면 false.
        bool Attach(Connection* Db)
        {
            std::lock_guard Lock(mutex_);
            if (cancelled_) return false;
            db_ = Db;
            return true;
        }
        void Detach()
        {
            std::lock_guard Lock(mutex_);
            db_ = nullptr;
        }
        // 작업 스레드: 연결을 닫은 뒤에 부른다.
        void Finish()
        {
            {
                std::lock_guard Lock(mutex_);
                db_ = nullptr;
                done_ = true;
            }
            finished_.notify_all();
        }
        template <class Fn> bool Post(Fn&& Send)
        {
            std::lock_guard Lock(mutex_);
            if (cancelled_) return false;
            Send();
            return true;
        }
        // GUI: 취소하고 진행 중인 문에 중단을 보낸다.
        void Cancel()
        {
            std::lock_guard Lock(mutex_);
            cancelled_ = true;
            if (db_) db_->Interrupt();
        }
        // GUI: 시작한 작업이 끝날 때까지 기다린다(시작하지 않은 작업은 연결을 열지 않으므로 바로 돌아온다).
        bool Wait(int TimeoutMs)
        {
            std::unique_lock Lock(mutex_);
            const bool Finished = finished_.wait_for(Lock, std::chrono::milliseconds(TimeoutMs), [this] { return !started_ || done_; });
            if (!Finished) qWarning("sqlite worker still running after %d ms (cancelled=%d attached=%d)", TimeoutMs, int(cancelled_), int(db_ != nullptr));
            return Finished;
        }
        bool Done() const
        {
            std::lock_guard Lock(mutex_);
            return !started_ || done_;
        }
    private:
        mutable std::mutex mutex_;
        std::condition_variable finished_;
        bool cancelled_ = false, started_ = false, done_ = false;
        Connection* db_ = nullptr;
    };
}
