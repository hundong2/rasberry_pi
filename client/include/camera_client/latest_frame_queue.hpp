// 템플릿/인라인 구현이 여러 번 포함돼도 한 번만 선언되게 한다.
#pragma once

#include "camera_client/frame.hpp"

// 시간 단위, 스레드 대기, 잠금, 값 존재 여부와 이동 연산에 필요한 헤더다.
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace camera_client {

/**
 * 최대 한 장만 저장하는 스레드 안전 큐다.
 *
 * 새 push는 아직 소비되지 않은 이전 Frame을 교체한다. 큐가 무한히 쌓이지 않으므로
 * 메모리 사용량이 제한되고 오래된 영상의 완전 전송보다 낮은 지연을 우선한다.
 */
class LatestFrameQueue {
 public:
  /**
   * 가장 최신 Frame을 저장하고 기다리는 소비자 하나를 깨운다.
   *
   * @param frame JPEG vector 소유권을 큐 안으로 이동시킬 값이다.
   * @return 소비되지 않은 이전 Frame을 교체했으면 true, 아니면 false다. 단, 닫힌
   *         큐도 데이터를 받지 않으며 false를 반환하므로 호출자는 close와 구분하지 않는다.
   */
  bool push(Frame frame) {
    // lock_guard 생성자는 mutex_.lock(), 소멸자는 mutex_.unlock()을 자동 호출한다.
    std::lock_guard lock(mutex_);
    if (closed_) {
      // 닫힌 뒤에는 상태를 바꾸지 않는다. false는 기존 프레임 교체가 없다는 뜻이다.
      return false;
    }

    // optional::has_value()는 값이 있으면 true, 비어 있으면 false를 반환한다.
    const bool replaced = latest_.has_value();
    // std::move는 frame을 이동 가능한 값으로 바꿔 큰 JPEG 벡터의 복사를 피한다.
    latest_ = std::move(frame);
    // notify_one()은 반환값 없이 대기 중인 소비자 하나를 깨운다.
    ready_.notify_one();
    return replaced;
  }

  /**
   * 최신 Frame이 들어올 때까지 제한 시간 동안 기다린 뒤 꺼낸다.
   *
   * @param timeout 호출 스레드가 대기할 최대 steady-clock 시간이다.
   * @return Frame이 있으면 소유권을 가진 optional, timeout/close이면 nullopt다.
   */
  std::optional<Frame> pop_for(std::chrono::milliseconds timeout) {
    // condition_variable은 대기 중 mutex를 풀고 깨어날 때 다시 잠글 수 있는
    // unique_lock을 요구한다.
    std::unique_lock lock(mutex_);
    // wait_for는 predicate가 true면 true를, 제한 시간이 지나면 false를 반환한다.
    // [this]는 람다가 현재 객체의 closed_와 latest_를 읽게 한다.
    const bool ready = ready_.wait_for(lock, timeout, [this] {
      return closed_ || latest_.has_value();
    });
    if (!ready || !latest_) {
      // nullopt는 optional 안에 Frame이 없다는 명시적인 반환값이다.
      return std::nullopt;
    }

    // *latest_는 optional 내부 Frame 참조이며 이동 후 result가 JPEG를 소유한다.
    Frame result = std::move(*latest_);
    // reset()은 이동되고 남은 Frame을 파괴하여 optional을 빈 상태로 만든다.
    latest_.reset();
    return result;
  }

  /** 큐를 닫고 모든 대기자를 깨운다. 여러 번 호출해도 최종 상태가 같은 멱등 연산이다. */
  void close() {
    // 잠금을 잡은 동안 closed_와 latest_를 함께 변경하여 일관성을 지킨다.
    std::lock_guard lock(mutex_);
    closed_ = true;
    latest_.reset();
    // notify_all()은 반환값 없이 대기 중인 모든 소비자를 깨워 종료하게 한다.
    ready_.notify_all();
  }

 private:
  // 아래 네 멤버는 mutex_로 보호되는 하나의 공유 상태다.
  std::mutex mutex_;
  std::condition_variable ready_;
  std::optional<Frame> latest_;
  bool closed_{false};
};

}  // namespace camera_client
