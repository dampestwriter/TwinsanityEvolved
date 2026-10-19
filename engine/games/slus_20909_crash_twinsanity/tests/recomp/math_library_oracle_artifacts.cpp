#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_oracle_artifacts.h"

#include <cfenv>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "games/slus_20909_crash_twinsanity/tests/recomp/sha256.h"

namespace {
// A write-only streambuf that folds every byte into one SHA-256 (digest mode).
class DigestStreambuf : public std::streambuf {
public:
    std::string Hex() { return sha_.Hex(); }

protected:
    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) {
            const uint8_t byte = static_cast<uint8_t>(ch);
            sha_.Update(&byte, 1);
        }
        return ch;
    }
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        sha_.Update(reinterpret_cast<const uint8_t*>(data), static_cast<size_t>(count));
        return count;
    }

private:
    ps2r_tests::Sha256 sha_;
};
}  // namespace

void MathLibraryOracleArtifacts::Sink::close() {
    if (stream) stream->flush();
    if (auto* file = dynamic_cast<std::filebuf*>(buffer.get())) file->close();
}

std::string MathLibraryOracleArtifacts::Sink::digest() const {
    auto* d = dynamic_cast<DigestStreambuf*>(buffer.get());
    return d ? d->Hex() : std::string();
}

void MathLibraryOracleArtifacts::Open(Sink& sink, const char* channel) {
    sink.channel = channel;
    if (digest_mode_) {
        sink.buffer = std::make_unique<DigestStreambuf>();
    } else {
        auto file = std::make_unique<std::filebuf>();
        const std::filesystem::path path = directory_ / channel;
        if (!file->open(path, std::ios::binary | std::ios::out))
            throw std::runtime_error("cannot create artifact " + path.string());
        sink.buffer = std::move(file);
    }
    sink.stream = std::make_unique<std::ostream>(sink.buffer.get());
    sink.stream->exceptions(std::ios::badbit | std::ios::failbit);
}

void MathLibraryOracleArtifacts::Word(std::ostream& stream, uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) stream.put(static_cast<char>(value >> (byte * 8)));
}

void MathLibraryOracleArtifacts::Long(std::ostream& stream, uint64_t value) {
    Word(stream, static_cast<uint32_t>(value));
    Word(stream, static_cast<uint32_t>(value >> 32));
}

void MathLibraryOracleArtifacts::JsonString(std::ostream& stream, const std::string& value) {
    constexpr char digits[] = "0123456789abcdef";
    stream.put('"');
    for (const unsigned char byte : value) {
        if (byte == '"' || byte == '\\') {
            stream.put('\\');
            stream.put(static_cast<char>(byte));
        } else if (byte < 32) {
            stream << "\\u00" << digits[byte >> 4] << digits[byte & 15];
        } else stream.put(static_cast<char>(byte));
    }
    stream.put('"');
}

void MathLibraryOracleArtifacts::WriteCounts(std::ostream& stream, const Counts& values) {
    stream.put('{');
    bool first = true;
    for (const auto& value : values) {
        if (!first) stream.put(',');
        first = false;
        JsonString(stream, value.first);
        stream << ':' << value.second;
    }
    stream.put('}');
}

MathLibraryOracleArtifacts::Counts MathLibraryOracleArtifacts::ParseCensus(const std::string& census) {
    Counts result;
    size_t start = 0;
    while (start < census.size()) {
        const auto equal = census.find('=', start);
        const auto comma = census.find(',', start);
        const auto end = comma == std::string::npos ? census.size() : comma;
        if (equal == std::string::npos || equal == start || equal >= end)
            throw std::runtime_error("invalid actual counter census");
        uint64_t count = 0;
        const auto parsed = std::from_chars(census.data() + equal + 1, census.data() + end, count);
        if (parsed.ec != std::errc{} || parsed.ptr != census.data() + end ||
            !result.emplace(census.substr(start, equal - start), count).second)
            throw std::runtime_error("invalid or duplicate actual counter value");
        start = end + 1;
    }
    if (result.empty()) throw std::runtime_error("empty actual counter census");
    return result;
}

