#include "thread.h"

#include "log.h"
#include "util.h"

namespace sylar {

// thread_local表示，每个线程都有独立的变量样本
// 做到全局可访问+线程间不可共享
static thread_local Thread* t_thread = nullptr;
static thread_local std::string t_thread_name = "UNKNOW";

static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

Thread* Thread::GetThis() {
    return t_thread;
}

const std::string& Thread::GetName() {
    return t_thread_name;
}

void Thread::SetName(const std::string& name) {
    if (name.empty()) {
        return;
    }
    if (t_thread) {
        t_thread->m_name = name;
    }
    t_thread_name = name;
}

Thread::Thread(std::function<void()> cb, const std::string& name) : m_cb(cb), m_name(name) {
    if (name.empty()) {
        m_name = "UNKNOW";
    }
    int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);
    if (rt) {
        SYLAR_LOG_ERROR(g_logger) << "pthread_create thread fail, rt=" << rt << " name=" << name;
        throw std::logic_error("pthread_create error");
    }
    m_semaphore.wait();
}

Thread::~Thread() {
    if (m_thread) {
        pthread_detach(m_thread);
    }
}

void Thread::join() {
    if (m_thread) {
        int rt = pthread_join(m_thread, nullptr);
        if (rt) {
            SYLAR_LOG_ERROR(g_logger)
                << "pthread_join thread fail, rt=" << rt << " name=" << m_name;
            throw std::logic_error("pthread_join error");
        }
        m_thread = 0;
    }
}

void* Thread::run(void* arg) {
    Thread* thread = (Thread*)arg;
    t_thread = thread;
    t_thread_name = thread->m_name;
    thread->m_id = sylar::GetThreadId();
    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

    std::function<void()> cb;
    /*
     * thread->m_cb 是成员变量。
     * 如果 cb() 运行时间很长，而外部因为某种原因销毁了 Thread 对象，直接访问成员变量可能会导致崩溃
     * std::function 可能捕获了大量的闭包变量（智能指针等）。执行 swap 后，thread->m_cb 变为空
     *当线程执行完毕时，局部变量 cb 自动析构，它所持有的资源会立即释放，而不需要等到 Thread 对象析构
     */
    cb.swap(thread->m_cb);

    thread->m_semaphore.notify();

    cb();
    return 0;
}

}  // namespace sylar
