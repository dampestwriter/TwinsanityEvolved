#include "games/slus_20909_crash_twinsanity/tests/recomp/math_library_joint_observations.h"

#include <cfenv>
#include <cstring>
#include <stdexcept>

void MathLibraryJointObservations::Open(std::ofstream& stream, const std::filesystem::path& path) {
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.open(path, std::ios::binary | std::ios::out);
}

void MathLibraryJointObservations::Bytes(std::ofstream& stream, const void* data, size_t length) {
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(length));
}

void MathLibraryJointObservations::Word(std::ofstream& stream, uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) stream.put(static_cast<char>(value >> (byte * 8)));
}

MathLibraryJointObservations::MathLibraryJointObservations(
        const char* directory, size_t memory_size, Scope scope)
    : scope_(scope), memory_size_(memory_size) {
    if (scope == Scope::Rotate)
        vectors_ = {0, 1, 2, 3, 4, 5, 6, 7, 8, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30, 31};
    else if (scope == Scope::Transform)
        vectors_ = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    else
        vectors_ = {0, 1};
    if (!directory) return;
    // The full artifact writer has already created a fresh directory.
    directory_ = std::filesystem::path(directory) / "source";
    if (!std::filesystem::create_directory(directory_))
        throw std::runtime_error("joint source observation directory already exists");
    for (unsigned arm = 0; arm < 2; ++arm) {
        const std::string prefix = arm ? "selected" : "control";
        Open(inputs_[arm], directory_ / (prefix + ".inputs.raw"));
        Open(outputs_[arm], directory_ / (prefix + ".outputs.raw"));
    }
    enabled_ = true;
}

bool MathLibraryJointObservations::Equal(const CpuContext& actual, const CpuContext& control) const {
    if (std::memcmp(&actual.cpuRegs, &control.cpuRegs, sizeof(actual.cpuRegs)) ||
        std::memcmp(&actual.fpuRegs, &control.fpuRegs, sizeof(actual.fpuRegs))) return false;
    for (unsigned reg : vectors_)
        if (std::memcmp(&actual.vuRegs.VF[reg], &control.vuRegs.VF[reg], 16)) return false;
    return true;
}

void MathLibraryJointObservations::Input(
        bool selected, uint32_t dispatch, const CpuContext& context, const uint8_t* memory) {
    if (!enabled_) return;
    auto& stream = inputs_[selected];
    Word(stream, dispatch);
    Word(stream, static_cast<uint32_t>(std::fegetround()));
    Bytes(stream, &context, sizeof(context));
    Bytes(stream, memory, memory_size_);
    ++inputs_count_[selected];
}

void MathLibraryJointObservations::Output(
        bool selected, const CpuContext& context, const uint8_t* memory) {
    if (!enabled_) return;
    if (inputs_count_[selected] != outputs_count_[selected] + 1)
        throw std::runtime_error("joint source observation pairing mismatch");
    auto& stream = outputs_[selected];
    Bytes(stream, &context.cpuRegs, sizeof(context.cpuRegs));
    Bytes(stream, &context.fpuRegs, sizeof(context.fpuRegs));
    for (unsigned reg : vectors_) Bytes(stream, &context.vuRegs.VF[reg], 16);
    Bytes(stream, memory, memory_size_);
    ++outputs_count_[selected];
}

void MathLibraryJointObservations::Finish() {
    if (!enabled_) return;
    if (inputs_count_ != outputs_count_ || !outputs_count_[0] || outputs_count_[0] != outputs_count_[1])
        throw std::runtime_error("incomplete joint source observations");
    for (unsigned arm = 0; arm < 2; ++arm) {
        inputs_[arm].close();
        outputs_[arm].close();
    }
    std::ofstream metadata;
    Open(metadata, directory_ / "complete.json");
    const bool rotate = scope_ == Scope::Rotate, animate = scope_ == Scope::Animate;
    metadata << "{\"schema\":1,\"operation\":\"" <<
        (rotate ? "RotateJoint" : animate ? "AnimateJoint" : "CreateJointTransform") <<
        "\",\"entry\":" << (rotate ? 0x297b40u : animate ? 0x293ec0u : 0x294300u) << ","
        "\"pairing\":\"same trial order within each caller rounding mode\","
        "\"records_per_arm\":" << outputs_count_[0] <<
        ",\"input_record_bytes\":" << 8 + sizeof(CpuContext) + memory_size_ <<
        ",\"input_layout\":[\"dispatch_le32\",\"rounding_le32\",\"raw_context\",\"raw_memory\"],"
        "\"output_record_bytes\":" << sizeof(CpuContext::cpuRegs) + sizeof(CpuContext::fpuRegs) +
            vectors_.size() * 16 + memory_size_ <<
        ",\"output_layout\":[\"raw_cpuRegs\",\"raw_fpuRegs\",\"listed_raw_VF\",\"raw_memory\"],"
        "\"cpu_bytes\":" << sizeof(CpuContext::cpuRegs) <<
        ",\"fpu_bytes\":" << sizeof(CpuContext::fpuRegs) <<
        ",\"memory_bytes\":" << memory_size_ << ",\"vectors\":[";
    for (size_t i = 0; i < vectors_.size(); ++i) {
        if (i) metadata.put(',');
        metadata << vectors_[i];
    }
    metadata << "],\"complete_context_outputs\":\"../{selected,control}.outputs.bin\","
        "\"actual_library_counters\":\"../{selected,control}.counters.json\","
        "\"command_metadata\":\"../metadata.json\","
        "\"excluded_state\":\"" << (rotate ?
        "Discarded interpolation VF9..19/VF29, integer and control temporaries, ACC and arithmetic flags." :
        animate ?
        "Discarded frame-blend/turn/interpolation program operands and scratch VF2..31 (the frame "
        "movement VF2 is consumed from the frame slot before the turn program overwrites it; inputs "
        "VF28..31 are overwritten by the programs), integer and control temporaries, ACC, Q and "
        "arithmetic flags." :
        "Discarded basis/composition VF9..14 and VF29..31 operands, integer and control temporaries, "
        "ACC, Q and arithmetic flags.") <<
        " All raw bytes remain in the full outputs; "
        "no full-context equivalence or whole-model retirement is claimed.\"}\n";
    metadata.close();
}
