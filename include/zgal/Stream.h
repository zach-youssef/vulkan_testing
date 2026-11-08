#pragma once

#include "core/Contract.h"
#include "core/Result.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

/*
 Streams for ZGAL's stream-buffer (RenderGraph::createStreamBuffer): bytes a program reads over
 frames, e.g. a scene file, instead of loading them whole at build.

    zgal::StreamOpener open = zgal::fileStream;   // or one wrapping it, e.g. checking the header
    TRY_ASSIGN(auto opened, zgal::openStream<SceneHeader>(open, path));
    const BufferId scene = graph.createStreamBuffer("scene", std::move(opened.stream),
                                                    opened.header.bytes / 4, 1 << 20);

 A Stream, like the objects it wraps, isn't thread safe: its user (one graph) synchronizes access.
*/

namespace zgal {

// A source of bytes with no known length. read() fills up to bytes.size() bytes, returning how
// many it filled: 0 once the stream has ended, otherwise at least 1. It may block until bytes
// arrive. A Stream is a RenderGraph::StreamSource. Copies of the streams this module makes share
// one position in the stream (a read from either moves both on).
class Stream {
public:
    using Read = std::function<Result<size_t>(std::span<std::byte>)>;

    explicit Stream(Read read) : read_(std::move(read)) {
        if (!read_) {
            contractViolation("A stream needs a read function");
        }
    }

    Result<size_t> read(std::span<std::byte> bytes) { return read_(bytes); }
    Result<size_t> operator()(std::span<std::byte> bytes) { return read_(bytes); }

private:
    Read read_;
};

// What a program supplies for a stream input: opens the stream at a path
using StreamOpener = std::function<Result<Stream>(const std::filesystem::path&)>;

// A file's bytes, read as asked. Fails with ErrorCode::NotFound if the file can't be opened, and
// its reads with ErrorCode::InvalidData if reading it fails.
inline Result<Stream> fileStream(const std::filesystem::path& path) {
    auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
    if (!*file) {
        return Error(ErrorCode::NotFound, "Can't open '" + path.string() + "'");
    }
    return Stream([file, name = path.string()](std::span<std::byte> bytes) -> Result<size_t> {
        file->read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (file->bad()) {
            return Error(ErrorCode::InvalidData, "Failed to read '" + name + "'");
        }
        return static_cast<size_t>(file->gcount());
    });
}

// A stream opened with its header H (a std430 struct) read, and stream, which gives the header's
// bytes again and then the rest, so a streamed buffer holds the whole stream.
template<typename H>
struct OpenedStream {
    H header;
    Stream stream;
};

// Opens path with open and reads sizeof(H) bytes as an H. Failing to open fails here; a short
// header, or a stream failing or throwing (other than a contract violation) while it's read,
// leaves the header zeroed and makes the returned stream fail on its first read, so a graph
// streaming it fails its first frame, as for any other failure of the stream.
template<typename H>
Result<OpenedStream<H>> openStream(const StreamOpener& open, const std::filesystem::path& path) {
    static_assert(std::is_trivially_copyable_v<H>, "A stream's header is plain data");
    if (!open) {
        contractViolation("No opener for stream '" + path.string() + "'");
    }
    auto opened = open(path);
    if (!opened) {
        return std::move(opened).error().withContext("opening stream '" + path.string() + "'");
    }
    auto inner = std::make_shared<Stream>(std::move(*opened));

    // The header, read until whole, ended or failed
    auto header = std::make_shared<std::vector<std::byte>>(sizeof(H));
    std::optional<Error> failure;
    size_t filled = 0;
    while (filled < header->size() && !failure) {
        Result<size_t> read = size_t(0);
        try {
            read = inner->read(std::span<std::byte>(header->data() + filled, header->size() - filled));
        } catch (const ContractViolation&) {
            throw;
        } catch (const std::exception& exception) {
            read = Error(ErrorCode::InvalidData, std::string("the stream threw: ") + exception.what());
        } catch (...) {
            read = Error(ErrorCode::InvalidData, "the stream threw");
        }
        if (!read) {
            failure = std::move(read).error().withContext("reading the header of stream '" + path.string() + "'");
        } else if (*read == 0) {
            failure = Error(ErrorCode::InvalidData, "stream '" + path.string() + "' ended after " + std::to_string(filled)
                                                    + " bytes of its " + std::to_string(sizeof(H)) + "-byte header");
        } else if (*read > header->size() - filled) {
            failure = Error(ErrorCode::InvalidData, "stream '" + path.string() + "' read more bytes than asked");
        } else {
            filled += *read;
        }
    }

    OpenedStream<H> result{H{}, Stream([](std::span<std::byte>) -> Result<size_t> { return size_t(0); })};
    if (failure) {
        result.stream = Stream([failure = *failure](std::span<std::byte>) -> Result<size_t> { return failure; });
        return result;
    }
    std::memcpy(&result.header, header->data(), sizeof(H));
    // The header's bytes first, then the rest of the stream
    // (shared, as the stream is, by copies of the Stream)
    auto served = std::make_shared<size_t>(0);
    result.stream = Stream([inner, header, served](std::span<std::byte> bytes) -> Result<size_t> {
        if (*served < header->size()) {
            const size_t count = std::min(bytes.size(), header->size() - *served);
            std::memcpy(bytes.data(), header->data() + *served, count);
            *served += count;
            return count;
        }
        return inner->read(bytes);
    });
    return result;
}

} // namespace zgal
