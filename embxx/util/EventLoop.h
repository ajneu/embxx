//
// Copyright 2013 (C). Alex Robenko. All rights reserved.
//

/// @file util/EventLoop.h
/// Contains EventLoop class definition.

#pragma once

#include <cstddef>
#include <type_traits>
#include <mutex>

#include "embxx/container/StaticQueue.h"

namespace embxx
{

namespace util
{

/// @addtogroup util
/// @{

/// @brief Implements basic event loop for bare metal platform.
/// @details Provides an ability to post new handlers to be executed in
///          non-interrupt context.
/// @tparam TSize Size in bytes to be allocated as data member for handlers
///         registration. It cannot be changed afterwards.
/// @tparam TLock "Lockable" class. It must provide the following functions:
///         @li @code void lock(); @endcode
///         @li @code void unlock(); @endcode
///
///         This lock must have a default constructor. The lock is used to
///         update the queue of pending handlers.
/// @tparam TCond Wait condition variable class. It must have a default
///         constructor and provide the following functions:
///         @li @code template <typename TLock> void wait(TLock& lock); @endcode
///         @li @code void notify_all(); @endcode
/// @headerfile embxx/util/EventLoop.h
template <std::size_t TSize,
          typename TLock,
          typename TCond>
class EventLoop
{
public:
    /// @brief Type of the lock
    typedef TLock LockType;

    /// @brief Type of the condition variable
    typedef TCond CondType;

    /// @brief Constructor.
    EventLoop();

    /// @brief Destructor
    ~EventLoop() = default;

    /// @brief Get reference to the lock.
    LockType& getLock();

    /// @brief Get reference to the condition variable
    CondType& getCond();

    /// @brief Post new handler for execution.
    /// @details Acquires lock before calling postNoLock(). See postNoLock()
    ///          for details.
    /// @param[in] task R-value reference to new handler functor.
    /// @return true in case the handler was successfully posted, false if
    ///         there is not enough space in the execution queue.
    /// @note Thread safety: Safe
    /// @note Exception guarantee: Basic
    template <typename TTask>
    bool post(TTask&& task);

    /// @brief Post new handler for execution.
    /// @details No lock is acquired. The task is added to the execution queue.
    ///          If the execution queue is empty before the new handler is
    ///          added, the condition variable is signalled by calling its
    ///          notify_all() member function.
    /// @param[in] task R-value reference to new handler functor.
    /// @return true in case the handler was successfully posted, false if
    ///         there is not enough space in the execution queue.
    /// @note Thread safety: Unsafe
    /// @note Exception guarantee: Basic
    template <typename TTask>
    bool postNoLock(TTask&& task);

    /// @brief Event loop execution function.
    /// @details The function keeps executing posted handlers until none
    ///          are left. When execution queue becomes empty the wait(...)
    ///          member function of the condition variable gets called to
    ///          execute blocking wait for new handlers. When new handler
    ///          is added, the condition variable will be signalled and blocking
    ///          wait is expected to be terminated to continue execution of
    ///          the event loop. This function never exits unless stop() was
    ///          called to terminate the execution. After stopping the main
    ///          loop use reset() member function to enable the loop to be
    ///          executed again.
    /// @note Thread safety: Unsafe
    /// @note Exception guarantee: Basic
    void run();

    /// @brief Stop execution of the event loop.
    /// @details The execution may not be stopped immediately. If there is an
    ///          event handler being executed, the loop will be stopped after
    ///          the execution of the handler is finished.
    /// @note Thread safety: Safe
    /// @note Exception guarantee: No throw
    void stop();

    /// @brief Reset the state of the event loop.
    /// @details Clear the queue of registered event handlers and resets the
    ///          "stopped" flag to allow new event loop execution.
    /// @note Thread safety: Unsafe
    /// @note Exception guarantee: Basic
    void reset();

private:

    /// @cond DOCUMENT_EVENT_LOOP_TASK
    class Task
    {
    public:
        virtual ~Task();
        virtual std::size_t getSize() const;
        virtual void exec();
    };

    template <typename TTask>
    class TaskBound : public Task
    {

    public:
        explicit TaskBound(TTask&& task);
        virtual ~TaskBound();

        virtual std::size_t getSize() const;
        virtual void exec();

        static const std::size_t Size =
            ((sizeof(TaskBound<TTask>) - 1) / sizeof(Task)) + 1;

    private:
        TTask task_;
    };
    /// @endcond

    typedef typename
        std::aligned_storage<
            sizeof(Task),
            std::alignment_of<Task>::value
        >::type ArrayElemType;

    static const std::size_t ArraySize = TSize / sizeof(Task);
    typedef embxx::container::StaticQueue<ArrayElemType, ArraySize> EventQueue;

    ArrayElemType* getAllocPlace(std::size_t requiredQueueSize);