void MathLibraryOracleArtifacts::Delta(std::ostream& stream, const std::vector<uint8_t>& image,
                                      const std::vector<uint8_t>& base) {
    // Coalesce gaps shorter than eight bytes: this bounds overhead on seeded
    // random inputs without changing the decoded image.
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    size_t cursor = 0;
    while (cursor < image.size()) {
        while (cursor < image.size() && image[cursor] == base[cursor]) ++cursor;
        if (cursor == image.size()) break;
        const size_t start = cursor++;
        size_t end = cursor;
        while (cursor < image.size() && cursor - end < 8) {
            if (image[cursor] != base[cursor]) end = cursor + 1;
            ++cursor;
        }
        ranges.emplace_back(static_cast<uint32_t>(start), static_cast<uint32_t>(end - start));
    }
    Word(stream, static_cast<uint32_t>(ranges.size()));
    for (const auto& range : ranges) {
        Word(stream, range.first);
        Word(stream, range.second);
        stream.write(reinterpret_cast<const char*>(image.data() + range.first), range.second);
    }
}

MathLibraryOracleArtifacts::MathLibraryOracleArtifacts(int argc, char* const* argv, const char* name,
    const char* source_digest, size_t memory_size, size_t scratch_offset, uint32_t accepted_control_entry)
    : memory_size_(memory_size) {
    if (argc == 1) return;
    if (argc != 3 || argv[2][0] == '\0' ||
        (std::strcmp(argv[1], "--artifacts") != 0 && std::strcmp(argv[1], "--digest") != 0))
        throw std::runtime_error("usage: oracle [--artifacts DIR | --digest FILE]");
    if (memory_size > std::numeric_limits<uint32_t>::max() - sizeof(CpuContext))
        throw std::runtime_error("artifact fixture too large");
    digest_mode_ = std::strcmp(argv[1], "--digest") == 0;
    directory_ = argv[2];
    if (digest_mode_) {
        // The digest file is written whole by Finish; refuse to clobber an existing one.
        if (std::filesystem::exists(directory_))
            throw std::runtime_error("digest file already exists; choose a fresh FILE");
        if (directory_.has_parent_path()) std::filesystem::create_directories(directory_.parent_path());
    } else if (!std::filesystem::create_directories(directory_)) {
        // Never overwrite a prior run or follow a preexisting output symlink.
        throw std::runtime_error("artifact directory already exists; choose a fresh DIR");
    }
    enabled_ = true;
    input_.resize(sizeof(CpuContext) + memory_size);
    previous_input_.resize(input_.size());
    output_.resize(input_.size());
    Open(inputs_, "inputs.bin");
    Open(model_, "model.outputs.bin");
    Open(selected_, "selected.outputs.bin");
    Open(control_, "control.outputs.bin");
    inputs_.out().write("MATHIN01", 8);
    Word(inputs_.out(), sizeof(CpuContext));
    Word(inputs_.out(), static_cast<uint32_t>(memory_size));
    for (auto* sink : {&model_, &selected_, &control_}) {
        sink->out().write("MATHOU01", 8);
        Word(sink->out(), sizeof(CpuContext));
        Word(sink->out(), static_cast<uint32_t>(memory_size));
    }
    Open(model_counts_.sink, "model.counters.json");
    Open(selected_counts_.sink, "selected.counters.json");
    Open(control_counts_.sink, "control.counters.json");
    for (auto* file : {&model_counts_, &selected_counts_, &control_counts_})
        file->sink.out() << "{\"schema\":1,\"records\":[\n";
    if (digest_mode_) return;   // metadata.json names the command path: host-specific, not a lane
    std::ofstream metadata;
    metadata.exceptions(std::ios::badbit | std::ios::failbit);
    metadata.open(directory_ / "metadata.json", std::ios::binary | std::ios::out);
    metadata << "{\"schema\":1,\"oracle\":";
    JsonString(metadata, name);
    metadata << ",\"source_sha256\":";
    JsonString(metadata, source_digest);
    metadata << ",\"command\":[";
    for (int i = 0; i < argc; ++i) {
        if (i) metadata.put(',');
        JsonString(metadata, argv[i]);
    }
    const uint16_t endian = 1;
    metadata << "],\"context_size\":" << sizeof(CpuContext)
        << ",\"memory_size\":" << memory_size << ",\"scratch_offset\":" << scratch_offset
        << ",\"context_byte_order\":\"" << (*reinterpret_cast<const uint8_t*>(&endian) ? "little" : "big")
        << "\",\"context_offsets\":{\"cpuRegs\":" << offsetof(CpuContext, cpuRegs)
        << ",\"fpuRegs\":" << offsetof(CpuContext, fpuRegs)
        << ",\"vuRegs\":" << offsetof(CpuContext, vuRegs)
        << "},\"accepted_control_entry\":" << accepted_control_entry
        << ",\"arms\":{\"model\":\"direct NEW generated reference\","
           "\"selected\":\"selection=true installed library\","
           "\"control\":\"selection=false installed library invoking captured originals, except accepted_control_entry\"}"
        << ",\"encoding\":\"math-byte-ranges-v1\","
           "\"counter_scope\":\"per-installation cumulative; totals sum call deltas\"}\n";
    metadata.close();
}

