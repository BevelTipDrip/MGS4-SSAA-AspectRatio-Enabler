#include "pch.hpp"
#include "shader_port.hpp"

#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>

namespace mgs4e::shaderport
{
    namespace
    {
        // ---- DXBC container ----------------------------------------------------------------
        struct Chunk { uint32_t offset; uint32_t size; };   // offset of the data (after the 8-byte header)

        bool FindChunk(const uint8_t* blob, size_t length, const char* fourcc, Chunk& out)
        {
            if (length < 0x24 || std::memcmp(blob, "DXBC", 4) != 0) { return false; }
            uint32_t count = 0;
            std::memcpy(&count, blob + 0x1C, 4);
            if (count > 64 || 0x20 + static_cast<size_t>(count) * 4 > length) { return false; }
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t at = 0;
                std::memcpy(&at, blob + 0x20 + i * 4, 4);
                if (static_cast<size_t>(at) + 8 > length || std::memcmp(blob + at, fourcc, 4) != 0) { continue; }
                uint32_t size = 0;
                std::memcpy(&size, blob + at + 4, 4);
                if (static_cast<size_t>(at) + 8 + size > length) { return false; }
                out = { at + 8, size };
                return true;
            }
            return false;
        }

        bool FindCode(const uint8_t* blob, size_t length, Chunk& out)
        {
            return FindChunk(blob, length, "SHEX", out) || FindChunk(blob, length, "SHDR", out);
        }

        // ---- MD5 with the DXBC final block -----------------------------------------------------
        void Md5Transform(uint32_t state[4], const uint8_t block[64])
        {
            static const uint32_t K[64] = {
                0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
                0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
                0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
                0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
                0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
                0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
                0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
                0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
            static const uint32_t R[64] = {
                7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
            uint32_t M[16];
            std::memcpy(M, block, 64);
            uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
            for (uint32_t i = 0; i < 64; ++i)
            {
                uint32_t f, g;
                if (i < 16) { f = (b & c) | (~b & d); g = i; }
                else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
                else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
                else { f = c ^ (b | ~d); g = (7 * i) % 16; }
                f = f + a + K[i] + M[g];
                a = d; d = c; c = b;
                b = b + ((f << R[i]) | (f >> (32 - R[i])));
            }
            state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        }
    }

    Checksum ComputeChecksum(const uint8_t* blob, size_t length)
    {
        Checksum out {};
        if (length < 20) { return out; }
        const uint8_t* data = blob + 20;
        const size_t size = length - 20;
        uint32_t state[4] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476 };
        const uint32_t bits = static_cast<uint32_t>(size * 8);
        const size_t full = size & ~static_cast<size_t>(63);
        for (size_t i = 0; i < full; i += 64) { Md5Transform(state, data + i); }
        const uint8_t* last = data + full;
        const size_t n = size - full;
        uint8_t block[64] {};
        if (n >= 56)
        {
            std::memcpy(block, last, n);
            block[n] = 0x80;
            Md5Transform(state, block);
            uint8_t block2[64] {};
            std::memcpy(block2, &bits, 4);
            const uint32_t tail = (bits >> 2) | 1;
            std::memcpy(block2 + 60, &tail, 4);
            Md5Transform(state, block2);
        }
        else
        {
            std::memcpy(block, &bits, 4);
            std::memcpy(block + 4, last, n);
            block[4 + n] = 0x80;
            const uint32_t tail = (bits >> 2) | 1;
            std::memcpy(block + 60, &tail, 4);
            Md5Transform(state, block);
        }
        std::memcpy(out.data(), state, 16);
        return out;
    }

    bool ParseChecksumName(const std::string& name, Checksum& out)
    {
        if (name.size() < 35) { return false; }
        for (int dword = 0; dword < 4; ++dword)
        {
            const char* begin = name.data() + dword * 9;
            uint32_t value = 0;
            if (std::from_chars(begin, begin + 8, value, 16).ec != std::errc()) { return false; }
            if (dword < 3 && begin[8] != '-') { return false; }
            std::memcpy(out.data() + dword * 4, &value, 4);
        }
        return true;
    }

    std::string ChecksumName(const Checksum& sum)
    {
        uint32_t w[4];
        std::memcpy(w, sum.data(), 16);
        return std::format("{:08x}-{:08x}-{:08x}-{:08x}", w[0], w[1], w[2], w[3]);
    }