    EventQueue queue_;
    LockType lock_;
    CondType cond_;
    volatile bool stopped_;
};

/// @}

// Implementation
template <std::size_t TSize,
          typename TLock,
          typename TCond>
EventLoop<TSize, TLock, TCond>::EventLoop()
    : stopped_(false)
{
}

template <std::size_t TSize,
          typename TLock,
          typename TCond>
typename EventLoop<TSize, TLock, TCond>::LockType&
EventLoop<TSize, TLock, TCond>::getLock()
{
    return lock_;
}

template <std::size_t TSize,
          typename TLock,
          typename TCond>
typename EventLoop<TSize, TLock, TCond>::CondType&
EventLoop<TSize, TLock, TCond>::getCond()
{
    return cond_;
}

template <std::size_t TSize,
          typename TLock,
          typename TCond>
template <typename TTask>
bool EventLoop<TSize, TLock, TCond>::post(TTask&& task)
{
    std::lock_guard<LockType> guard(lock_);
    return postNoLock(std::forward<TTask>(task));
}

template <std::size_t TSize,
          typename TLock,
          typename TCond>
template <typename TTask>
bool EventLoop<TSize, TLock, TCond>::postNoLock(TTask&& task)
{
    typedef TaskBound<typename std::decay<TTask>::type> TaskBoundType;
    static_assert(std::alignment_of<Task>::value == std::alignment_of<TaskBoundType>::value,
        "Alignment of TaskBound must be same as alignment of Task");

    static const std::size_t requiredQueueSize = TaskBoundType::Size;

    bool wasEmpty = queue_.isEmpty();

    auto placePtr = getAllocPlace(requiredQueueSize);
    if (placePtr == nullptr) {
        return false;
    }

    auto taskPtr = new (placePtr) TaskBoundType(std::forward<TTask>(task));
    static_cast<void>(taskPtr);

    if (wasEmpty) {
        cond_.notify_all();
    }
    return true;
}

template <std::size_t TSize,
          typename TLock,
          typename TCond>
void EventLoop<TSize, TLock, TCond>::run()
{
    while (true) {
        std::unique_lock<LockType> guard(lock_);
        while ((!queue_.isEmpty()) && (!stopped_)) {
            auto taskPtr = reinterpret_cast<Task*>(&queue_.front());
            guard.unlock();
            taskPtr->exec();
            auto sizeToRemove = taskPtr->getSize();
            taskPtr->~Task();
            guard.lock();
            queue_.popFront(sizeToRemove);
        }

        if (stopped_) {
            break;
        }

        cond_.wait(guard);
    }
}

template <std::size_t TSize,
          typename TLock,
          typename TCond>
void EventLoop<TSize, TLock, TCond>::stop()
{
    stopped_ = true;
    cond_.notify_all();
}

template <std::size_t TSize,
          typename TLock,
          typename TCond>
void EventLoop<TSize, TLock, TCond>::reset()
{
    std::lock_guard<LockType> guard(lock_);
    stopped_ = false;
    queue_.clear();
}

/// @cond DOCUMENT_EVENT_LOOP_TASK
template <std::size_t TSize,
          typename TLock,
          typename TCond>
EventLoop<TSize, TLock, TCond>::Task::~Task()
{
}

template <std::size_t TSize,
          typename TLock,
          typename TCond>
std::size_t EventLoop<TSize, TLock, TCond>::Task::getSize() const
{
    return 1;
}

template <std::size_t TSize,
          typename TLock,
          typename TCond>
void EventLoop<TSize, TLock, TCond>::Task::exec()
{
}

template <std::size_t TSize,
          typename TLock,
          typename TCond>
template <typename TTask>
EventLoop<TSize, TLock, TCond>::TaskBound<TTask>::TaskBound(TTask&& task)
    : task_(task)
{
}

template <std::size_t TSize,
          typename TLock,
          typename TCond>
template <typename TTask>
EventLoop<TSize, TLock, TCond>::TaskBound<TTask>::~TaskBound()
{
}

template <std::size_t TSize,
          typename TLock,
          typename TCond>
template <typename TTask>
std::size_t EventLoop<TSize, TLock, TCond>::TaskBound<TTask>::getSize() const
{
    return Size;
}

template <std::size_t TSize,
          typename TLock,
          typename TCond>
template <typename TTask>
void EventLoop<TSize, TLock, TCond>::TaskBound<TTask>::exec()
{
    task_();
}

/// @endcond

template <std::size_t TSize,
          typename TLock,
          typename TCond>
typename EventLoop<TSize, TLock, TCond>::ArrayElemType*
EventLoop<TSize, TLock, TCond>::getAllocPlace(
    std::size_t requiredQueueSize)
{
    auto invalidIter = queue_.invalidIter();
    while (true)
    {
        if ((queue_.capacity() - queue_.size()) < requiredQueueSize) {
            return nullptr;
        }

        auto curSize = queue_.size();
        if (queue_.isLinearised()) {
            auto dist = std::distance(queue_.arrayTwo().second, invalidIter);
            if ((0 < dist) && (dist < requiredQueueSize)) {
                queue_.resize(curSize + 1);
                auto placePtr = static_cast<void*>(&queue_.back());
                auto taskPtr = new (placePtr) Task();
                static_cast<void>(taskPtr);
                continue;
            }
        }

        queue_.resize(curSize + requiredQueueSize);
        return &queue_[curSize];
    }
}

}  // namespace util

}  // namespace embxx
