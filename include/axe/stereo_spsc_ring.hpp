#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace axe {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "audio sequence counters require lock-free 64-bit atomics");
static_assert(std::atomic<std::int32_t>::is_always_lock_free &&
                  std::atomic<bool>::is_always_lock_free,
              "audio status atomics must be lock-free");

struct StereoFrame {
    float left{};
    float right{};
};

// A fixed-capacity, single-producer/single-consumer ring for interleaved stereo
// frames. Construction allocates storage once; push/peek/consume do not allocate
// or lock. Exactly one thread may call producer operations and exactly one
// thread may call consumer operations at a time.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // intentional cache-line separation of SPSC cursors
#endif
class StereoSpscRing final {
public:
    class ConsumerView final {
    public:
        [[nodiscard]] std::size_t available() const noexcept {
            return static_cast<std::size_t>(write_ - read_);
        }

        [[nodiscard]] bool peek(std::size_t offset, StereoFrame& frame) const noexcept {
            if (offset >= available()) {
                return false;
            }
            std::size_t index = read_index_ + offset;
            if (index >= ring_->capacity_) {
                index -= ring_->capacity_;
            }
            frame = ring_->frames_[index];
            return true;
        }

        void consume(std::size_t count) noexcept {
            if (ring_->capacity_ == 0) {
                return;
            }
            read_ += count;
            read_index_ += count;
            if (read_index_ >= ring_->capacity_) {
                read_index_ %= ring_->capacity_;
            }
        }

        [[nodiscard]] std::size_t discard_before(std::uint64_t sequence) noexcept {
            const std::uint64_t distance = sequence - read_;
            if (distance > available()) {
                return 0;
            }
            const auto count = static_cast<std::size_t>(distance);
            consume(count);
            return count;
        }

        // Refresh makes frames published by the producer during this callback
        // visible without reloading the consumer's sequence for each sample.
        void refresh() noexcept {
            write_ = ring_->write_sequence_.load(std::memory_order_acquire);
        }

        // Publish the final consumer cursor once at the end of the callback.
        void commit() noexcept {
            ring_->read_sequence_.store(read_, std::memory_order_release);
        }

    private:
        friend class StereoSpscRing;
        ConsumerView(StereoSpscRing* ring, std::uint64_t read,
                     std::uint64_t write) noexcept
            : ring_(ring), read_(read), write_(write),
              read_index_(ring->capacity_ == 0 ? 0 : static_cast<std::size_t>(read % ring->capacity_)) {}

        StereoSpscRing* ring_;
        std::uint64_t read_;
        std::uint64_t write_;
        std::size_t read_index_;
    };

    explicit StereoSpscRing(std::size_t capacity_frames)
        : frames_(capacity_frames == 0 ? nullptr : new StereoFrame[capacity_frames]),
          capacity_(capacity_frames) {}

    StereoSpscRing(const StereoSpscRing&) = delete;
    StereoSpscRing& operator=(const StereoSpscRing&) = delete;
    StereoSpscRing(StereoSpscRing&&) = delete;
    StereoSpscRing& operator=(StereoSpscRing&&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // Producer side. Used to mark a source discontinuity boundary without
    // moving or resetting the consumer cursor.
    [[nodiscard]] std::uint64_t producer_position() const noexcept {
        return write_sequence_.load(std::memory_order_relaxed);
    }

    // Producer side. Returns the number of frames accepted. New frames are
    // rejected when full; the producer never moves the consumer cursor.
    [[nodiscard]] std::size_t push(const float* interleaved,
                                   std::size_t frame_count) noexcept {
        return push_mapped(interleaved, frame_count, [](float left, float right) noexcept {
            return StereoFrame{left, right};
        });
    }

    template <class Mapper>
    [[nodiscard]] std::size_t push_mapped(const float* interleaved,
                                          std::size_t frame_count,
                                          Mapper&& map) noexcept {
        if (interleaved == nullptr || capacity_ == 0 || frame_count == 0) {
            return 0;
        }
        const std::uint64_t write = write_sequence_.load(std::memory_order_relaxed);
        const std::uint64_t read = read_sequence_.load(std::memory_order_acquire);
        const std::size_t used = static_cast<std::size_t>(write - read);
        const std::size_t accepted = frame_count < capacity_ - used
                                         ? frame_count
                                         : capacity_ - used;
        for (std::size_t i = 0; i < accepted; ++i) {
            frames_[static_cast<std::size_t>((write + i) % capacity_)] =
                map(interleaved[i * 2], interleaved[i * 2 + 1]);
        }
        if (accepted != 0) {
            write_sequence_.store(write + accepted, std::memory_order_release);
        }
        return accepted;
    }

    [[nodiscard]] std::size_t push_silence(std::size_t frame_count) noexcept {
        if (capacity_ == 0 || frame_count == 0) {
            return 0;
        }
        const std::uint64_t write = write_sequence_.load(std::memory_order_relaxed);
        const std::uint64_t read = read_sequence_.load(std::memory_order_acquire);
        const std::size_t used = static_cast<std::size_t>(write - read);
        const std::size_t accepted = frame_count < capacity_ - used
                                         ? frame_count
                                         : capacity_ - used;
        for (std::size_t i = 0; i < accepted; ++i) {
            frames_[static_cast<std::size_t>((write + i) % capacity_)] = {};
        }
        if (accepted != 0) {
            write_sequence_.store(write + accepted, std::memory_order_release);
        }
        return accepted;
    }

    // Consumer side. Offset zero names the next frame to consume. The returned
    // copy remains coherent until this consumer advances with consume().
    [[nodiscard]] bool peek(std::size_t offset, StereoFrame& frame) const noexcept {
        const std::uint64_t read = read_sequence_.load(std::memory_order_relaxed);
        const std::uint64_t write = write_sequence_.load(std::memory_order_acquire);
        if (offset >= static_cast<std::size_t>(write - read)) {
            return false;
        }
        frame = frames_[static_cast<std::size_t>((read + offset) % capacity_)];
        return true;
    }

    [[nodiscard]] std::size_t available() const noexcept {
        // Load read first. An observer must not combine a newer consumer cursor
        // with an older producer cursor, which would underflow the subtraction.
        const std::uint64_t read = read_sequence_.load(std::memory_order_acquire);
        const std::uint64_t write = write_sequence_.load(std::memory_order_acquire);
        const std::uint64_t distance = write - read;
        return distance <= capacity_ ? static_cast<std::size_t>(distance) : capacity_;
    }

    [[nodiscard]] ConsumerView begin_consumer_read() noexcept {
        const std::uint64_t read = read_sequence_.load(std::memory_order_relaxed);
        const std::uint64_t write = write_sequence_.load(std::memory_order_acquire);
        return ConsumerView(this, read, write);
    }

    // Consumer side. count must not exceed available().
    void consume(std::size_t count) noexcept {
        const std::uint64_t read = read_sequence_.load(std::memory_order_relaxed);
        read_sequence_.store(read + count, std::memory_order_release);
    }

private:
    std::unique_ptr<StereoFrame[]> frames_;
    const std::size_t capacity_;
    alignas(64) std::atomic<std::uint64_t> write_sequence_{0};
    alignas(64) std::atomic<std::uint64_t> read_sequence_{0};
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace axe
