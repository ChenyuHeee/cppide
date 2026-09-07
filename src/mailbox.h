// mailbox.h —— 线程安全队列(header-only,Wave 1 即完成实现)
//
// 用途:UI 线程与工作线程(Runner / AiService)之间唯一的通信设施。
//
// 使用契约(违反会导致 UI 卡顿,请严格遵守):
//   * UI 线程**只允许**调用 tryPop() —— 永不阻塞。
//   * 工作线程用 waitPop(out, timeout_ms) 等活干,超时返回 false 后可去做别的事
//     (例如检查取消标志),从而做到“可被及时叫醒也可被及时终止”。
//   * close() 之后:所有等待者立刻被唤醒;push() 变成静默丢弃(不抛异常);
//     tryPop() 仍能把队列里的残余排空 —— 退出路径依赖这个语义。
//   * 临界区里只做 deque 的 push/pop 与 move,绝不调用用户代码,
//     所以持锁时间恒为常数级,UI 线程不会在锁上被饿死。
//   * T 需要可移动构造。事件类型(AppEvent)故意做成“重载荷用 shared_ptr”,
//     保证入队/出队都是廉价 move。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

template <class T>
class Mailbox {
 public:
  Mailbox() = default;
  ~Mailbox() = default;
  Mailbox(const Mailbox&) = delete;
  Mailbox& operator=(const Mailbox&) = delete;

  // 入队 + 唤醒一个等待者。已 close 则丢弃。
  void push(T v) {
    {
      std::lock_guard<std::mutex> lk(m_);
      if (closed_) return;
      q_.push_back(std::move(v));
    }
    cv_.notify_one();
  }

  // 非阻塞取。取到写入 out 并返回 true;队列空(无论是否已 close)返回 false。
  bool tryPop(T& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  // 阻塞取,最多等 timeout_ms 毫秒。
  //   timeout_ms < 0  -> 一直等到有数据或 close;
  //   timeout_ms == 0 -> 等价于 tryPop。
  // 返回 false 表示超时或“已 close 且队列已空”。
  bool waitPop(T& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_);
    if (q_.empty()) {
      if (closed_) return false;
      if (timeout_ms == 0) return false;
      auto ready = [this] { return !q_.empty() || closed_; };
      if (timeout_ms < 0) {
        cv_.wait(lk, ready);
      } else {
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), ready)) return false;
      }
      if (q_.empty()) return false;  // 被 close 唤醒
    }
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lk(m_);
    return q_.size();
  }

  bool empty() const {
    std::lock_guard<std::mutex> lk(m_);
    return q_.empty();
  }

  // 关闭:唤醒所有等待者,后续 push 被丢弃。幂等。
  void close() {
    {
      std::lock_guard<std::mutex> lk(m_);
      if (closed_) return;
      closed_ = true;
    }
    cv_.notify_all();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lk(m_);
    return closed_;
  }

  // 丢弃全部残余(退出期用,避免析构时跑一堆无意义的 T 析构逻辑)。
  void drain() {
    std::deque<T> tmp;
    {
      std::lock_guard<std::mutex> lk(m_);
      tmp.swap(q_);
    }
  }

 private:
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<T> q_;
  bool closed_ = false;
};
