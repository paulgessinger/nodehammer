#include <nodehammer/nhb.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void reject(std::span<const std::byte> input) {
    try {
        (void)nodehammer::fromNhb(input);
    } catch (const nodehammer::Error &) {
        return;
    }
    throw std::runtime_error("invalid input was accepted");
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "expected fixture path");
        std::ifstream file(argv[1], std::ios::binary);
        require(file.good(), "open fixture");
        const std::vector<char> contents{std::istreambuf_iterator<char>{file}, {}};
        const auto input = std::as_bytes(std::span{contents});
        const auto original = nodehammer::fromNhb(input);
        require(original.scene.nodeCount() == 3, "raw scene count");
        for (int level : {-1, 3, 9}) {
            const auto compressed = nodehammer::toNhbZstd(original.scene, level);
            require(compressed.size() >= 4 && compressed[0] == std::byte{0x28} &&
                        compressed[1] == std::byte{0xb5} && compressed[2] == std::byte{0x2f} &&
                        compressed[3] == std::byte{0xfd},
                    "zstd frame magic");
            const auto restored = nodehammer::fromNhb(compressed);
            require(restored.scene.nodeCount() == 3, "compressed scene count");
            require(nodehammer::toNhb(restored.scene) == nodehammer::toNhb(original.scene),
                    "roundtrip bytes");
            reject(std::span{compressed}.first(compressed.size() / 2));
        }
        reject({});
        reject(input.first(4));
        const std::byte invalid[] = {std::byte{0x28}, std::byte{0xb5}, std::byte{0x2f},
                                     std::byte{0xfd}};
        reject(invalid);
        try {
            (void)nodehammer::toNhbZstd(nodehammer::SemanticScene{});
            throw std::runtime_error("empty handle accepted");
        } catch (const nodehammer::Error &) {
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
