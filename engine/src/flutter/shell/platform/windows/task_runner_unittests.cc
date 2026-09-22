// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/task_runner.h"

#include <atomic>
#include <chrono>

#include "flutter/fml/macros.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/shell/platform/windows/task_runner_window.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {

namespace {
class MockTaskRunner : public TaskRunner {
 public:
  MockTaskRunner(CurrentTimeProc get_current_time,
                 const TaskExpiredCallback& on_task_expired)
      : TaskRunner(get_current_time, on_task_expired) {}

  virtual bool RunsTasksOnCurrentThread() const override { return true; }

  void SimulateTimerAwake() { ProcessTasks(); }

 protected:
  virtual void WakeUp() override {
    // Do nothing to avoid processing tasks immediately after the tasks is
    // posted.
  }

  virtual TaskTimePoint GetCurrentTimeForTask() const override {
    return TaskTimePoint(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(10000)));
  }

 private:
  FML_DISALLOW_COPY_AND_ASSIGN(MockTaskRunner);
};

uint64_t MockGetCurrentTime() {
  return 10000;
}
}  // namespace

TEST(TaskRunnerTest, MaybeExecuteTaskWithExactOrder) {
  std::vector<uint64_t> executed_task_order;
  auto runner =
      MockTaskRunner(MockGetCurrentTime,
                     [&executed_task_order](const FlutterTask* expired_task) {
                       executed_task_order.push_back(expired_task->task);
                     });

  uint64_t time_now = MockGetCurrentTime();

  runner.PostFlutterTask(FlutterTask{nullptr, 1}, time_now);
  runner.PostFlutterTask(FlutterTask{nullptr, 2}, time_now);
  runner.PostTask(
      [&executed_task_order]() { executed_task_order.push_back(3); });
  runner.PostTask(
      [&executed_task_order]() { executed_task_order.push_back(4); });

  runner.SimulateTimerAwake();

  std::vector<uint64_t> posted_task_order{1, 2, 3, 4};
  EXPECT_EQ(executed_task_order, posted_task_order);
}

TEST(TaskRunnerTest, MaybeExecuteTaskOnlyExpired) {
  std::set<uint64_t> executed_task;
  auto runner = MockTaskRunner(
      MockGetCurrentTime, [&executed_task](const FlutterTask* expired_task) {
        executed_task.insert(expired_task->task);
      });

  uint64_t task_expired_before_now = 1;
  uint64_t time_before_now = 0;
  runner.PostFlutterTask(FlutterTask{nullptr, task_expired_before_now},
                         time_before_now);

  uint64_t task_expired_after_now = 2;
  uint64_t time_after_now = MockGetCurrentTime() * 2;
  runner.PostFlutterTask(FlutterTask{nullptr, task_expired_after_now},
                         time_after_now);

  runner.SimulateTimerAwake();

  std::set<uint64_t> only_task_expired_before_now{task_expired_before_now};
  EXPECT_EQ(executed_task, only_task_expired_before_now);
}

TEST(TaskRunnerTest, TimerThreadDoesNotCancelEarlierScheduledTasks) {
  fml::AutoResetWaitableEvent signaled;
  std::optional<std::chrono::high_resolution_clock::time_point> callback_time;
  TimerThread timer_thread([&]() {
    callback_time = std::chrono::high_resolution_clock::now();
    signaled.Signal();
  });
  auto now = std::chrono::high_resolution_clock::now();
  // Make sure that subsequent call to schedule does not cancel earlier task
  // as documented in TimerThread::ScheduleAt.
  timer_thread.ScheduleAt(now + std::chrono::milliseconds(20));
  timer_thread.ScheduleAt(now + std::chrono::seconds(10));
  timer_thread.Start();

  const bool timed_out =
      signaled.WaitWithTimeout(fml::TimeDelta::FromSeconds(2));
  timer_thread.Stop();

  EXPECT_FALSE(timed_out);
  ASSERT_TRUE(callback_time.has_value());
  EXPECT_GE(*callback_time, now + std::chrono::milliseconds(20));
}

class TestTimerThread : public TimerThread {
 public:
  using TimerThread::TimerThread;

  bool WaitUntilParked(const std::shared_ptr<int>& callback_state) {
    return WaitUntil([this, &callback_state]() {
      // The test and callback_ each own the state. The running callback's
      // local copy is destroyed with mutex_ held before entering the next
      // wait, so observing two owners here also ensures it has finished.
      return next_fire_time_ ==
                 std::chrono::high_resolution_clock::time_point::max() &&
             callback_state.use_count() == 2;
    });
  }

  bool WaitUntilStopped() {
    return WaitUntil([this]() { return callback_ == nullptr; });
  }

 private:
  template <typename Predicate>
  bool WaitUntil(Predicate predicate) {
    const auto timeout =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < timeout) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (predicate()) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }
};

TEST(TaskRunnerTest, TimerThreadSchedulesDelayedTaskAfterParking) {
  fml::AutoResetWaitableEvent signaled;
  auto callback_state = std::make_shared<int>(0);
  std::optional<std::chrono::high_resolution_clock::time_point> callback_time;
  TestTimerThread timer_thread([&, callback_state]() {
    (void)callback_state;
    callback_time = std::chrono::high_resolution_clock::now();
    signaled.Signal();
  });
  timer_thread.ScheduleAt(std::chrono::high_resolution_clock::now());
  timer_thread.Start();

  const bool first_timed_out =
      signaled.WaitWithTimeout(fml::TimeDelta::FromSeconds(2));
  const bool parked =
      !first_timed_out && timer_thread.WaitUntilParked(callback_state);
  const auto deadline =
      std::chrono::high_resolution_clock::now() + std::chrono::milliseconds(50);
  bool delayed_timed_out = true;
  if (parked) {
    // No other input or scheduling notifications should be needed to wake the
    // timer when this newly scheduled deadline arrives.
    timer_thread.ScheduleAt(deadline);
    delayed_timed_out =
        signaled.WaitWithTimeout(fml::TimeDelta::FromSeconds(2));
  }
  timer_thread.Stop();

  EXPECT_FALSE(first_timed_out);
  ASSERT_TRUE(parked);
  EXPECT_FALSE(delayed_timed_out);
  ASSERT_TRUE(callback_time.has_value());
  EXPECT_GE(*callback_time, deadline);
}

