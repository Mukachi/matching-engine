// The sample day's messages, one at a time: Nasdaq's file (gzip, each message
// after a two-byte length) read through gzip, decoded with the feed handler's
// default decoder, fetched at its pinned commit. For the tests and benchmarks
// over the day (label `data`).
#pragma once

#include <itchfh/decoder.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace me {

class day_file {
public:
    explicit day_file(const std::string& path) : in_(::popen(("gzip -dc '" + path + "'").c_str(), "r")), buf_(1 << 16) {}
    ~day_file() {
        if (in_) ::pclose(in_);
    }
    day_file(const day_file&) = delete;
    day_file& operator=(const day_file&) = delete;

    bool open() const { return in_ != nullptr; }
    // The next message (its sequence number is its place in the day, from 1); false at the end.
    bool next(itchfh::message& m) {
        for (;;) {
            unsigned char lb[2];
            if (std::fread(lb, 1, 2, in_) != 2) return false;
            const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
            if (len == 0 || std::fread(buf_.data(), 1, len, in_) != len) return false;
            ++sequence_;
            if (itchfh::decode(buf_.data(), len, m) == itchfh::decode_status::ok) return true;
            ++undecoded_;
        }
    }
    std::uint64_t sequence() const { return sequence_; }
    std::uint64_t undecoded() const { return undecoded_; }

private:
    std::FILE* in_;
    std::vector<unsigned char> buf_;
    std::uint64_t sequence_ = 0, undecoded_ = 0;
};

}  // namespace me
