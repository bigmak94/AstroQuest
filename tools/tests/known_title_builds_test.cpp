// Checks how the emulator tells the builds of ASTRO BOT Rescue Mission apart, and that it only
// ever writes into one it has recognised, all of what it means to write or nothing.
//
//   clang-cl /std:c++latest /EHsc -fuse-ld=lld /I shadps4-arm64-main/src
//       tools/tests/known_title_builds_test.cpp /Fe:build/tests/known_title_builds_test.exe
//   build/tests/known_title_builds_test.exe [plain ELF of an eboot.bin ...]
//
// Without arguments it works on images made up here, which hold what a build has at the places
// the emulator looks at and nothing else. Given executables (made plain with
// `node tools/ps4elf.mjs unwrap <eboot.bin> <out.elf>`), it also says which build each is.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "core/known_title_builds.h"

#ifdef _WIN32
extern "C" {
__declspec(dllimport) void* __stdcall VirtualAlloc(void*, size_t, unsigned long, unsigned long);
__declspec(dllimport) int __stdcall VirtualFree(void*, size_t, unsigned long);
}
#endif

using namespace Core::KnownTitle::Builds;

static int failures = 0;

static void Check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    failures += ok ? 0 : 1;
}

template <typename T>
static void Put(std::vector<u8>& image, u64 at, T value) {
    std::memcpy(image.data() + at, &value, sizeof(T));
}

/// An image that holds what `build` has, as loaded, in every place the emulator looks at.
static std::vector<u8> MadeUp(const Build& build) {
    std::vector<u8> image(0x3200000);
    std::memcpy(image.data() + build.set_recentre, SetRecentreCode.data(), SetRecentreCode.size());
    Put(image, build.frame_rate, ConsoleFrameRate);
    Put(image, build.frame_seconds, ConsoleFrameSeconds);
    Put(image, build.frame_microseconds, ConsoleFrameMicroseconds);
    for (u32 level = 0; level < ConsoleSizes.size(); ++level) {
        Put(image, build.size_widths + 4 * level, ConsoleSizes[level][0]);
        Put(image, build.size_heights + 4 * level, ConsoleSizes[level][1]);
    }
    for (const Change& change : SizeChanges(build, Sizes{})) {
        std::memcpy(image.data() + change.at, &change.was, change.bytes);
    }
    for (const Change& change : PhysicsStepChanges(build)) {
        std::memcpy(image.data() + change.at, &change.was, change.bytes);
    }
    for (const Change& change : PhysicsSubstepChanges(build)) {
        std::memcpy(image.data() + change.at, &change.was, change.bytes);
    }
    for (const Change& change : ThrowLeadChanges(build)) {
        std::memcpy(image.data() + change.at, &change.was, change.bytes);
    }
    {
        // And the constant the throw multiplies by: 60, four times.
        const Change first = ThrowLeadChanges(build)[0];
        s32 distance = 0;
        std::memcpy(&distance, reinterpret_cast<const u8*>(&first.was) + 4, sizeof(distance));
        for (u32 i = 0; i < 4; ++i) {
            Put(image, first.at + 8 + distance + 4 * i, 60.0f);
        }
        // And the end of its last instruction, which stays (vmulps ..., xmm1).
        Put(image, first.at + 16, u16{0xc159});
    }
    for (const Change& change : FrameCountChanges(build)) {
        std::memcpy(image.data() + change.at, &change.was, change.bytes);
        // And the constant they are to read.
        s32 distance = 0;
        std::memcpy(&distance, reinterpret_cast<const u8*>(&change.now) + 4, sizeof(distance));
        Put(image, change.at + 8 + distance, ConsoleFrameSeconds);
    }
    return image;
}

/// Where the call in a run of code leads: the first E8 of `code`, which starts at `at`.
static u64 CallTarget(std::span<const u8> code, u64 at) {
    for (size_t i = 0; i + 5 <= code.size(); ++i) {
        if (code[i] == 0xe8) {
            s32 distance = 0;
            std::memcpy(&distance, code.data() + i + 1, sizeof(distance));
            return at + i + 5 + distance;
        }
    }
    return 0;
}

