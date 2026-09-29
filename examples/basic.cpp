#include <intentio/intentio.hpp>

#include <fcntl.h>
#include <iostream>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: intentio_example FILE\n";
        return 2;
    }

    const int fd = ::open(argv[1], O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        std::perror("open");
        return 1;
    }

    intentio::Runtime io;
    auto group = io.create_group();
    group.read(fd, 0, 256 * 1024);
    group.read(fd, 256 * 1024, 256 * 1024);
    group.hint(intentio::AccessPattern::Sequential);

    const auto results = group.submit();
    for (const auto& result : results) {
        if (!result.ok()) {
            std::cerr << "request " << result.request_id << " failed: "
                      << result.error.message() << '\n';
            ::close(fd);
            return 1;
        }
        std::cout << "request " << result.request_id << ": "
                  << result.bytes_read() << " bytes\n";
    }

    ::close(fd);
    return 0;
}
