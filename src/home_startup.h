// Bounded first-page readiness. All timestamps use the monotonic UI clock.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

class HomeStartupGate {
public:
    static constexpr double kMaximumWait = 8.0;
    static constexpr double kProviderGrace = 1.5;
    static constexpr double kArtworkGrace = 1.25;
    static constexpr double kSettle = 0.30;

    struct State {
        bool content = false;
        bool leading_catalogs_settled = false;
        bool account_settled = false;
        bool all_catalogs_settled = false;
        bool artwork_ready = false;
        bool textures_ready = false;
        std::uint64_t signature = 0;
    };
    enum class Result { Waiting, Ready, Partial, Empty, Deadline };

    void begin(double now) {
        active_ = true;
        started_ = changed_ = now;
        content_at_ = usable_at_ = -1;
        have_signature_ = false;
        result_ = Result::Waiting;
    }
    void cancel() { active_ = false; }
    bool active() const { return active_; }
    Result result() const { return result_; }
    double elapsed(double now) const { return now - started_; }

    Result update(double now, const State& state) {
        if (!active_) return result_;
        if (!have_signature_ || signature_ != state.signature) {
            signature_ = state.signature;
            have_signature_ = true;
            changed_ = now;
        }
        if (state.content && content_at_ < 0) content_at_ = now;
        // No callback, failed artwork request or slow provider can restart
        // this deadline. Empty and partial pages remain navigable afterward.
        if (now - started_ >= kMaximumWait) return finish(Result::Deadline);
        const bool stable = now - changed_ >= kSettle;
        if (!state.content) {
            if (stable && state.all_catalogs_settled && state.account_settled)
                return finish(Result::Empty);
            return Result::Waiting;
        }
        const bool providers_ready = state.leading_catalogs_settled && state.account_settled;
        if (!providers_ready && now - content_at_ < kProviderGrace) return Result::Waiting;
        if (usable_at_ < 0) usable_at_ = now;
        const bool art_ready = state.artwork_ready && state.textures_ready;
        if (stable && (art_ready || now - usable_at_ >= kArtworkGrace))
            return finish(providers_ready && art_ready ? Result::Ready : Result::Partial);
        return Result::Waiting;
    }

private:
    Result finish(Result value) { active_ = false; return result_ = value; }
    bool active_ = false, have_signature_ = false;
    double started_ = 0, changed_ = 0, content_at_ = -1, usable_at_ = -1;
    std::uint64_t signature_ = 0;
    Result result_ = Result::Waiting;
};
