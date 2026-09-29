#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <linux/io_uring.h>
#include <linux/stat.h>
#include <sstream>
#include <string>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef RWF_DONTCACHE
#define RWF_DONTCACHE 0x00000080U
#endif

namespace {

struct NamedBit {
    const char* name;
    unsigned bit;
};

struct ProbeFile {
    int fd{-1};
    std::string path;

    ProbeFile() = default;

    ~ProbeFile() {
        if (fd >= 0) {
            ::close(fd);
        }
        if (!path.empty()) {
            ::unlink(path.c_str());
        }
    }

    ProbeFile(const ProbeFile&) = delete;
    ProbeFile& operator=(const ProbeFile&) = delete;

    ProbeFile(ProbeFile&& other) noexcept
        : fd(std::exchange(other.fd, -1)),
          path(std::move(other.path)) {}

    ProbeFile& operator=(ProbeFile&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) {
                ::close(fd);
            }
            if (!path.empty()) {
                ::unlink(path.c_str());
            }
            fd = std::exchange(other.fd, -1);
            path = std::move(other.path);
        }
        return *this;
    }
};

const std::vector<NamedBit>& feature_bits() {
    static const std::vector<NamedBit> bits{
        {"SINGLE_MMAP", IORING_FEAT_SINGLE_MMAP},
        {"NODROP", IORING_FEAT_NODROP},
        {"SUBMIT_STABLE", IORING_FEAT_SUBMIT_STABLE},
        {"RW_CUR_POS", IORING_FEAT_RW_CUR_POS},
        {"CUR_PERSONALITY", IORING_FEAT_CUR_PERSONALITY},
        {"FAST_POLL", IORING_FEAT_FAST_POLL},
        {"POLL_32BITS", IORING_FEAT_POLL_32BITS},
        {"SQPOLL_NONFIXED", IORING_FEAT_SQPOLL_NONFIXED},
        {"EXT_ARG", IORING_FEAT_EXT_ARG},
        {"NATIVE_WORKERS", IORING_FEAT_NATIVE_WORKERS},
        {"RSRC_TAGS", IORING_FEAT_RSRC_TAGS},
        {"CQE_SKIP", IORING_FEAT_CQE_SKIP},
        {"LINKED_FILE", IORING_FEAT_LINKED_FILE},
        {"REG_REG_RING", IORING_FEAT_REG_REG_RING},
        {"RECVSEND_BUNDLE", IORING_FEAT_RECVSEND_BUNDLE},
        {"MIN_TIMEOUT", IORING_FEAT_MIN_TIMEOUT},
        {"RW_ATTR", IORING_FEAT_RW_ATTR},
        {"NO_IOWAIT", IORING_FEAT_NO_IOWAIT},
    };
    return bits;
}

const std::vector<NamedBit>& setup_flags() {
    static const std::vector<NamedBit> flags{
        {"IOPOLL", IORING_SETUP_IOPOLL},
        {"SQPOLL", IORING_SETUP_SQPOLL},
        {"SQ_AFF", IORING_SETUP_SQ_AFF},
        {"CQSIZE", IORING_SETUP_CQSIZE},
        {"CLAMP", IORING_SETUP_CLAMP},
        {"ATTACH_WQ", IORING_SETUP_ATTACH_WQ},
        {"R_DISABLED", IORING_SETUP_R_DISABLED},
        {"SUBMIT_ALL", IORING_SETUP_SUBMIT_ALL},
        {"COOP_TASKRUN", IORING_SETUP_COOP_TASKRUN},
        {"TASKRUN_FLAG", IORING_SETUP_TASKRUN_FLAG},
        {"SQE128", IORING_SETUP_SQE128},
        {"CQE32", IORING_SETUP_CQE32},
        {"SINGLE_ISSUER", IORING_SETUP_SINGLE_ISSUER},
        {"DEFER_TASKRUN", IORING_SETUP_DEFER_TASKRUN},
        {"NO_MMAP", IORING_SETUP_NO_MMAP},
        {"REGISTERED_FD_ONLY", IORING_SETUP_REGISTERED_FD_ONLY},
        {"NO_SQARRAY", IORING_SETUP_NO_SQARRAY},
        {"HYBRID_IOPOLL", IORING_SETUP_HYBRID_IOPOLL},
        {"CQE_MIXED", IORING_SETUP_CQE_MIXED},
        {"SQE_MIXED", IORING_SETUP_SQE_MIXED},
        {"SQ_REWIND", IORING_SETUP_SQ_REWIND},
    };
    return flags;
}

int setup_ring(unsigned entries, io_uring_params& params) {
    return static_cast<int>(::syscall(__NR_io_uring_setup, entries, &params));
}

std::string kernel_version() {
    utsname info{};
    if (::uname(&info) != 0) {
        return std::string("unavailable: ") + std::strerror(errno);
    }
    return std::string(info.release);
}

std::string filesystem_type(const std::string& path) {
    struct statfs info{};
    if (::statfs(path.c_str(), &info) != 0) {
        return std::string("unavailable: ") + std::strerror(errno);
    }
    std::ostringstream output;
    output << "type=0x" << std::hex
           << static_cast<unsigned long long>(info.f_type) << std::dec;
    return output.str();
}

