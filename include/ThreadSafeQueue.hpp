#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>

template <typename T>
class ThreadSafeQueue {
public:
    ThreadSafeQueue(size_t max_size = 100) : m_max_size(max_size) {}

    // 生产者调用：将任务压入队列
    bool push(T item) {
        std::unique_lock<std::mutex> lock(m_mutex);
        // 如果队列满了，可以选择阻塞或者直接丢弃（这里演示满载时拒绝，实现防雪崩丢帧）
        if (m_queue.size() >= m_max_size) {
            return false; 
        }
        m_queue.push(std::move(item));
        lock.unlock();
        m_cond.notify_one(); // 唤醒一个正在等待的消费者（AI线程）
        return true;
    }

    // 消费者（AI线程）调用：阻塞获取任务
    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cond.wait(lock, [this]() { 
            return !m_queue.empty() || m_stop; 
        });
        
        if (m_stop && m_queue.empty()) {
            return false;
        }
        
        item = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    void stop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_stop = true;
        m_cond.notify_all();
    }

private:
    std::queue<T> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    size_t m_max_size;
    bool m_stop = false;
};