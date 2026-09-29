#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <linux/io_uring.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

struct NamedBit {
    const char* name;
    unsigned bit;
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
