#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include "xbox360_tkmk00.cpp"

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr, "usage: tkmk00_extract_helper input.bin output.rgba16 [alpha_color]\n");
        return 2;
    }
    int32_t alpha_color = 1;
    if (argc == 4) {
        char* end = nullptr;
        long value = std::strtol(argv[3], &end, 0);
        if (end == argv[3] || *end != '\0' || value < 0 || value > 0xFFFF) return 2;
        alpha_color = static_cast<int32_t>(value);
    }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) return 3;
    f.seekg(0, std::ios::end); std::streamoff n = f.tellg(); f.seekg(0);
    if (n < 0x30) return 4;
    std::vector<uint8_t> in((size_t)n); f.read((char*)in.data(), n);
    if (in[0]!='T'||in[1]!='K'||in[2]!='M'||in[3]!='K') return 5;
    int w=(in[8]<<8)|in[9], h=(in[10]<<8)|in[11];
    if (w<=0 || h<=0 || (size_t)w*(size_t)h > 16*1024*1024) return 6;
    std::vector<uint8_t> tmp((size_t)w*h);
    std::vector<uint8_t> out((size_t)w*h*2);
    tkmk00decode(in.data(), tmp.data(), out.data(), alpha_color);
    std::ofstream o(argv[2], std::ios::binary); if (!o) return 7;
    uint8_t hdr[8]={(uint8_t)(w>>8),(uint8_t)w,(uint8_t)(h>>8),(uint8_t)h,0,0,0,0};
    o.write((char*)hdr,8); o.write((char*)out.data(),out.size());
    return o.good()?0:8;
}