void MathLibraryOracleArtifacts::Image(std::vector<uint8_t>& destination,
    const CpuContext& context, const uint8_t* memory) {
    std::memcpy(destination.data(), &context, sizeof(context));
    std::memcpy(destination.data() + sizeof(context), memory, memory_size_);
}

void MathLibraryOracleArtifacts::Fixture(const char* name, const uint8_t* bytes, size_t length) {
    if (!enabled_) return;
    const std::filesystem::path relative(name);
    if (pending_ || records_ != 0 || relative.empty() || relative.has_parent_path() ||
        fixtures_.count(name) || (!digest_mode_ && std::filesystem::exists(directory_ / relative)))
        throw std::runtime_error("invalid, late or duplicate artifact fixture");
    if (digest_mode_) {
        ps2r_tests::Sha256 sha;
        sha.Update(bytes, length);
        fixture_digests_.emplace_back(std::string("fixture/") + name, sha.Hex());
    } else {
        std::ofstream fixture;
        fixture.exceptions(std::ios::badbit | std::ios::failbit);
        fixture.open(directory_ / relative, std::ios::binary | std::ios::out);
        fixture.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(length));
        fixture.close();
    }
    fixtures_.emplace(name, length);
}

void MathLibraryOracleArtifacts::Begin(Case key, const CpuContext& context, const uint8_t* memory) {
    if (!enabled_) return;
    if (pending_) throw std::runtime_error("previous artifact call has no actual result");
    key_ = key;
    if (key.step == 0) {
        ++cases_;
        current_model_counts_.clear();
    } else if (cases_ == 0) throw std::runtime_error("artifact sequence without initial call");
    pending_ = true;
    model_ran_ = model_written_ = actual_checked_ = false;
    rounding_ = std::fegetround();
    Image(input_, context, memory);
    Long(inputs_.out(), records_);
    Long(inputs_.out(), cases_ - 1);
    for (const uint32_t value : {key.entry, key.dispatch, key.operation, key.trial, key.step,
         static_cast<uint32_t>(key.selected), static_cast<uint32_t>(rounding_)}) Word(inputs_.out(), value);
    Delta(inputs_.out(), input_, previous_input_);
    previous_input_ = input_;
}

void MathLibraryOracleArtifacts::RunModel(const std::function<void()>& body) {
    if (enabled_) {
        if (!pending_ || model_ran_) throw std::runtime_error("model invocation without a fresh input");
        char name[40];
        std::snprintf(name, sizeof(name), "generated_body_%08x", key_.entry);
        ++current_model_counts_[name];
        model_ran_ = true;
    }
    body();
}

void MathLibraryOracleArtifacts::WriteCounters(CounterFile& file, const Counts& values) {
    if (key_.step == 0) file.previous.clear();
    for (const auto& item : values) {
        const auto before = file.previous[item.first];
        if (item.second < before) throw std::runtime_error("counter decreased inside one installation");
        file.totals[item.first] += item.second - before;
    }
    file.previous = values;
    if (file.records++) file.sink.out() << ",\n";
    file.sink.out() << "{\"record\":" << records_ << ",\"values\":";
    WriteCounts(file.sink.out(), values);
    file.sink.out().put('}');
}

