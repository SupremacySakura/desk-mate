#include "gate3/recovery_output.hpp"

#include <vector>

namespace deskmate::gate3 {

RecoveryOutputRouter::RecoveryOutputRouter(DecoderOutputOps &operations) : operations_(operations) {}

bool RecoveryOutputRouter::BindControlThread()
{
    std::lock_guard lock(mutex_);
    if (control_thread_bound_) {
        return false;
    }
    control_thread_ = std::this_thread::get_id();
    control_thread_bound_ = true;
    return true;
}

bool RecoveryOutputRouter::StartDecoder(DecoderInstanceId decoder)
{
    std::lock_guard lock(mutex_);
    if (!IsControlThreadLocked() || decoder == 0) {
        return false;
    }
    current_decoder_ = decoder;
    ++recovery_epoch_;
    output_open_ = true;
    return true;
}

bool RecoveryOutputRouter::BeginRecovery(DecoderInstanceId decoder)
{
    std::lock_guard lock(mutex_);
    if (!IsControlThreadLocked() || decoder == 0) {
        return false;
    }
    current_decoder_ = decoder;
    ++recovery_epoch_;
    output_open_ = false;
    return true;
}

bool RecoveryOutputRouter::OpenRecoveryOutput(DecoderInstanceId decoder)
{
    std::lock_guard lock(mutex_);
    if (!IsControlThreadLocked() || decoder == 0 || decoder != current_decoder_) {
        return false;
    }
    output_open_ = true;
    return true;
}

bool RecoveryOutputRouter::SetSurfaceRenderable(bool renderable)
{
    std::lock_guard lock(mutex_);
    if (!IsControlThreadLocked()) {
        return false;
    }
    surface_renderable_fast_.store(renderable, std::memory_order_release);
    surface_renderable_ = renderable;
    if (!renderable) {
        last_good_frame_visible_ = false;
    }
    return true;
}

void RecoveryOutputRouter::InvalidateSurfaceFromCallback()
{
    surface_renderable_fast_.store(false, std::memory_order_release);
    {
        std::lock_guard gate(render_gate_);
    }
    std::lock_guard lock(mutex_);
    ++recovery_epoch_;
    output_open_ = false;
    surface_renderable_ = false;
    last_good_frame_visible_ = false;
}

bool RecoveryOutputRouter::Drain()
{
    bool all_operations_ok = true;
    std::deque<PendingFree> pending_frees;
    {
        std::lock_guard lock(mutex_);
        if (!IsControlThreadLocked()) {
            return false;
        }
        pending_frees.swap(pending_frees_);
        stats_.pending_free_buffers = pending_frees.size();
    }

    std::deque<PendingFree> retry_frees;
    const auto free_output = [&](PendingFree pending) {
        if (operations_.FreeOutput(pending.command.decoder, pending.command.index)) {
            std::lock_guard lock(mutex_);
            if (pending.stale_epoch) {
                ++stats_.freed_stale_epoch;
            } else {
                ++stats_.freed_surface_invalid;
            }
        } else {
            std::lock_guard lock(mutex_);
            ++stats_.free_failures;
            retry_frees.push_back(pending);
            all_operations_ok = false;
        }
    };
    for (const PendingFree &pending : pending_frees) {
        free_output(pending);
    }

    std::vector<Command> commands;
    {
        std::lock_guard lock(mutex_);
        while (!commands_.empty()) {
            commands.push_back(commands_.front());
            commands_.pop_front();
        }
    }

    for (const Command &command : commands) {
        bool render = false;
        bool release = false;
        bool stale_release = false;
        {
            std::lock_guard lock(mutex_);
            if (command.decoder != current_decoder_) {
                ++stats_.residual_decoder_callbacks;
                continue;
            }
            if (!command.accepted_when_posted || command.epoch != recovery_epoch_) {
                release = true;
                stale_release = true;
            } else if (!surface_renderable_fast_.load(std::memory_order_acquire)) {
                release = true;
            } else {
                render = true;
            }
        }

        if (render) {
            bool rendered = false;
            {
                std::lock_guard gate(render_gate_);
                if (surface_renderable_fast_.load(std::memory_order_acquire)) {
                    rendered = operations_.RenderOutput(command.decoder, command.index);
                } else {
                    release = true;
                    stale_release = false;
                }
            }
            if (!rendered && !release &&
                !surface_renderable_fast_.load(std::memory_order_acquire)) {
                release = true;
                stale_release = false;
            }
            if (rendered) {
                std::lock_guard lock(mutex_);
                ++stats_.rendered;
                last_good_frame_visible_ = true;
            } else if (release) {
                free_output(PendingFree{command, stale_release});
            } else {
                std::lock_guard lock(mutex_);
                ++stats_.render_failures;
                all_operations_ok = false;
            }
        } else if (release) {
            free_output(PendingFree{command, stale_release});
        }
    }
    {
        std::lock_guard lock(mutex_);
        pending_frees_.insert(pending_frees_.end(), retry_frees.begin(), retry_frees.end());
        stats_.pending_free_buffers = pending_frees_.size();
    }
    return all_operations_ok;
}

void RecoveryOutputRouter::PostOutputBuffer(DecoderInstanceId decoder, std::uint32_t index)
{
    std::lock_guard lock(mutex_);
    ++stats_.callbacks;
    commands_.push_back(Command{
        decoder,
        index,
        recovery_epoch_,
        output_open_ && decoder == current_decoder_ &&
            surface_renderable_fast_.load(std::memory_order_acquire),
    });
}

RecoveryEpoch RecoveryOutputRouter::recovery_epoch() const
{
    std::lock_guard lock(mutex_);
    return recovery_epoch_;
}

DecoderInstanceId RecoveryOutputRouter::current_decoder() const
{
    std::lock_guard lock(mutex_);
    return current_decoder_;
}

bool RecoveryOutputRouter::output_open() const
{
    std::lock_guard lock(mutex_);
    return output_open_;
}

bool RecoveryOutputRouter::surface_renderable() const
{
    return surface_renderable_fast_.load(std::memory_order_acquire);
}

bool RecoveryOutputRouter::last_good_frame_visible() const
{
    std::lock_guard lock(mutex_);
    return last_good_frame_visible_;
}

OutputRouterStats RecoveryOutputRouter::stats() const
{
    std::lock_guard lock(mutex_);
    return stats_;
}

bool RecoveryOutputRouter::IsControlThreadLocked() const
{
    return control_thread_bound_ && std::this_thread::get_id() == control_thread_;
}

} // namespace deskmate::gate3
