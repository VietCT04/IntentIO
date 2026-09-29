#include "intentio/file.hpp"

#include "detail/file_state.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <linux/stat.h>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef RWF_DONTCACHE
#define RWF_DONTCACHE 0x00000080U
#endif

namespace intentio::detail {

FileState::~FileState() {
    if (direct_fd >= 0) {
        ::close(direct_fd);
    }
    if (buffered_fd >= 0) {
        ::close(buffered_fd);
    }
}

} // namespace intentio::detail

namespace intentio {

namespace {

bool same_file(const struct stat& first, const struct stat& second) noexcept {
    return first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}

void close_on_error(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void fill_direct_capabilities(detail::FileState& state,
                              const struct stat& buffered_stat) noexcept {
    if (state.direct_fd >= 0) {
        struct stat direct_stat{};
        if (::fstat(state.direct_fd, &direct_stat) != 0 ||
            !same_file(buffered_stat, direct_stat)) {
            close_on_error(state.direct_fd);
            state.direct_open_errno = ESTALE;
        }
    }

    struct statx info{};
    constexpr unsigned mask =
#ifdef STATX_DIO_READ_ALIGN
        STATX_DIOALIGN | STATX_DIO_READ_ALIGN;
#else
        STATX_DIOALIGN;
#endif
    if (::statx(AT_FDCWD,
                state.path.c_str(),
                AT_STATX_SYNC_AS_STAT,
                mask,
                &info) != 0) {
        close_on_error(state.direct_fd);
        state.direct_open_errno = ENOTSUP;
        return;
    }

    if ((info.stx_mask & STATX_DIOALIGN) != 0U &&
        info.stx_dio_mem_align != 0U &&
        info.stx_dio_offset_align != 0U) {
        state.capabilities.memory_alignment = info.stx_dio_mem_align;
        state.capabilities.offset_alignment = info.stx_dio_offset_align;
#ifdef STATX_DIO_READ_ALIGN
        state.capabilities.read_offset_alignment =
            ((info.stx_mask & STATX_DIO_READ_ALIGN) != 0U &&
             info.stx_dio_read_offset_align != 0U)
                ? info.stx_dio_read_offset_align
                : info.stx_dio_offset_align;
#else
        state.capabilities.read_offset_alignment = info.stx_dio_offset_align;
#endif
    }

    if (state.direct_fd < 0 ||
        state.capabilities.memory_alignment == 0 ||
        state.capabilities.offset_alignment == 0 ||
        state.capabilities.read_offset_alignment == 0) {
        close_on_error(state.direct_fd);
        if (state.direct_open_errno == 0) {
            state.direct_open_errno = ENOTSUP;
        }
        return;
    }
    state.capabilities.direct_io_supported = true;
}

bool probe_dontcache(int fd) noexcept {
    std::byte buffer[4096]{};
    iovec vector{buffer, sizeof(buffer)};
    const ssize_t result = ::preadv2(fd, &vector, 1, 0, RWF_DONTCACHE);
    return result >= 0;
}

} // namespace

File File::open(const std::string& path) {
    const int buffered_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (buffered_fd < 0) {
        throw std::system_error(errno,
                                std::generic_category(),
                                "IntentIO::File::open");
    }

    struct stat buffered_stat{};
    if (::fstat(buffered_fd, &buffered_stat) != 0) {
        const int saved_errno = errno;
        ::close(buffered_fd);
        throw std::system_error(saved_errno,
                                std::generic_category(),
                                "IntentIO::File::fstat");
    }
    if (!S_ISREG(buffered_stat.st_mode)) {
        ::close(buffered_fd);
        throw std::system_error(EISDIR,
                                std::generic_category(),
                                "IntentIO::File requires a regular file");
    }

    auto state = std::make_shared<detail::FileState>();
    state->buffered_fd = buffered_fd;
    state->path = path;
    state->capabilities.dontcache_supported = probe_dontcache(buffered_fd);

#ifdef O_DIRECT
    state->direct_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECT);
    if (state->direct_fd < 0) {
        state->direct_open_errno = errno;
    }
#else
    state->direct_open_errno = ENOTSUP;
#endif

    fill_direct_capabilities(*state, buffered_stat);
    return File(std::move(state));
}

File::File(std::shared_ptr<detail::FileState> state)
    : state_(std::move(state)) {}

File::File(File&&) noexcept = default;

File& File::operator=(File&&) noexcept = default;

File::~File() = default;

bool File::valid() const noexcept {
    return state_ != nullptr && state_->buffered_fd >= 0;
}

FileCapabilities File::capabilities() const noexcept {
    return state_ != nullptr ? state_->capabilities : FileCapabilities{};
}

} // namespace intentio