/// The soccer robots' frame counts: every place reads the same stack slot as a VEX instruction
/// of eight bytes, and is to read, with the same instruction, a constant 8 bytes on that holds
/// 1/60 as a float (where the image has it: made-up images hold nothing there).
static void CheckFrameCounts(const Build& build, std::vector<u8> image, const std::string& name) {
    const auto changes = FrameCountChanges(build);
    Check(changes.size() == 6, name + ": the soccer robots' frame counts in 6 places");
    bool same_slot = true;
    bool same_instruction = true;
    bool one_constant = true;
    bool constant_holds = true;
    u64 constant = 0;
    for (const Change& change : changes) {
        u8 was[8];
        u8 now[8];
        std::memcpy(was, &change.was, 8);
        std::memcpy(now, &change.now, 8);
        same_slot &= change.bytes == 8 && was[0] == 0xc5 && (was[3] & 0xc7) == 0x85 &&
                     (change.was >> 32) == (changes[0].was >> 32);
        same_instruction &= std::memcmp(was, now, 3) == 0 && (now[3] & 0xc7) == 0x05 &&
                            (now[3] & 0x38) == (was[3] & 0x38);
        s32 distance = 0;
        std::memcpy(&distance, now + 4, sizeof(distance));
        const u64 at = change.at + 8 + distance;
        one_constant &= constant == 0 || constant == at;
        constant = at;
        if (at + 4 <= image.size()) {
            float value = 0.0f;
            std::memcpy(&value, image.data() + at, sizeof(value));
            constant_holds &= value == ConsoleFrameSeconds;
        }
    }
    Check(same_slot, name + ": each reads the step from the same place on the stack");
    Check(same_instruction, name + ": each is to be the same instruction from rip+...");
    Check(one_constant, name + ": all of them read one constant");
    Check(constant_holds, name + ": which holds 1/60 where the image has it");
    Check(Apply(image, changes) == nullptr && Recognise(image) == &build,
          name + ": written whole, and still recognised");
}

/// The physics step of a build, changed: written once and whole, into one run of bytes that
/// still calls (or jumped to) the library's step it called before.
static void CheckPhysicsStep(const Build& build, std::vector<u8> image, const std::string& name) {
    const auto changes = PhysicsStepChanges(build);
    Check(changes.size() >= 3, name + ": the physics step is changed in " +
                                   std::to_string(changes.size()) + " places");
    bool in_a_row = true;
    u64 bytes = 0;
    for (const Change& change : changes) {
        in_a_row &= change.at == changes[0].at + bytes;
        bytes += change.bytes;
    }
    Check(in_a_row, name + ": they follow one another");
    const u64 from = changes[0].at;
    const std::vector<u8> before{image.begin() + from, image.begin() + from + bytes};
    // The step the library takes was called at the end, or jumped to (E9) where the function
    // had nothing left to do after it.
    u64 led_to = CallTarget(before, from);
    for (size_t i = 0; led_to == 0 && i + 5 <= before.size(); ++i) {
        if (before[i] == 0xe9) {
            s32 distance = 0;
            std::memcpy(&distance, before.data() + i + 1, sizeof(distance));
            led_to = from + i + 5 + distance;
        }
    }
    Check(Apply(image, changes) == nullptr, name + ": the physics step is written");
    const std::span<const u8> after{image.data() + from, bytes};
    Check(led_to != 0 && CallTarget(after, from) == led_to,
          name + ": it calls the library's step it went to before");
    // The time step is written down after the call, no longer before it.
    static constexpr std::array<u8, 6> Store{0x89, 0x87, 0x60, 0x27, 0x00, 0x00};
    const auto call = std::find(after.begin(), after.end(), u8{0xe8});
    const auto store = std::search(after.begin(), after.end(), Store.begin(), Store.end());
    Check(store != after.end() && call < store, name + ": the time step is kept after the step");
    Check(Apply(image, changes) != nullptr, name + ": not written a second time");

    std::vector<u8> odd{image};
    std::memcpy(odd.data() + from, before.data(), bytes);
    odd[changes.back().at] ^= 1;
    const std::vector<u8> untouched = odd;
    Check(Apply(odd, changes) == &changes.back() && odd == untouched,
          name + ": nothing of it written when the last place is not as expected");
}

