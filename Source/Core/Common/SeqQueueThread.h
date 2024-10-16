// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>

#include "Common/Thread.h"

#include <SFML/Network/UdpSocket.hpp>

// A thread that executes the given function for every item placed into its queue.

namespace Common
{
template <typename T>
class SeqQueueThread
{
public:
  SeqQueueThread() = default;
  SeqQueueThread(const std::string_view name, std::function<void(T)> function)
  {
    Reset(name, std::move(function));
  }
  ~SeqQueueThread() { Shutdown(); }

  // Shuts the current work thread down (if any) and starts a new thread with the given function
  // Note: Some consumers of this API push items to the queue before starting the thread.
  void Reset(const std::string_view name)
  {
    Shutdown();
    std::lock_guard lg(m_lock);
    m_thread_name = name;
    m_shutdown = false;
    m_thread = std::thread(&SeqQueueThread::ThreadLoop, this);
  }

  // Adds an item to the work queue
  template <typename... Args>
  void EmplaceItem(Args&&... args)
  {
    std::lock_guard lg(m_lock);
    if (m_shutdown)
      return;

    m_items.emplace(std::forward<Args>(args)...);
    m_idle = false;
    m_worker_cond_var.notify_one();
  }

  // Adds an item to the work queue
  void Push(T&& item)
  {
    std::lock_guard lg(m_lock);
    if (m_shutdown)
      return;

    m_items.push(std::move(item));
    m_idle = false;
    m_worker_cond_var.notify_one();
  }

  // Adds an item to the work queue
  void Push(const T& item)
  {
    std::lock_guard lg(m_lock);
    if (m_shutdown)
      return;

    m_items.push(item);
    m_idle = false;
    m_worker_cond_var.notify_one();
  }

  // Empties the queue
  // If the worker polls IsCanceling(), it can abort it's work when Cancelling
  void Cancel()
  {
    std::unique_lock lg(m_lock);
    if (m_shutdown)
      return;

    m_cancelling = true;
    m_items = std::queue<T>();
    m_worker_cond_var.notify_one();
  }

  // Tells the worker to shut down when it's queue is empty
  // Blocks until the worker thread exits.
  // If cancel is true, will Cancel before before telling the worker to exit
  // Otherwise, all currently queued items will complete before the worker exits
  void Shutdown(bool cancel = false)
  {
    {
      std::unique_lock lg(m_lock);
      if (m_shutdown || !m_thread.joinable())
        return;

      if (cancel)
      {
        m_cancelling = true;
        m_items = std::queue<T>();
      }

      m_shutdown = true;
      m_worker_cond_var.notify_one();
    }

    m_thread.join();
  }

  // Blocks until all items in the queue have been processed (or cancelled)
  void WaitForCompletion()
  {
    std::unique_lock lg(m_lock);
    // don't check m_shutdown, because it gets set to request a shutdown, and we want to wait until
    // after the shutdown completes.
    // We also check m_cancelling, because we want to ensure the worker acknowledges our cancel.
    if (m_idle && !m_cancelling.load())
      return;

    m_wait_cond_var.wait(lg, [&] { return m_idle && !m_cancelling; });
  }

  // If the worker polls IsCanceling(), it can abort its work when Cancelling
  bool IsCancelling() const { return m_cancelling.load(); }

private:
  void ThreadLoop()
  {
    Common::SetCurrentThreadName(m_thread_name.c_str());

    while (true)
    {
      std::unique_lock lg(m_lock);
      while (m_items.empty())
      {
        m_idle = true;
        m_cancelling = false;
        m_wait_cond_var.notify_all();
        if (m_shutdown)
          return;

        m_worker_cond_var.wait(
            lg, [&] { return !m_items.empty() || m_shutdown || m_cancelling.load(); });
      }

      // Count how many chunks we can send, maximum of 100.
      // The max datagram size for UDP is 65507 bytes, so we must not pass that.
      size_t chunk_count = 1000;
      if (m_items.size() < chunk_count)
      {
        chunk_count = m_items.size();
      }

      if (chunk_count > 0)
      {
        // Read up to 100 chunks
        std::vector<std::string> chunks(chunk_count);
        for (int i = 0; i < chunk_count; i++)
        {
          T item{std::move(m_items.front())};
          m_items.pop();
          chunks[i] = item;
        }

        // Now we can unlock the mutex and let other chunks accumulate again
        lg.unlock();

        // Create single data stream of the chunk(s)
        std::string full_str;
        for (int i = 0; i < chunk_count; i++)
        {
          full_str += chunks[i];
        }

        // Send UDP data of multiple chunks
        sf::UdpSocket m_socket;
        if (m_socket.send(full_str.data(), full_str.size(), sf::IpAddress("localhost"), 12198) !=
            sf::Socket::Status::Done)
        {
          printf("SEQ UDPClient send failed");
        }
      }
      else
      {
        // No chunks to process, unlock mutex
        lg.unlock();
      }

    }
  }

  std::string m_thread_name;
  std::thread m_thread;
  std::mutex m_lock;
  std::queue<T> m_items;
  std::condition_variable m_wait_cond_var;
  std::condition_variable m_worker_cond_var;
  std::atomic<bool> m_cancelling = false;
  bool m_idle = true;
  bool m_shutdown = false;
};

}  // namespace Common
