#include "detail/backends.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <linux/io_uring.h>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace intentio::detail {

struct IoUringBackend::Impl {
    int ring_fd{-1};
    io_uring_params params{};

    void* sq_ring{MAP_FAILED};
    std::size_t sq_ring_size{};
    void* cq_ring{MAP_FAILED};
    std::size_t cq_ring_size{};
    io_uring_sqe* sqes{nullptr};
    std::size_t sqes_size{};

    unsigned* sq_head{};
    unsigned* sq_tail{};
    unsigned* sq_ring_mask{};
    unsigned* sq_ring_entries{};
    unsigned* sq_flags{};
    unsigned* sq_dropped{};
    unsigned* sq_array{};

    unsigned* cq_head{};
    unsigned* cq_tail{};
    unsigned* cq_ring_mask{};
    unsigned* cq_ring_entries{};
    io_uring_cqe* cqes{};

    ~Impl() {
        if (sqes != nullptr && reinterpret_cast<void*>(sqes) != MAP_FAILED) {
            ::munmap(sqes, sqes_size);
        }
        if (sq_ring != MAP_FAILED) {
            ::munmap(sq_ring, sq_ring_size);
        }
        if (cq_ring != MAP_FAILED && cq_ring != sq_ring) {
            ::munmap(cq_ring, cq_ring_size);
        }
        if (ring_fd >= 0) {
            ::close(ring_fd);
        }
    }
};

