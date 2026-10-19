#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

#include "engine/ee/cpu/cpu_state.h"

// Actual source-visible bytes, in addition to the complete lossless context
// images retained by MathLibraryOracleArtifacts. Never writes a model result
// in place of the installed control or selected arm.
class MathLibraryJointObservations {
public:
    // Rotate observes the twenty caller-visible vectors of RotateJoint.
    // Transform observes VF0..8: the constant, four result rows and the
    // parent rows loaded by CreateJointTransform's own scalar code.
    // Animate observes VF0..1: the constant and the published frame rotation;
    // the frame movement is consumed through its frame slot before the turn
    // program reuses VF2.
    enum class Scope { Rotate, Transform, Animate };
    MathLibraryJointObservations(const char* directory, size_t memory_size, Scope scope = Scope::Rotate);
    bool Equal(const CpuContext& actual, const CpuContext& control) const;
    void Input(bool selected, uint32_t dispatch, const CpuContext& context, const uint8_t* memory);
    void Output(bool selected, const CpuContext& context, const uint8_t* memory);
    void Finish();

private:
    static void Open(std::ofstream& stream, const std::filesystem::path& path);
    static void Bytes(std::ofstream& stream, const void* data, size_t length);
    static void Word(std::ofstream& stream, uint32_t value);
    bool enabled_ = false;
    Scope scope_;
    std::vector<unsigned> vectors_;
    size_t memory_size_;
    std::filesystem::path directory_;
    std::array<std::ofstream, 2> inputs_, outputs_;
    std::array<uint64_t, 2> inputs_count_{}, outputs_count_{};
};
