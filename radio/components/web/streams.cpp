#include "streams.h"

namespace web {

bool Streams::add(int fd) {
    remove(fd);
    if (full()) return false;
    fd_[n_] = fd;
    skips_[n_++] = 0;
    return true;
}

bool Streams::remove(int fd) {
    for (int i = 0; i < n_; i++) {
        if (fd_[i] != fd) continue;
        for (n_--; i < n_; i++) {
            fd_[i] = fd_[i + 1];
            skips_[i] = skips_[i + 1];
        }
        return true;
    }
    return false;
}

void Streams::push(Send (*send)(int fd, void* ctx), void (*close)(int fd, void* ctx), void* ctx) {
    for (int i = 0; i < n_;) {
        const int fd = fd_[i];
        const Send r = send(fd, ctx);
        if (r == Send::Sent) skips_[i] = 0;
        if (r == Send::Sent || (r == Send::Skipped && ++skips_[i] < kMaxSkips)) {
            i++;
            continue;
        }
        remove(fd);
        close(fd, ctx);
    }
}

}  // namespace web
