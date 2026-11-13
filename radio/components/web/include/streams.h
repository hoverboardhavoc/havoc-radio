// The open event streams (GET /api/events): at most three sockets, oldest first. Pure: compiled into the firmware and
// the host tests; web.cpp keeps the one table and touches it only in the web server task.
#pragma once

namespace web {

class Streams {
public:
    static constexpr int kMax = 3;
    // A stream that cannot take this many pushes in a row (2 s at two a second) is closed.
    static constexpr int kMaxSkips = 4;

    // What a send to one stream did: sent; skipped because the stream cannot take an event now; or failed.
    enum class Send { Sent, Skipped, Failed };

    // Registers fd as the newest stream; false with every slot taken, when the caller refuses the stream.
    bool add(int fd);
    bool full() const { return n_ == kMax; }
    // Forgets fd; false when it is not a stream. The server's close callback calls it for every socket it closes.
    bool remove(int fd);
    // Calls send for each stream, oldest first. A stream whose send fails, or that is skipped kMaxSkips pushes in a
    // row, is forgotten before close is called for it, so no later push reaches it. A sent event restarts the count.
    void push(Send (*send)(int fd, void* ctx), void (*close)(int fd, void* ctx), void* ctx);
    int count() const { return n_; }
    int fd(int i) const { return fd_[i]; }
    int skips(int i) const { return skips_[i]; }

private:
    int fd_[kMax] = {};
    int skips_[kMax] = {};
    int n_ = 0;
};

}  // namespace web