void MathLibraryOracleArtifacts::WriteOutput(Sink& sink, CounterFile& counters,
    const CpuContext& context, const uint8_t* memory, const Counts& values) {
    Image(output_, context, memory);
    Long(sink.out(), records_);
    Word(sink.out(), static_cast<uint32_t>(std::fegetround()));
    Delta(sink.out(), output_, input_);
    WriteCounters(counters, values);
}

void MathLibraryOracleArtifacts::ModelOutput(const CpuContext& context, const uint8_t* memory) {
    if (!enabled_) return;
    if (!pending_ || !model_ran_ || model_written_) throw std::runtime_error("unexpected model result");
    WriteOutput(model_, model_counts_, context, memory, current_model_counts_);
    model_written_ = true;
}

void MathLibraryOracleArtifacts::CheckActualInput(const CpuContext& context, const uint8_t* memory) {
    if (!enabled_) return;
    Image(output_, context, memory);
    if (!model_written_ || input_ != output_ || rounding_ != std::fegetround())
        throw std::runtime_error("model and actual arms did not consume the same input bytes/rounding");
    actual_checked_ = true;
}

void MathLibraryOracleArtifacts::ActualOutput(const CpuContext& context, const uint8_t* memory,
                                             const std::string& census) {
    if (!enabled_) return;
    if (!pending_ || !actual_checked_) throw std::runtime_error("actual result without verified input");
    WriteOutput(key_.selected ? selected_ : control_, key_.selected ? selected_counts_ : control_counts_,
        context, memory, ParseCensus(census));
    pending_ = false;
    ++records_;
}

void MathLibraryOracleArtifacts::CloseCounters(CounterFile& file) {
    file.sink.out() << "\n],\"count\":" << file.records << ",\"totals\":";
    WriteCounts(file.sink.out(), file.totals);
    file.sink.out() << "}\n";
    file.sink.close();
}

void MathLibraryOracleArtifacts::Finish() {
    if (!enabled_) return;
    if (pending_ || records_ == 0) throw std::runtime_error("incomplete artifact stream");
    inputs_.close();
    model_.close();
    selected_.close();
    control_.close();
    for (auto* file : {&model_counts_, &selected_counts_, &control_counts_}) CloseCounters(*file);
    std::ostringstream complete_text;
    complete_text << "{\"schema\":1,\"records\":" << records_ << ",\"cases\":" << cases_
        << ",\"model_records\":" << model_counts_.records
        << ",\"selected_records\":" << selected_counts_.records
        << ",\"control_records\":" << control_counts_.records << ",\"fixtures\":";
    WriteCounts(complete_text, fixtures_);
    complete_text << ",\"oracle_passed\":true}\n";
    if (digest_mode_) {
        // The lane: `<channel>,<sha256>` per channel, the fixtures, and complete.json's own text
        // digest (records/cases counts). Written whole, last, only by a passing oracle.
        std::ofstream digest;
        digest.exceptions(std::ios::badbit | std::ios::failbit);
        digest.open(directory_, std::ios::binary | std::ios::out);
        digest << "# math oracle identity lane: channel,sha256 (the --artifacts byte streams, digested)\n";
        for (const Sink* sink : {&inputs_, &model_, &selected_, &control_,
                                 &model_counts_.sink, &selected_counts_.sink, &control_counts_.sink})
            digest << sink->channel << "," << sink->digest() << "\n";
        for (const auto& item : fixture_digests_) digest << item.first << "," << item.second << "\n";
        ps2r_tests::Sha256 sha;
        const std::string text = complete_text.str();
        sha.Update(reinterpret_cast<const uint8_t*>(text.data()), text.size());
        digest << "complete.json," << sha.Hex() << "\n";
        digest.close();
    } else {
        std::ofstream complete;
        complete.exceptions(std::ios::badbit | std::ios::failbit);
        complete.open(directory_ / "complete.json", std::ios::binary | std::ios::out);
        complete << complete_text.str();
        complete.close();
    }
    enabled_ = false;
}