namespace {

[[noreturn]] void throw_errno(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

int io_uring_enter(int fd,
                   unsigned to_submit,
                   unsigned min_complete,
                   unsigned flags) {
    return static_cast<int>(::syscall(__NR_io_uring_enter,
                                      fd,
                                      to_submit,
                                      min_complete,
                                      flags,
                                      nullptr,
                                      0));
}

void submit_all(IoUringBackend::Impl& ring, unsigned count) {
    unsigned submitted = 0;
    while (submitted < count) {
        const int result = io_uring_enter(ring.ring_fd,
                                          count - submitted,
                                          0,
                                          0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("io_uring_enter submit");
        }
        if (result == 0) {
            throw std::system_error(std::make_error_code(std::errc::io_error),
                                    "io_uring_enter submitted no SQEs");
        }
        submitted += static_cast<unsigned>(result);
    }
}

void wait_for_one(IoUringBackend::Impl& ring) {
    for (;;) {
        const int result = io_uring_enter(ring.ring_fd,
                                          0,
                                          1,
                                          IORING_ENTER_GETEVENTS);
        if (result >= 0) {
            return;
        }
        if (errno != EINTR) {
            throw_errno("io_uring_enter wait");
        }
    }
}

} // namespace

IoUringBackend::IoUringBackend(std::size_t queue_depth)
    : impl_(new Impl) {
    if (queue_depth == 0 || queue_depth > std::numeric_limits<unsigned>::max()) {
        delete impl_;
        impl_ = nullptr;
        throw std::invalid_argument("io_uring queue depth must fit in unsigned");
    }

    impl_->params = {};
    impl_->ring_fd = static_cast<int>(::syscall(__NR_io_uring_setup,
                                                static_cast<unsigned>(queue_depth),
                                                &impl_->params));
    if (impl_->ring_fd < 0) {
        const int saved_errno = errno;
        delete impl_;
        impl_ = nullptr;
        throw std::system_error(saved_errno,
                                std::generic_category(),
                                "io_uring_setup");
    }

    const std::size_t sq_ring_size =
        impl_->params.sq_off.array +
        impl_->params.sq_entries * sizeof(unsigned);
    const std::size_t cq_ring_size =
        impl_->params.cq_off.cqes +
        impl_->params.cq_entries * sizeof(io_uring_cqe);
    const bool single_mmap = (impl_->params.features & IORING_FEAT_SINGLE_MMAP) != 0;

    impl_->sq_ring_size = single_mmap ? std::max(sq_ring_size, cq_ring_size)
                                      : sq_ring_size;
    impl_->sq_ring = ::mmap(nullptr,
                            impl_->sq_ring_size,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED,
                            impl_->ring_fd,
                            IORING_OFF_SQ_RING);
    if (impl_->sq_ring == MAP_FAILED) {
        const int saved_errno = errno;
        delete impl_;
        impl_ = nullptr;
        throw std::system_error(saved_errno,
                                std::generic_category(),
                                "mmap io_uring SQ ring");
    }

    if (single_mmap) {
        impl_->cq_ring = impl_->sq_ring;
        impl_->cq_ring_size = impl_->sq_ring_size;
    } else {
        impl_->cq_ring_size = cq_ring_size;
        impl_->cq_ring = ::mmap(nullptr,
                                impl_->cq_ring_size,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED,
                                impl_->ring_fd,
                                IORING_OFF_CQ_RING);
        if (impl_->cq_ring == MAP_FAILED) {
            const int saved_errno = errno;
            delete impl_;
            impl_ = nullptr;
            throw std::system_error(saved_errno,
                                    std::generic_category(),
                                    "mmap io_uring CQ ring");
        }
    }

    impl_->sqes_size = impl_->params.sq_entries * sizeof(io_uring_sqe);
    impl_->sqes = static_cast<io_uring_sqe*>(::mmap(nullptr,
                                                    impl_->sqes_size,
                                                    PROT_READ | PROT_WRITE,
                                                    MAP_SHARED,
                                                    impl_->ring_fd,
                                                    IORING_OFF_SQES));
    if (impl_->sqes == MAP_FAILED) {
        const int saved_errno = errno;
        delete impl_;
        impl_ = nullptr;
        throw std::system_error(saved_errno,
                                std::generic_category(),
                                "mmap io_uring SQEs");
    }

    auto* sq_base = static_cast<char*>(impl_->sq_ring);
    auto* cq_base = static_cast<char*>(impl_->cq_ring);
    impl_->sq_head = reinterpret_cast<unsigned*>(sq_base + impl_->params.sq_off.head);
    impl_->sq_tail = reinterpret_cast<unsigned*>(sq_base + impl_->params.sq_off.tail);
    impl_->sq_ring_mask =
        reinterpret_cast<unsigned*>(sq_base + impl_->params.sq_off.ring_mask);
    impl_->sq_ring_entries =
        reinterpret_cast<unsigned*>(sq_base + impl_->params.sq_off.ring_entries);
    impl_->sq_flags = reinterpret_cast<unsigned*>(sq_base + impl_->params.sq_off.flags);
    impl_->sq_dropped =
        reinterpret_cast<unsigned*>(sq_base + impl_->params.sq_off.dropped);
    impl_->sq_array = reinterpret_cast<unsigned*>(sq_base + impl_->params.sq_off.array);

    impl_->cq_head = reinterpret_cast<unsigned*>(cq_base + impl_->params.cq_off.head);
    impl_->cq_tail = reinterpret_cast<unsigned*>(cq_base + impl_->params.cq_off.tail);
    impl_->cq_ring_mask =
        reinterpret_cast<unsigned*>(cq_base + impl_->params.cq_off.ring_mask);
    impl_->cq_ring_entries =
        reinterpret_cast<unsigned*>(cq_base + impl_->params.cq_off.ring_entries);
    impl_->cqes = reinterpret_cast<io_uring_cqe*>(cq_base + impl_->params.cq_off.cqes);
}

IoUringBackend::~IoUringBackend() {
    delete impl_;
}

ExecutionStats IoUringBackend::execute(
    std::vector<PhysicalRead>& reads,
    const intentio::ExecutionPolicy& policy) {
    if (reads.empty()) {
        return {};
    }
    if (policy.max_in_flight == 0) {
        throw std::invalid_argument("execution max in-flight must be greater than zero");
    }

    const unsigned queue_depth = static_cast<unsigned>(std::min<std::size_t>(
        *impl_->sq_ring_entries, policy.max_in_flight));
    if (queue_depth == 0) {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                "io_uring queue depth is zero");
    }
    ExecutionStats stats;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t base = 0; base < reads.size();) {
        const std::size_t limit =
            std::min(reads.size(), base + static_cast<std::size_t>(queue_depth));
        std::vector<std::size_t> queued;
        queued.reserve(limit - base);

        unsigned sq_tail = *impl_->sq_tail;
        const unsigned sq_head = *impl_->sq_head;
        const unsigned available = *impl_->sq_ring_entries - (sq_tail - sq_head);
        if (available < limit - base) {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                    "io_uring submission queue is full");
        }

        for (std::size_t index = base; index < limit; ++index) {
            PhysicalRead& read = reads[index];
            if (read.request.length > std::numeric_limits<unsigned>::max()) {
                read.error = std::make_error_code(std::errc::value_too_large);
                continue;
            }

            const unsigned slot = sq_tail & *impl_->sq_ring_mask;
            io_uring_sqe& sqe = impl_->sqes[slot];
            std::memset(&sqe, 0, sizeof(sqe));
            sqe.opcode = IORING_OP_READ;
            sqe.fd = read.request.fd;
            sqe.off = read.request.offset;
            sqe.addr = reinterpret_cast<std::uint64_t>(read.data.data());
            sqe.len = static_cast<unsigned>(read.request.length);
            sqe.user_data = static_cast<std::uint64_t>(index);
            impl_->sq_array[slot] = slot;
            ++sq_tail;
            queued.push_back(index);
        }
        *impl_->sq_tail = sq_tail;

        if (!queued.empty()) {
            submit_all(*impl_, static_cast<unsigned>(queued.size()));
        }

        const auto batch_start = std::chrono::steady_clock::now();
        std::size_t completed = 0;
        while (completed < queued.size()) {
            unsigned cq_head = *impl_->cq_head;
            const unsigned cq_tail = *impl_->cq_tail;
            if (cq_head == cq_tail) {
                wait_for_one(*impl_);
                continue;
            }

            io_uring_cqe& cqe = impl_->cqes[cq_head & *impl_->cq_ring_mask];
            const std::size_t index = static_cast<std::size_t>(cqe.user_data);
            if (index < base || index >= limit) {
                *impl_->cq_head = cq_head + 1;
                continue;
            }

            PhysicalRead& read = reads[index];
            if (cqe.res < 0) {
                read.error = std::error_code(-cqe.res, std::generic_category());
            } else {
                read.bytes_read = static_cast<std::size_t>(cqe.res);
            }
            *impl_->cq_head = cq_head + 1;
            ++completed;
        }

        const auto batch_stop = std::chrono::steady_clock::now();
        const auto batch_elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                batch_stop - batch_start)
                .count());
        stats.total_latency_ns += batch_elapsed * queued.size();
        stats.peak_in_flight = std::max(stats.peak_in_flight, queued.size());

        base = limit;
    }

    const auto stop = std::chrono::steady_clock::now();
    stats.completed = reads.size();
    stats.elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
            .count());
    return stats;
}

} // namespace intentio::detail
