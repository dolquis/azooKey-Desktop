#pragma once

#ifdef _WIN32

#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>
#include <thread>

#include "StderrLineQueue.h"

namespace azookey::host {

// A pipe reader must not control the lifetime of the Host or its learning flush.
// Install before any Host workers and destroy after they have joined. Host
// std::cerr pipe I/O runs only on this thread; cancelling it cannot abort file
// saves. The Host's llama callback also uses cerr; direct third-party C stdio
// diagnostics do not use this buffer.
class StderrPipeWriter final : private std::streambuf {
 public:
  StderrPipeWriter() {
    pipe_ = GetStdHandle(STD_ERROR_HANDLE);
    if (pipe_ == nullptr || pipe_ == INVALID_HANDLE_VALUE || GetFileType(pipe_) != FILE_TYPE_PIPE) {
      return;
    }
    worker_ = std::thread([this] { WriteLoop(); });
    previous_buffer_ = std::cerr.rdbuf(this);
    // cerr normally flushes cout before every insertion. That implicit stdout
    // write would escape the console handler's stdio cancellation gate.
    previous_tie_ = std::cerr.tie(nullptr);
  }

  StderrPipeWriter(const StderrPipeWriter&) = delete;
  StderrPipeWriter& operator=(const StderrPipeWriter&) = delete;

  ~StderrPipeWriter() override {
    if (!worker_.joinable()) return;
    std::cerr.rdbuf(previous_buffer_);
    std::cerr.tie(previous_tie_);
    std::unique_lock<std::mutex> lock(mutex_);
    closing_ = true;
    changed_.notify_all();
    // Let a responsive reader receive final diagnostics. A stalled reader is
    // best-effort: discard its queue and repeatedly cancel to cover the race
    // between WriteLoop checking discard_ and entering WriteFile.
    if (!changed_.wait_for(lock, std::chrono::milliseconds(100), [this] { return done_; })) {
      discard_ = true;
      queue_.Clear();
      changed_.notify_all();
      while (!done_) {
        CancelSynchronousIo(worker_.native_handle());
        changed_.wait_for(lock, std::chrono::milliseconds(10), [this] { return done_; });
      }
    }
    lock.unlock();
    worker_.join();
  }

 private:
  std::streamsize xsputn(const char* text, std::streamsize size) override {
    if (size <= 0) return 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.Append(std::string_view(text, static_cast<size_t>(size)));
    }
    changed_.notify_all();
    // Queue saturation drops diagnostics without putting cerr into fail state,
    // so a later diagnostic can still be emitted when the reader recovers.
    return size;
  }

  int_type overflow(int_type value) override {
    if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
    const char byte = traits_type::to_char_type(value);
    xsputn(&byte, 1);
    return value;
  }

  int sync() override { return 0; }

  void WriteLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      changed_.wait(lock, [this] { return closing_ || !queue_.Empty(); });
      if (discard_ || (closing_ && queue_.Empty())) break;
      std::string batch = queue_.Take();
      lock.unlock();
      size_t offset = 0;
      while (offset < batch.size()) {
        lock.lock();
        const bool discard = discard_;
        lock.unlock();
        if (discard) break;
        DWORD written = 0;
        if (!WriteFile(pipe_, batch.data() + offset, static_cast<DWORD>(batch.size() - offset),
                       &written, nullptr) ||
            written == 0) {
          break;
        }
        offset += written;
      }
      lock.lock();
    }
    done_ = true;
    changed_.notify_all();
  }

  HANDLE pipe_ = nullptr;
  std::streambuf* previous_buffer_ = nullptr;
  std::ostream* previous_tie_ = nullptr;
  std::mutex mutex_;
  std::condition_variable changed_;
  detail::StderrLineQueue queue_;
  bool closing_ = false;
  bool discard_ = false;
  bool done_ = false;
  std::thread worker_;
};

}  // namespace azookey::host

#endif