std::string storage_device(const std::string& path) {
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) {
        return std::string("unavailable: ") + std::strerror(errno);
    }
    std::ostringstream output;
    const unsigned device_major = major(info.st_dev);
    const unsigned device_minor = minor(info.st_dev);
    output << "stat_dev=" << device_major << ':' << device_minor;
    if (device_major == 0U) {
        output << " (not a directly identified block device)";
    } else {
        const std::filesystem::path sysfs_path =
            std::filesystem::path("/sys/dev/block") /
            (std::to_string(device_major) + ':' + std::to_string(device_minor));
        std::error_code error;
        const std::filesystem::path target =
            std::filesystem::read_symlink(sysfs_path, error);
        if (!error) {
            output << " sysfs=" << target.string();
        }
    }
    return output.str();
}

std::string memory_total() {
    std::ifstream input("/proc/meminfo");
    std::string key;
    std::string value;
    std::string unit;
    while (input >> key >> value >> unit) {
        if (key == "MemTotal:") {
            return value + ' ' + unit;
        }
    }
    return "unavailable";
}

ProbeFile make_probe_file(const std::string& input_path) {
    std::filesystem::path input(input_path);
    std::error_code error;
    std::filesystem::path directory = input;
    if (!std::filesystem::is_directory(input, error)) {
        directory = input.parent_path();
    }
    if (directory.empty()) {
        directory = ".";
    }

    std::string pattern = (directory / ".intentio-phase9-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const int fd = ::mkstemp(writable.data());
    if (fd < 0) {
        return {};
    }
    ProbeFile result;
    result.fd = fd;
    result.path = writable.data();
    std::vector<char> contents(4096, 'I');
    if (::ftruncate(fd, static_cast<off_t>(contents.size())) != 0 ||
        ::pwrite(fd,
                 contents.data(),
                 contents.size(),
                 0) != static_cast<ssize_t>(contents.size())) {
        return result;
    }
    return result;
}

std::string probe_dontcache(int fd) {
    std::vector<char> buffer(4096);
    iovec vector{
        buffer.data(),
        buffer.size(),
    };
    const ssize_t result = ::preadv2(fd, &vector, 1, 0, RWF_DONTCACHE);
    if (result >= 0) {
        return "usable=yes bytes=" + std::to_string(result);
    }
    const int saved_errno = errno;
    if (saved_errno == EOPNOTSUPP || saved_errno == ENOTSUP) {
        return "usable=no status=unsupported errno=" +
               std::to_string(saved_errno);
    }
    return "usable=no status=error errno=" + std::to_string(saved_errno) +
           " (" + std::strerror(saved_errno) + ")";
}

std::string probe_dioalign(const std::string& path) {
    struct statx info{};
    if (::statx(AT_FDCWD,
                path.c_str(),
                AT_STATX_SYNC_AS_STAT,
                STATX_DIOALIGN,
                &info) != 0) {
        return std::string("unavailable errno=") + std::to_string(errno) +
               " (" + std::strerror(errno) + ")";
    }
    if ((info.stx_mask & STATX_DIOALIGN) == 0U) {
        return "unavailable mask_missing";
    }
    return "supported memory_alignment=" +
           std::to_string(info.stx_dio_mem_align) +
           " offset_alignment=" + std::to_string(info.stx_dio_offset_align);
}

void print_bits(const char* label,
                const std::vector<NamedBit>& bits,
                unsigned value) {
    std::cout << label << ':';
    bool found = false;
    for (const NamedBit& bit : bits) {
        if ((value & bit.bit) != 0U) {
            std::cout << ' ' << bit.name;
            found = true;
        }
    }
    if (!found) {
        std::cout << " none";
    }
    std::cout << '\n';
}

void print_setup_flag_names() {
    std::cout << "compiled setup flags:";
    for (const NamedBit& flag : setup_flags()) {
        std::cout << ' ' << flag.name;
    }
    std::cout << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : ".";
    std::cout << "kernel=" << kernel_version() << '\n';
    std::cout << "filesystem=" << filesystem_type(path) << '\n';
    std::cout << "storage_device=" << storage_device(path) << '\n';
    std::cout << "cpu_count=" << ::sysconf(_SC_NPROCESSORS_ONLN) << '\n';
    std::cout << "memory_total=" << memory_total() << '\n';
    print_setup_flag_names();

    ProbeFile probe_file = make_probe_file(path);
    if (probe_file.fd >= 0) {
        std::cout << "rwf_dontcache=" << probe_dontcache(probe_file.fd) << '\n';
        std::cout << "statx_dioalign=" << probe_dioalign(probe_file.path) << '\n';
    } else {
        std::cout << "rwf_dontcache=unavailable (could not create probe file)\n";
        std::cout << "statx_dioalign=unavailable (could not create probe file)\n";
    }

    constexpr unsigned requested_entries = 8U;
    io_uring_params params{};
    const int ring_fd = setup_ring(requested_entries, params);
    if (ring_fd < 0) {
        const int saved_errno = errno;
        std::cout << "io_uring_available=no\n";
        std::cout << "io_uring_setup_errno=" << saved_errno << " ("
                  << std::strerror(saved_errno) << ")\n";
        std::cout << "setup_features=unavailable\n";
        std::cout << "supported_setup_flags=unavailable\n";
        std::cout << "ring_entries=unavailable\n";
        return 0;
    }

    std::cout << "io_uring_available=yes\n";
    print_bits("setup_features", feature_bits(), params.features);
    std::cout << "supported_setup_flags=baseline setup accepted; individual "
                 "flag probes are not used for dependent flags\n";
    std::cout << "sq_entries=" << params.sq_entries << '\n';
    std::cout << "cq_entries=" << params.cq_entries << '\n';
    std::cout << "ring_entries=" << params.sq_entries << '/' << params.cq_entries
              << '\n';
    ::close(ring_fd);
    return 0;
}
