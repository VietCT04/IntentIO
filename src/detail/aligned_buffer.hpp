#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <system_error>
#include <utility>

namespace intentio::detail {

class AlignedBuffer {
public:
    AlignedBuffer() noexcept = default;

    static AlignedBuffer allocate(std::size_t size,
                                  std::size_t requested_alignment,
                                  std::error_code& error) noexcept {
        error.clear();
        if (size == 0) {
            return {};
        }

        std::size_t alignment = std::max(requested_alignment,
                                         alignof(std::max_align_t));
        const std::size_t minimum_alignment = sizeof(void*);
        alignment = std::max(alignment, minimum_alignment);
        if ((alignment & (alignment - 1U)) != 0U) {
            std::size_t power = minimum_alignment;
            while (power < alignment) {
                if (power > std::numeric_limits<std::size_t>::max() / 2U) {
                    error = std::make_error_code(std::errc::invalid_argument);
                    return {};
                }
                power *= 2U;
            }
            alignment = power;
        }

        void* allocation = nullptr;
        const int result = ::posix_memalign(&allocation, alignment, size);
        if (result != 0) {
            error = std::error_code(result, std::generic_category());
            return {};
        }
        return AlignedBuffer(allocation, size);
    }

    ~AlignedBuffer() { std::free(data_); }

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    AlignedBuffer(AlignedBuffer&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)) {}

    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
        if (this != &other) {
            std::free(data_);
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    [[nodiscard]] std::byte* data() noexcept {
        return static_cast<std::byte*>(data_);
    }

    [[nodiscard]] const std::byte* data() const noexcept {
        return static_cast<const std::byte*>(data_);
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    AlignedBuffer(void* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    void* data_{nullptr};
    std::size_t size_{};
};

} // namespace intentio::detail