/// The library's step of a build, made to take a long time step in two halves: one run of
/// code at the start of the function the title's own step calls, which calls the library's
/// tasks' begin, its step in so many parts, and leaves through its tasks' end. Where the image
/// has those functions (made-up images hold nothing there), they are what they are taken for.
static void CheckPhysicsSubsteps(const Build& build, std::vector<u8> image,
                                 const std::string& name) {
    const auto changes = PhysicsSubstepChanges(build);
    bool in_a_row = !changes.empty();
    u64 bytes = 0;
    for (const Change& change : changes) {
        in_a_row &= change.bytes == 8 && change.at == changes[0].at + bytes;
        bytes += change.bytes;
    }
    Check(in_a_row && bytes == 96, name + ": the half steps are 96 bytes in a row");
    const u64 from = changes[0].at;
    // It replaces the start of the function the title's step calls or jumps to.
    std::vector<u8> stepped = image;
    const auto step = PhysicsStepChanges(build);
    u64 step_bytes = 0;
    for (const Change& change : step) {
        step_bytes += change.bytes;
    }
    Check(Apply(stepped, step) == nullptr &&
              CallTarget(std::span<const u8>{stepped.data() + step[0].at, step_bytes},
                         step[0].at) == from,
          name + ": they start the function the title's step calls");
    const std::vector<u8> before{image.begin() + from, image.begin() + from + bytes};
    Check(Apply(image, changes) == nullptr, name + ": the half steps are written");
    const std::span<const u8> after{image.data() + from, bytes};
    // Two calls and a jump out, in that order.
    std::vector<u64> calls;
    u64 leaves_to = 0;
    for (size_t i = 0; i + 5 <= after.size();) {
        s32 distance = 0;
        std::memcpy(&distance, after.data() + i + 1, sizeof(distance));
        if (after[i] == 0xe8) {
            calls.push_back(from + i + 5 + distance);
            i += 5;
        } else if (after[i] == 0xe9 && calls.size() == 2) {
            leaves_to = from + i + 5 + distance;
            break;
        } else {
            ++i;
        }
    }
    Check(calls.size() == 2 && leaves_to != 0, name + ": two calls and a jump out");
    if (calls.size() == 2 && leaves_to != 0) {
        Check(leaves_to == calls[0] + 0x20 && calls[1] > from && calls[1] < from + 0x1000,
              name + ": to the tasks' begin, the step in parts just after, the tasks' end");
        // The time step that is too long for one go, as the float the code compares with.
        u32 longest = 0;
        std::memcpy(&longest, &LongestWholeStep, sizeof(longest));
        static constexpr std::array<u8, 3> Compare{0x41, 0x81, 0xfc};
        const auto compare = std::search(after.begin(), after.end(), Compare.begin(), Compare.end());
        u32 compared = 0;
        if (after.end() - compare >= 7) {
            std::memcpy(&compared, &*compare + 3, sizeof(compared));
        }
        Check(compared == longest, name + ": a time step above LongestWholeStep is halved");
        const auto holds = [&](u64 at, std::initializer_list<u8> code) {
            return at + code.size() <= image.size() &&
                   std::equal(code.begin(), code.end(), image.begin() + at);
        };
        if (image[calls[0]] != 0) {
            // mov rdi, [rdi+0xf88]; test rdi, rdi; je; mov rax, [rdi]; jmp [rax+0x10] / [rax+0x18]
            Check(holds(calls[0], {0x48, 0x8b, 0xbf, 0x88, 0x0f, 0x00, 0x00}) &&
                      holds(calls[0] + 15, {0xff, 0x60, 0x10}) &&
                      holds(leaves_to, {0x48, 0x8b, 0xbf, 0x88, 0x0f, 0x00, 0x00}) &&
                      holds(leaves_to + 15, {0xff, 0x60, 0x18}),
                  name + ": the tasks' begin and end are those");
            // push rbp; mov rbp, rsp; ... mov [rbp-0x10c], esi: how many parts it is to take.
            static constexpr std::array<u8, 6> Parts{0x89, 0xb5, 0xf4, 0xfe, 0xff, 0xff};
            const auto start = image.begin() + calls[1];
            Check(holds(calls[1], {0x55, 0x48, 0x89, 0xe5}) &&
                      std::search(start, start + 0x30, Parts.begin(), Parts.end()) != start + 0x30,
                  name + ": the step in parts takes their number");
        }
    }
    Check(Apply(image, changes) != nullptr, name + ": not written a second time");
    std::vector<u8> odd{image};
    std::memcpy(odd.data() + from, before.data(), bytes);
    odd[changes.back().at] ^= 1;
    const std::vector<u8> untouched = odd;
    Check(Apply(odd, changes) == &changes.back() && odd == untouched,
          name + ": nothing of them written when the last place is not as expected");
}

