#include "zgal/Stream.h"
#include "common/ResultChecks.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

struct Header {
    uint32_t magic;
    uint32_t count;
};

std::vector<std::byte> bytesOf(std::initializer_list<int> values) {
    std::vector<std::byte> bytes;
    for (int value : values) {
        bytes.push_back(std::byte(value));
    }
    return bytes;
}

// A stream of bytes given in chunks of at most chunk bytes
zgal::Stream memoryStream(std::vector<std::byte> bytes, size_t chunk) {
    return zgal::Stream([bytes = std::move(bytes), chunk, offset = size_t(0)](std::span<std::byte> out) mutable -> Result<size_t> {
        const size_t count = std::min({out.size(), chunk, bytes.size() - offset});
        std::memcpy(out.data(), bytes.data() + offset, count);
        offset += count;
        return count;
    });
}

// Reads a stream to its end in reads of at most size bytes
Result<std::vector<std::byte>> readAll(zgal::Stream& stream, size_t size) {
    std::vector<std::byte> all;
    std::vector<std::byte> buffer(size);
    while (true) {
        TRY_ASSIGN(size_t read, stream.read(buffer));
        if (read == 0) {
            return all;
        }
        all.insert(all.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read));
    }
}

} // namespace

TEST_CASE("fileStream reads a file's bytes as asked, then ends") {
    const auto path = std::filesystem::temp_directory_path() / "zgal_stream_test.bin";
    const auto expected = bytesOf({1, 2, 3, 4, 5, 6, 7});
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(expected.data()), static_cast<std::streamsize>(expected.size()));
    }
    auto stream = zgal::fileStream(path).value();
    CHECK(readAll(stream, 3).value() == expected);
    std::vector<std::byte> more(4);
    CHECK(stream.read(more).value() == 0);
    std::filesystem::remove(path);

    CHECK_ERROR(zgal::fileStream(path), ErrorCode::NotFound, "zgal_stream_test.bin");
}

TEST_CASE("openStream reads the header, and its stream gives the header's bytes again then the rest") {
    const auto bytes = bytesOf({0x10, 0, 0, 0, 3, 0, 0, 0, 9, 8, 7});
    zgal::StreamOpener open = [&](const std::filesystem::path& path) -> Result<zgal::Stream> {
        CHECK(path == "scene.stream");
        return memoryStream(bytes, 3);   // The header arrives over several reads
    };
    auto opened = zgal::openStream<Header>(open, "scene.stream").value();
    CHECK(opened.header.magic == 0x10);
    CHECK(opened.header.count == 3);
    // A copy shares the position: the header isn't replayed twice
    zgal::Stream copy = opened.stream;
    std::vector<std::byte> start(2);
    CHECK(copy.read(start).value() == 2);
    auto rest = readAll(opened.stream, 5).value();
    rest.insert(rest.begin(), start.begin(), start.end());
    CHECK(rest == bytes);
}

TEST_CASE("openStream fails when the stream can't be opened, and defers a stream's failures to its first read") {
    zgal::StreamOpener missing = [](const std::filesystem::path&) -> Result<zgal::Stream> {
        return Error(ErrorCode::NotFound, "no such file");
    };
    CHECK_ERROR(zgal::openStream<Header>(missing, "a.stream"), ErrorCode::NotFound, "opening stream 'a.stream': no such file");

    std::vector<std::byte> into(16);
    SUBCASE("a short header") {
        zgal::StreamOpener shortHeader = [](const std::filesystem::path&) -> Result<zgal::Stream> {
            return memoryStream(bytesOf({1, 2, 3, 4, 5}), 2);
        };
        auto opened = zgal::openStream<Header>(shortHeader, "a.stream").value();
        CHECK(opened.header.magic == 0);
        CHECK(opened.header.count == 0);
        CHECK_ERROR(opened.stream.read(into), ErrorCode::InvalidData, "ended after 5 bytes of its 8-byte header");
    }
    SUBCASE("a failing stream") {
        zgal::StreamOpener failing = [](const std::filesystem::path&) -> Result<zgal::Stream> {
            return zgal::Stream([](std::span<std::byte>) -> Result<size_t> { return Error(ErrorCode::NotFound, "gone"); });
        };
        auto opened = zgal::openStream<Header>(failing, "a.stream").value();
        CHECK_ERROR(opened.stream.read(into), ErrorCode::NotFound, "reading the header of stream 'a.stream': gone");
    }
    SUBCASE("a throwing stream") {
        zgal::StreamOpener throwing = [](const std::filesystem::path&) -> Result<zgal::Stream> {
            return zgal::Stream([](std::span<std::byte>) -> Result<size_t> { throw std::runtime_error("broken pipe"); });
        };
        auto opened = zgal::openStream<Header>(throwing, "a.stream").value();
        CHECK_ERROR(opened.stream.read(into), ErrorCode::InvalidData, "the stream threw: broken pipe");
    }
}