    namespace
    {
        // ---- shader model 4/5 token stream ---------------------------------------------------
        constexpr uint32_t kOpcodeCustomData = 0x35;
        constexpr uint32_t kOpcodeDclResource = 0x58;
        constexpr uint32_t kOpcodeDclConstantBuffer = 0x59;
        constexpr uint32_t kOperandImmediate32 = 4;
        constexpr uint32_t kOperandImmediate64 = 5;
        constexpr uint32_t kOperandConstantBuffer = 8;

        using OnConstant = std::function<void(size_t slotPos, size_t indexPos)>;

        // Length of the operand at w[j] in dwords; reports constant-buffer operands whose slot
        // and element are immediate (the element may also carry a relative part: the immediate
        // base is what shifts).
        size_t OperandSpan(const std::vector<uint32_t>& w, size_t j, const OnConstant& onConstant, bool& bad)
        {
            if (j >= w.size()) { bad = true; return 1; }
            const uint32_t t = w[j];
            const uint32_t type = (t >> 12) & 0xFF;
            const uint32_t components = t & 3;
            const uint32_t dimension = (t >> 20) & 3;
            const uint32_t reps[3] = { (t >> 22) & 7, (t >> 25) & 7, (t >> 28) & 7 };
            size_t k = j + 1;
            uint32_t extended = t >> 31;
            while (extended)
            {
                if (k >= w.size()) { bad = true; return k - j; }
                extended = w[k] >> 31;
                ++k;
            }
            if (type == kOperandImmediate32) { k += components == 2 ? 4 : 1; return k - j; }
            if (type == kOperandImmediate64) { k += components == 2 ? 8 : 2; return k - j; }
            size_t positions[3] = { SIZE_MAX, SIZE_MAX, SIZE_MAX };
            for (uint32_t d = 0; d < dimension; ++d)
            {
                switch (reps[d])
                {
                case 0: positions[d] = k; k += 1; break;                                        // imm32
                case 1: k += 2; break;                                                          // imm64
                case 2: k += OperandSpan(w, k, onConstant, bad); break;                         // relative
                case 3: positions[d] = k; k += 1; k += OperandSpan(w, k, onConstant, bad); break; // imm32 + relative
                case 4: k += 2; k += OperandSpan(w, k, onConstant, bad); break;                 // imm64 + relative
                default: bad = true; return k - j;
                }
            }
            if (type == kOperandConstantBuffer && dimension == 2 && positions[0] != SIZE_MAX && positions[1] != SIZE_MAX && onConstant)
            {
                onConstant(positions[0], positions[1]);
            }
            return k - j;
        }

        // Walks every instruction; calls onDecl(operandPos) for dcl_constantbuffer and
        // onConstant for every constant-buffer read. Returns false on a malformed stream.
        bool Walk(std::vector<uint32_t>& w, const OnConstant& onConstant, const std::function<void(size_t)>& onDecl)
        {
            bool bad = false;
            size_t i = 2;
            while (i < w.size())
            {
                const uint32_t t = w[i];
                const uint32_t op = t & 0x7FF;
                size_t length = (t >> 24) & 0x7F;
                if (op == kOpcodeCustomData)
                {
                    if (i + 1 >= w.size()) { return false; }
                    length = w[i + 1];
                    if (length < 2) { return false; }
                    i += length;
                    continue;
                }
                if (length == 0) { break; }
                const size_t end = i + length;
                if (end > w.size()) { return false; }
                size_t j = i + 1;
                uint32_t extended = t >> 31;
                while (extended && j < end) { extended = w[j] >> 31; ++j; }
                if (op == kOpcodeDclConstantBuffer)
                {
                    onDecl(j);
                    i = end;
                    continue;
                }
                // Declarations whose payload is not a plain operand list, or that never read
                // constants: skipped whole.
                const bool skip = op == 0x5C || op == 0x5D || op == 0x5E || op == 0x60 || op == 0x61 || op == 0x63 || op == 0x64
                    || op == 0x66 || op == 0x67 || op == 0x68 || op == 0x69 || op == 0x6A;
                if (skip) { i = end; continue; }
                if (op == kOpcodeDclResource) { i = end; continue; }
                while (j < end && !bad)
                {
                    j += OperandSpan(w, j, onConstant, bad);
                }
                if (bad) { return false; }
                i = end;
            }
            return true;
        }