/// The hero's lead on a moving target: 16 bytes that multiplied the target's way since the
/// frame before (xmm8) by 60 and by the flight's time (xmm9), and are to divide it by the
/// engine's seconds a frame instead. Run here as the processor runs them, with a time of a
/// frame of its own beside them: the lead is to be way / seconds a frame x time.
static void CheckThrowLead(const Build& build, std::vector<u8> image, const std::string& name) {
    const auto changes = ThrowLeadChanges(build);
    Check(changes.size() == 2 && changes[0].bytes == 8 && changes[1].bytes == 8 &&
              changes[1].at == changes[0].at + 8,
          name + ": the throw's lead is 16 bytes in a row");
    const u64 from = changes[0].at;
    // vmulps xmm0, xmm8, [rip+...]: 60 four times.
    static constexpr std::array<u8, 4> Times{0xc5, 0xb8, 0x59, 0x05};
    s32 distance = 0;
    std::memcpy(&distance, image.data() + from + 4, sizeof(distance));
    std::array<float, 4> sixty{};
    std::memcpy(sixty.data(), image.data() + from + 8 + distance, sizeof(sixty));
    Check(std::equal(Times.begin(), Times.end(), image.begin() + from) &&
              sixty == std::array<float, 4>{60.0f, 60.0f, 60.0f, 60.0f},
          name + ": it multiplied the way a frame by 60");
    Check(Apply(image, changes) == nullptr, name + ": the throw's lead is written");
    // vdivss xmm1, xmm9, [rip+...]: the engine's seconds a frame.
    static constexpr std::array<u8, 4> Over{0xc5, 0xb2, 0x5e, 0x0d};
    std::memcpy(&distance, image.data() + from + 4, sizeof(distance));
    Check(std::equal(Over.begin(), Over.end(), image.begin() + from) &&
              from + 8 + distance == build.frame_seconds,
          name + ": it divides by the engine's seconds a frame");
#if defined(_WIN32) && defined(__x86_64__)
    // sub rsp, 0x28; vmovups [rsp], xmm8; vmovups [rsp+0x10], xmm9; vmovups xmm8, [rcx];
    // vmovss xmm9, [rdx]; <the 18 bytes of the lead>; vmovups [r8], xmm0; vmovups xmm8, [rsp];
    // vmovups xmm9, [rsp+0x10]; add rsp, 0x28; ret; and at 64 the seconds a frame.
    static constexpr std::array<u8, 23> Before{0x48, 0x83, 0xec, 0x28, 0xc5, 0x78, 0x11, 0x04,
                                               0x24, 0xc5, 0x78, 0x11, 0x4c, 0x24, 0x10, 0xc5,
                                               0x78, 0x10, 0x01, 0xc5, 0x7a, 0x10, 0x0a};
    static constexpr std::array<u8, 21> After{0xc4, 0xc1, 0x78, 0x11, 0x00, 0xc5, 0x78,
                                              0x10, 0x04, 0x24, 0xc5, 0x78, 0x10, 0x4c,
                                              0x24, 0x10, 0x48, 0x83, 0xc4, 0x28, 0xc3};
    auto* const code = static_cast<u8*>(VirtualAlloc(nullptr, 4096, 0x3000, 0x40));
    bool right = code != nullptr;
    if (code != nullptr) {
        std::memcpy(code, Before.data(), Before.size());
        std::memcpy(code + 23, image.data() + from, 18);
        const s32 to_seconds = 64 - (23 + 8);
        std::memcpy(code + 23 + 4, &to_seconds, sizeof(to_seconds));
        std::memcpy(code + 41, After.data(), After.size());
        const auto lead = reinterpret_cast<void (*)(const float*, const float*, float*)>(code);
        // A target that goes 6 units a second, seen at 30, 60 and 120 frames a second, and a
        // flight of half a second: 3 units ahead every time.
        for (const float seconds : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 120.0f}) {
            std::memcpy(code + 64, &seconds, sizeof(seconds));
            const float way[4]{6.0f * seconds, -3.0f * seconds, 0.0f, 0.0f};
            const float time = 0.5f;
            float ahead[4]{};
            lead(way, &time, ahead);
            right &= std::abs(ahead[0] - 3.0f) < 1e-4f && std::abs(ahead[1] + 1.5f) < 1e-4f &&
                     ahead[2] == 0.0f;
        }
        VirtualFree(code, 0, 0x8000);
    }
    Check(right, name + ": run, it leads by speed x time at 30, 60 and 120 frames a second");