TEST(TaskRunnerTest, TimerThreadPreservesDeadlineScheduledDuringCallback) {
  fml::AutoResetWaitableEvent signaled;
  std::optional<std::chrono::high_resolution_clock::time_point> deadline;
  std::optional<std::chrono::high_resolution_clock::time_point> callback_time;
  TimerThread* timer = nullptr;
  TimerThread timer_thread([&]() {
    if (!deadline.has_value()) {
      deadline = std::chrono::high_resolution_clock::now() +
                 std::chrono::milliseconds(50);
      timer->ScheduleAt(*deadline);
    } else {
      callback_time = std::chrono::high_resolution_clock::now();
      signaled.Signal();
    }
  });
  timer = &timer_thread;
  timer_thread.ScheduleAt(std::chrono::high_resolution_clock::now());
  timer_thread.Start();

  const bool timed_out =
      signaled.WaitWithTimeout(fml::TimeDelta::FromSeconds(2));
  timer_thread.Stop();

  EXPECT_FALSE(timed_out);
  ASSERT_TRUE(deadline.has_value());
  ASSERT_TRUE(callback_time.has_value());
  EXPECT_GE(*callback_time, *deadline);
}

TEST(TaskRunnerTest, TimerThreadPreservesConcurrentScheduleDuringCallback) {
  fml::AutoResetWaitableEvent callback_entered;
  fml::AutoResetWaitableEvent callback_can_finish;
  fml::AutoResetWaitableEvent signaled;
  bool first_callback = true;
  std::optional<std::chrono::high_resolution_clock::time_point> callback_time;
  TimerThread timer_thread([&]() {
    if (first_callback) {
      first_callback = false;
      callback_entered.Signal();
      callback_can_finish.Wait();
    } else {
      callback_time = std::chrono::high_resolution_clock::now();
      signaled.Signal();
    }
  });
  timer_thread.ScheduleAt(std::chrono::high_resolution_clock::now());
  timer_thread.Start();
  const bool first_timed_out =
      callback_entered.WaitWithTimeout(fml::TimeDelta::FromSeconds(2));
  const auto deadline =
      std::chrono::high_resolution_clock::now() + std::chrono::milliseconds(50);
  timer_thread.ScheduleAt(deadline);
  callback_can_finish.Signal();
  const bool delayed_timed_out =
      signaled.WaitWithTimeout(fml::TimeDelta::FromSeconds(2));
  timer_thread.Stop();

  EXPECT_FALSE(first_timed_out);
  EXPECT_FALSE(delayed_timed_out);
  ASSERT_TRUE(callback_time.has_value());
  EXPECT_GE(*callback_time, deadline);
}

TEST(TaskRunnerTest, TimerThreadStopKeepsRunningCallbackAlive) {
  fml::AutoResetWaitableEvent callback_entered;
  fml::AutoResetWaitableEvent callback_can_finish;
  auto callback_state = std::make_shared<int>(0);
  std::weak_ptr<int> weak_callback_state = callback_state;
  TestTimerThread timer_thread(
      [callback_state, &callback_entered, &callback_can_finish]() {
        (void)callback_state;
        callback_entered.Signal();
        callback_can_finish.Wait();
      });
  callback_state.reset();
  timer_thread.ScheduleAt(std::chrono::high_resolution_clock::now());
  timer_thread.Start();
  const bool timed_out =
      callback_entered.WaitWithTimeout(fml::TimeDelta::FromSeconds(2));

  std::thread stopper([&]() { timer_thread.Stop(); });
  const bool stopped = timer_thread.WaitUntilStopped();
  const bool callback_alive = !weak_callback_state.expired();
  callback_can_finish.Signal();
  stopper.join();

  EXPECT_FALSE(timed_out);
  EXPECT_TRUE(stopped);
  EXPECT_TRUE(callback_alive);
  EXPECT_TRUE(weak_callback_state.expired());
}

class TestTaskRunnerWindow : public TaskRunnerWindow {
 public:
  TestTaskRunnerWindow() : TaskRunnerWindow() {}
};

TEST(TaskRunnerTest, EmptyTaskRunnerReturnsNanoSecondsMax) {
  TaskRunner runner(MockGetCurrentTime, [](const FlutterTask*) {});
  auto res = runner.ProcessTasks();
  EXPECT_EQ(res, std::chrono::nanoseconds::max());
}

TEST(TaskRunnerTest, TaskRunnerWindowCoalescesWakeUpMessages) {
  class Delegate : public TaskRunnerWindow::Delegate {
   public:
    Delegate() {}

    std::chrono::nanoseconds ProcessTasks() override {
      process_tasks_call_count_++;
      return std::chrono::nanoseconds::max();
    }

    int process_tasks_call_count_ = 0;
  };

  Delegate delegate;
  TestTaskRunnerWindow window;

  window.AddDelegate(&delegate);

  window.WakeUp();
  window.WakeUp();

  ::MSG msg;
  while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  EXPECT_EQ(delegate.process_tasks_call_count_, 1);
}

}  // namespace testing
}  // namespace flutter
