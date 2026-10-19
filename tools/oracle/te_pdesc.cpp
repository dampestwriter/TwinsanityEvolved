// tools/oracle/te_pdesc.cpp -- the perceptual frame descriptor of PPM files, through the SAME code the port's harness
// leg writes frames.pdesc with (te::harness::Describe), so tools/oracle/diff.py describes the recomp oracle's dumped
// frames (its --dump-frames f<key>.ppm) bit for bit like the port's own rows. pdesc.py is the slow pure-Python mirror.
//
//   te_pdesc FRAME.ppm [...]      one "<key>,<row>" line per file, key = the digits after the leading f / frame_
//   te_pdesc --stdin              paths one per line on stdin (the sidecar streams them), same output, flushed per line
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "te_harness.h"

namespace {

bool ReadPpm(const std::string& path, int* w, int* h, std::vector<uint8_t>* px, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { *err = "cannot open"; return false; }
    std::vector<char> data((std::istreambuf_iterator<char>(f)), {});
    size_t pos = 0;
    std::vector<std::string> tokens;
    while (tokens.size() < 4 && pos < data.size()) {
        while (pos < data.size() && std::isspace(static_cast<unsigned char>(data[pos]))) ++pos;
        if (pos < data.size() && data[pos] == '#') { while (pos < data.size() && data[pos] != '\n') ++pos; continue; }
        const size_t start = pos;
        while (pos < data.size() && !std::isspace(static_cast<unsigned char>(data[pos]))) ++pos;
        tokens.emplace_back(data.begin() + long(start), data.begin() + long(pos));
    }
    ++pos;
    if (tokens.size() < 4 || tokens[0] != "P6") { *err = "not a binary PPM"; return false; }
    *w = std::atoi(tokens[1].c_str());
    *h = std::atoi(tokens[2].c_str());
    if (*w <= 0 || *h <= 0) { *err = "bad size"; return false; }
    const size_t need = size_t(*w) * size_t(*h) * 3;
    if (pos + need > data.size()) { *err = "truncated"; return false; }
    px->assign(data.begin() + long(pos), data.begin() + long(pos + need));
    return true;
}

long FrameKey(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t i = 0;
    if (base.rfind("frame_", 0) == 0) i = 6;
    else if (!base.empty() && base[0] == 'f') i = 1;
    else return -1;
    if (i >= base.size() || !std::isdigit(static_cast<unsigned char>(base[i]))) return -1;
    return std::atol(base.c_str() + i);
}

int One(const std::string& path) {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    std::string err;
    if (!ReadPpm(path, &w, &h, &px, &err)) {
        std::fprintf(stderr, "te_pdesc: %s: %s\n", path.c_str(), err.c_str());
        std::printf("%ld,ERROR\n", FrameKey(path));
        std::fflush(stdout);
        return 1;
    }
    std::printf("%ld,%s\n", FrameKey(path), te::harness::Describe(px.data(), w, h, 3).c_str());
    std::fflush(stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: te_pdesc FRAME.ppm [...] | --stdin\n"); return 2; }
    int bad = 0;
    if (std::strcmp(argv[1], "--stdin") == 0) {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty()) bad += One(line);
        }
        return bad ? 1 : 0;
    }
    for (int i = 1; i < argc; ++i) bad += One(argv[i]);
    return bad ? 1 : 0;
}
