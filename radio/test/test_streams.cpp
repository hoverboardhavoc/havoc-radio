// The event stream table: registering, replacing the older stream, skipping, closing, and no sends to a closed
// stream.
#include <vector>

#include "streams.h"
#include "test.h"

namespace {

using Send = web::Streams::Send;

// A fake network: the fds whose sends fail and those that cannot take an event now, and every send and close in
// order.
struct Net {
    std::vector<int> failing;
    std::vector<int> busy;
    std::vector<int> sent;
    std::vector<int> closed;
    web::Streams* table = nullptr;  // when set, close also forgets the fd, as the server's close callback does
};

Send fakeSend(int fd, void* ctx) {
    auto* n = static_cast<Net*>(ctx);
    n->sent.push_back(fd);
    for (int f : n->failing)
        if (f == fd) return Send::Failed;
    for (int f : n->busy)
        if (f == fd) return Send::Skipped;
    return Send::Sent;
}

void fakeClose(int fd, void* ctx) {
    auto* n = static_cast<Net*>(ctx);
    n->closed.push_back(fd);
    if (n->table) CHECK(!n->table->remove(fd));
}

}  // namespace

TEST(streams_register_up_to_three_and_refuse_a_fourth) {
    web::Streams s;
    CHECK_EQ(s.count(), 0);
    CHECK(s.add(54));
    CHECK(s.add(55));
    CHECK(!s.full());
    CHECK(s.add(56));
    CHECK(s.full());
    CHECK(!s.add(57));  // no replacing: the open streams stay
    CHECK_EQ(s.count(), 3);
    CHECK_EQ(s.fd(0), 54);
    CHECK_EQ(s.fd(1), 55);
    CHECK_EQ(s.fd(2), 56);
}

TEST(streams_same_socket_twice_is_one_stream) {
    web::Streams s;
    s.add(54);
    s.add(55);
    CHECK(s.add(54));
    CHECK_EQ(s.count(), 2);
    CHECK_EQ(s.fd(0), 55);
    CHECK_EQ(s.fd(1), 54);
}

TEST(streams_close_frees_the_slot) {
    web::Streams s;
    s.add(54);
    s.add(55);
    s.add(56);
    CHECK(s.remove(54));
    CHECK(!s.remove(54));
    CHECK(!s.remove(60));  // a socket that never was a stream
    CHECK_EQ(s.count(), 2);
    CHECK_EQ(s.fd(0), 55);
    CHECK(s.add(57));  // the freed slot takes it
    CHECK_EQ(s.fd(2), 57);
}

TEST(streams_push_reaches_every_stream_oldest_first) {
    web::Streams s;
    s.add(54);
    s.add(55);
    Net n;
    s.add(56);
    s.push(fakeSend, fakeClose, &n);
    CHECK(n.sent == (std::vector<int>{54, 55, 56}));
    CHECK(n.closed.empty());
}

TEST(streams_failed_send_closes_once_and_is_never_sent_to_again) {
    for (int bad : {54, 55}) {
        web::Streams s;
        s.add(54);
        s.add(55);
        Net n;
        n.table = &s;
        n.failing = {bad};
        s.push(fakeSend, fakeClose, &n);
        CHECK(n.sent == (std::vector<int>{54, 55}));  // the other stream still gets this event
        CHECK(n.closed == (std::vector<int>{bad}));
        CHECK_EQ(s.count(), 1);
        CHECK(s.fd(0) != bad);
        n.sent.clear();
        s.push(fakeSend, fakeClose, &n);
        CHECK(n.sent == (std::vector<int>{bad == 54 ? 55 : 54}));
        CHECK(n.closed == (std::vector<int>{bad}));
    }
}

TEST(streams_all_sends_fail) {
    web::Streams s;
    s.add(54);
    s.add(55);
    Net n;
    n.failing = {54, 55};
    s.push(fakeSend, fakeClose, &n);
    CHECK(n.closed == (std::vector<int>{54, 55}));
    CHECK_EQ(s.count(), 0);
    n.sent.clear();
    s.push(fakeSend, fakeClose, &n);
    CHECK(n.sent.empty());
}

TEST(streams_not_writable_skips_and_closes_at_the_fourth_skip_in_a_row) {
    web::Streams s;
    s.add(54);
    s.add(55);
    Net n;
    n.table = &s;
    n.busy = {54};
    for (int k = 1; k < web::Streams::kMaxSkips; k++) {
        s.push(fakeSend, fakeClose, &n);
        CHECK(n.closed.empty());
        CHECK_EQ(s.count(), 2);
        CHECK_EQ(s.skips(0), k);
        CHECK_EQ(s.skips(1), 0);
    }
    s.push(fakeSend, fakeClose, &n);
    CHECK(n.closed == (std::vector<int>{54}));
    CHECK_EQ(s.count(), 1);
    CHECK_EQ(s.fd(0), 55);
    n.sent.clear();
    s.push(fakeSend, fakeClose, &n);
    CHECK(n.sent == (std::vector<int>{55}));
    CHECK(n.closed == (std::vector<int>{54}));
}

TEST(streams_a_sent_event_resets_the_skips) {
    web::Streams s;
    s.add(54);
    Net n;
    for (int round = 0; round < 3; round++) {
        n.busy = {54};
        for (int k = 1; k < web::Streams::kMaxSkips; k++) s.push(fakeSend, fakeClose, &n);
        CHECK_EQ(s.skips(0), web::Streams::kMaxSkips - 1);
        n.busy.clear();
        s.push(fakeSend, fakeClose, &n);
        CHECK_EQ(s.skips(0), 0);
    }
    CHECK(n.closed.empty());
    CHECK_EQ(s.count(), 1);
}

TEST(streams_skips_follow_their_stream) {
    web::Streams s;
    s.add(54);
    s.add(55);
    Net n;
    n.busy = {55};
    s.push(fakeSend, fakeClose, &n);
    s.push(fakeSend, fakeClose, &n);
    CHECK_EQ(s.skips(1), 2);
    CHECK(s.remove(54));  // 55 moves to the first slot with its count
    CHECK_EQ(s.fd(0), 55);
    CHECK_EQ(s.skips(0), 2);
    CHECK(s.add(56));
    CHECK_EQ(s.skips(1), 0);  // a new stream starts with none
    s.push(fakeSend, fakeClose, &n);
    s.push(fakeSend, fakeClose, &n);
    CHECK(n.closed == (std::vector<int>{55}));
    CHECK_EQ(s.count(), 1);
    CHECK_EQ(s.fd(0), 56);
}