        // slot -> declared size of every dcl_constantbuffer.
        std::map<uint32_t, uint32_t> DeclaredSizes(const uint8_t* blob, size_t length, bool& ok)
        {
            std::map<uint32_t, uint32_t> sizes;
            Chunk code {};
            if (!FindCode(blob, length, code)) { ok = false; return sizes; }
            std::vector<uint32_t> w(code.size / 4);
            std::memcpy(w.data(), blob + code.offset, w.size() * 4);
            ok = Walk(w, nullptr, [&](size_t operand) { if (operand + 2 < w.size()) { sizes[w[operand + 1]] = w[operand + 2]; } });
            return sizes;
        }
    }

    std::vector<uint8_t> Port(const std::vector<uint8_t>& replacement, const uint8_t* original, size_t originalLength, std::string& why)
    {
        bool ok = true;
        const auto sizesOriginal = DeclaredSizes(original, originalLength, ok);
        if (!ok) { why = "the DirectX 12 shader's token stream could not be read"; return {}; }
        const auto sizesReplacement = DeclaredSizes(replacement.data(), replacement.size(), ok);
        if (!ok) { why = "the replacement's token stream could not be read"; return {}; }
        if (sizesOriginal.size() != sizesReplacement.size()) { why = "different constant buffers"; return {}; }
        std::map<uint32_t, int64_t> deltas;
        for (const auto& [slot, size] : sizesReplacement)
        {
            const auto it = sizesOriginal.find(slot);
            if (it == sizesOriginal.end()) { why = "different constant buffers"; return {}; }
            deltas[slot] = static_cast<int64_t>(it->second) - static_cast<int64_t>(size);
        }

        std::vector<uint8_t> out = replacement;
        Chunk code {};
        if (!FindCode(out.data(), out.size(), code)) { why = "no code chunk"; return {}; }
        std::vector<uint32_t> w(code.size / 4);
        std::memcpy(w.data(), out.data() + code.offset, w.size() * 4);
        bool bad = false;
        const bool walked = Walk(w,
            [&](size_t slotPos, size_t indexPos)
            {
                const auto it = deltas.find(w[slotPos]);
                if (it == deltas.end()) { bad = true; return; }
                const int64_t shifted = static_cast<int64_t>(w[indexPos]) + it->second;
                if (shifted < 0 || shifted > 0xFFFFFFFFll) { bad = true; return; }
                w[indexPos] = static_cast<uint32_t>(shifted);
            },
            [&](size_t operand)
            {
                if (operand + 2 >= w.size()) { bad = true; return; }
                const auto it = sizesOriginal.find(w[operand + 1]);
                if (it == sizesOriginal.end()) { bad = true; return; }
                w[operand + 2] = it->second;
            });
        if (!walked || bad) { why = "the replacement's constant reads could not be shifted"; return {}; }
        std::memcpy(out.data() + code.offset, w.data(), w.size() * 4);

        // The input signature: the DirectX 12 original's masks are what its vertex shaders feed.
        Chunk isgnOut {}, isgnOriginal {};
        if (!FindChunk(out.data(), out.size(), "ISGN", isgnOut) || !FindChunk(original, originalLength, "ISGN", isgnOriginal)
            || isgnOut.size != isgnOriginal.size)
        {
            why = "input signatures differ in size";
            return {};
        }
        std::memcpy(out.data() + isgnOut.offset, original + isgnOriginal.offset, isgnOut.size);

        const Checksum sum = ComputeChecksum(out.data(), out.size());
        std::memcpy(out.data() + 4, sum.data(), 16);
        return out;
    }

    std::map<Checksum, Checksum> LoadMap(const std::filesystem::path& file)
    {
        std::map<Checksum, Checksum> map;
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == ';' || line[0] == '#') { continue; }
            const size_t space = line.find(' ');
            if (space == std::string::npos) { continue; }
            Checksum dx11 {}, dx12 {};
            std::string second = line.substr(space + 1);
            while (!second.empty() && (second.back() == '\r' || second.back() == ' ')) { second.pop_back(); }
            if (ParseChecksumName(line.substr(0, space), dx11) && ParseChecksumName(second, dx12))
            {
                map[dx12] = dx11;
            }
        }
        return map;
    }
}