#endif
    Check(Apply(image, changes) != nullptr, name + ": not written a second time");
}

/// Twice the console's sizes, as the emulator works them out for a width of 2880.
static Sizes Doubled() {
    Sizes sizes;
    for (s32 level = FirstHeadsetLevel; level <= LastHeadsetLevel; ++level) {
        sizes.sizes[level] = {ConsoleSizes[level][0] * 2, ConsoleSizes[level][1] * 2};
    }
    sizes.target_pool = 880u << 20;
    sizes.small_pool = 44u << 20;
    sizes.graphics_heap = 2186ull << 20;
    return sizes;
}

/// The image an executable makes in memory: its loaded parts at their addresses.
static std::vector<u8> Load(const char* path) {
    std::ifstream file{path, std::ios::binary};
    std::vector<u8> elf{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (elf.size() < 0x40 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0) {
        return {};
    }
    const auto read = [&](u64 at, auto& value) {
        if (at + sizeof(value) > elf.size()) {
            return false;
        }
        std::memcpy(&value, elf.data() + at, sizeof(value));
        return true;
    };
    u64 headers = 0;
    u16 header_size = 0;
    u16 header_count = 0;
    read(0x20, headers);
    read(0x36, header_size);
    read(0x38, header_count);
    std::vector<u8> image;
    for (u32 i = 0; i < header_count; ++i) {
        const u64 at = headers + u64{i} * header_size;
        u32 type = 0;
        u64 offset = 0, address = 0, in_file = 0, in_memory = 0;
        if (!read(at, type) || !read(at + 8, offset) || !read(at + 16, address) ||
            !read(at + 32, in_file) || !read(at + 40, in_memory)) {
            return {};
        }
        // PT_LOAD and PT_SCE_RELRO.
        if ((type != 1 && type != 0x61000010) || offset + in_file > elf.size()) {
            continue;
        }
        if (image.size() < address + in_memory) {
            image.resize(address + in_memory);
        }
        std::memcpy(image.data() + address, elf.data() + offset, in_file);
    }
    return image;
}

int main(int argc, char** argv) {
    for (const Build& build : Known) {
        const std::string name = build.name;
        std::vector<u8> image = MadeUp(build);
        Check(Recognise(image) == &build, name + ": recognised");
        Check(Recognise(std::span<const u8>{}) == nullptr, name + ": not an empty image");
        Check(Recognise(std::span{image}.first(build.set_recentre + 4)) == nullptr,
              name + ": not an image cut short");

        // One byte off anywhere the emulator looks, and it is no build that is known.
        std::vector<u64> places{build.set_recentre + 3, build.frame_rate, build.frame_seconds,
                                build.frame_microseconds, build.size_widths + 4 * 6,
                                build.size_heights + 4 * 2};
        for (const Change& change : SizeChanges(build, Sizes{})) {
            places.push_back(change.at);
        }
        bool all_refused = true;
        for (const u64 place : places) {
            image[place] ^= 0x40;
            all_refused &= Recognise(image) == nullptr;
            image[place] ^= 0x40;
        }
        Check(all_refused, name + ": refused with any of " + std::to_string(places.size()) +
                               " places changed");
        Check(Recognise(image) == &build, name + ": recognised again once they are restored");

        // Larger sizes: all of them written, the television's left alone.
        const Sizes doubled = Doubled();
        const auto changes = SizeChanges(build, doubled);
        Check(changes.size() == 30, name + ": 30 places set the sizes");
        std::vector<u8> larger = image;
        Check(Apply(larger, changes) == nullptr, name + ": larger sizes written");
        bool written = true;
        for (const Change& change : changes) {
            u64 found = 0;
            std::memcpy(&found, larger.data() + change.at, change.bytes);
            written &= found == change.now;
        }
        Check(written, name + ": every place holds the larger size");
        u32 television = 0;
        std::memcpy(&television, larger.data() + build.size_widths + 4 * 2, sizeof(u32));
        Check(television == 1920, name + ": the television's sizes are left alone");
        Check(Apply(larger, changes) != nullptr, name + ": not written a second time");
        Check(Recognise(larger) == nullptr, name + ": an image with larger sizes is not asked "
                                                   "what it is again");

        // One place that is not as expected, the last one: nothing at all is written.
        std::vector<u8> odd = image;
        odd[changes.back().at] ^= 1;
        const std::vector<u8> before = odd;
        Check(Apply(odd, changes) == &changes.back() && odd == before,
              name + ": nothing written when the last place is not as expected");
        // A place beyond the image.
        const std::vector<Change> beyond{{image.size() - 2, 0, 1, 4}};
        Check(Apply(image, beyond) == &beyond[0], name + ": nothing written beyond the image");

        CheckPhysicsStep(build, image, name);
        CheckPhysicsSubsteps(build, image, name);
        CheckThrowLead(build, image, name);
        CheckFrameCounts(build, image, name);
        // What the game is told apart by does not depend on it.
        std::vector<u8> stepped = image;
        Check(Apply(stepped, PhysicsStepChanges(build)) == nullptr &&
                  Recognise(stepped) == &build,
              name + ": still recognised with its physics step changed");
    }

    // An image that would be both builds at once is neither.
    std::vector<u8> both = MadeUp(Known[0]);
    const std::vector<u8> other = MadeUp(Known[1]);
    for (size_t i = 0; i < both.size(); ++i) {
        both[i] |= other[i];
    }
    Check(Is(Known[0], both) && Is(Known[1], both) && Recognise(both) == nullptr,
          "an image that is both builds at once is neither");

    for (int i = 1; i < argc; ++i) {
        const std::vector<u8> image = Load(argv[i]);
        const Build* build = image.empty() ? nullptr : Recognise(image);
        std::printf("%s: %s\n", argv[i],
                    image.empty() ? "not a plain ELF"
                                  : build != nullptr ? build->name : "no build that is known");
        if (build != nullptr) {
            std::vector<u8> larger = image;
            Check(Apply(larger, SizeChanges(*build, Doubled())) == nullptr,
                  std::string{"larger sizes written into "} + argv[i]);
            CheckPhysicsStep(*build, image, argv[i]);
            CheckPhysicsSubsteps(*build, image, argv[i]);
            CheckThrowLead(*build, image, argv[i]);
            CheckFrameCounts(*build, image, argv[i]);
        }
    }

    std::printf("failed: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
