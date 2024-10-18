#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <queue>

// 调用智能指针返回值的标签
enum class WaitPopRetPtr : int8_t
{
    ptr
};

/* 线程安全的并发队列 */
template <class T>
class ConcurrentQueue
{
  private:
    mutable std::mutex mux_;
    std::condition_variable cond_;
    std::queue<T> que_;

  public:
    ConcurrentQueue() = default;

    auto push(T new_val) -> void
    {
        std::lock_guard lock(mux_);
        que_.push(std::move(new_val));
        cond_.notify_one();
    }

    auto waitPop() -> T
    {
        std::unique_lock lock(mux_);
        cond_.wait(lock, [this]
                   { return !que_.empty(); });
        T res = std::move(que_.front());
        que_.pop();
        return res;
    }

    auto waitPop(WaitPopRetPtr ret_ptr) -> std::shared_ptr<T>
    {
        (void)ret_ptr;
        std::lock_guard lock(mux_);
        cond_.wait(lock, [this]
                   { return !que_.empty(); });
        auto res = make_shared<T>(move(que_.front()));
        que_.pop();
        return res;
    }

    auto tryPop() -> std::optional<T>
    {
        std::lock_guard lock(mux_);
        if (que_.empty())
        {
            return std::nullopt;
        }
        T res = move(que_.front());
        que_.pop();
        return res;
    }

    auto tryPop(WaitPopRetPtr ret_ptr) -> std::optional<std::shared_ptr<T>>
    {
        (void)ret_ptr;
        std::lock_guard lock(mux_);
        if (que_.empty())
        {
            return std::nullopt;
        }
        auto res = make_shared<T>(move(que_.front()));
        que_.pop();
        return res;
    }

    auto empty() const -> bool
    {
        std::lock_guard lock(mux_);
        return que_.empty();
    }
};
