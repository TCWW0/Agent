#pragma once

#include <maya/app/app.hpp>
#include <maya/core/motion.hpp>

#include <cstdint>

namespace my_agent::ui {

// Adapts Maya's renderer-side animation requests to the event loop owned by
// my_agent. The bridge is intentionally host-side: Maya does not need to know
// about RepaintClock or this application's poll loop.
class MayaAnimationHostScope {
public:
    using RequestFrame = void (*)(void*) noexcept;
    using RequestFrameAfter = void (*)(void*, std::int64_t) noexcept;

    MayaAnimationHostScope(
        void* context,
        RequestFrame request_frame,
        RequestFrameAfter request_frame_after) noexcept
        : context_{context},
          request_frame_{request_frame},
          request_frame_after_{request_frame_after},
          previous_scope_{active_},
          previous_hook_{maya::anim::detail::raf_hook},
          previous_after_hook_{maya::anim::detail::raf_after_hook},
          previous_requested_{maya::detail::animation_requested_},
          previous_delay_ms_{maya::detail::next_frame_delay_ms_}
    {
        active_ = this;
        maya::anim::detail::raf_hook = &request_frame_thunk;
        maya::anim::detail::raf_after_hook = &request_frame_after_thunk;
        reset_pending();
    }

    ~MayaAnimationHostScope()
    {
        flush();
        maya::anim::detail::raf_hook = previous_hook_;
        maya::anim::detail::raf_after_hook = previous_after_hook_;
        active_ = previous_scope_;
        maya::detail::animation_requested_ = previous_requested_;
        maya::detail::next_frame_delay_ms_ = previous_delay_ms_;
    }

    MayaAnimationHostScope(const MayaAnimationHostScope&) = delete;
    MayaAnimationHostScope& operator=(const MayaAnimationHostScope&) = delete;

    // Consume direct Maya requests made while building/rendering a frame.
    // Motion primitives use the hooked fast path above; widgets that call
    // request_animation_frame[_after] directly are collected here.
    void flush() noexcept
    {
        if (!maya::detail::animation_requested_) {
            return;
        }
        const std::int64_t delay = maya::detail::next_frame_delay_ms_;
        reset_pending();
        if (delay > 0 && request_frame_after_ != nullptr) {
            request_frame_after_(context_, delay);
        } else if (request_frame_ != nullptr) {
            request_frame_(context_);
        }
    }

private:
    inline static thread_local MayaAnimationHostScope* active_ = nullptr;

    void* context_ = nullptr;
    RequestFrame request_frame_ = nullptr;
    RequestFrameAfter request_frame_after_ = nullptr;
    MayaAnimationHostScope* previous_scope_ = nullptr;
    maya::anim::detail::RafHook previous_hook_ = nullptr;
    maya::anim::detail::RafAfterHook previous_after_hook_ = nullptr;
    bool previous_requested_ = false;
    std::int64_t previous_delay_ms_ = -1;

    static void request_frame_thunk() noexcept
    {
        if (active_ != nullptr && active_->request_frame_ != nullptr) {
            active_->request_frame_(active_->context_);
        }
    }

    static void request_frame_after_thunk(std::int64_t delay_ms) noexcept
    {
        if (active_ != nullptr
            && active_->request_frame_after_ != nullptr) {
            active_->request_frame_after_(active_->context_, delay_ms);
        } else {
            request_frame_thunk();
        }
    }

    static void reset_pending() noexcept
    {
        maya::detail::animation_requested_ = false;
        maya::detail::next_frame_delay_ms_ = -1;
    }
};

}  // namespace my_agent::ui
