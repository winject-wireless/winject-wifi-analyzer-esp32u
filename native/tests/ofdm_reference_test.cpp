#include <fstream>
#include <iostream>
#include <iterator>

#include "ofdm.hpp"
using namespace winject::ofdm;
int main(int argc, char** argv)
{
    std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
    int n = f.tellg() / sizeof(std::complex<float>);
    f.seekg(0);
    std::vector<std::complex<float>> x(n);
    f.read((char*)x.data(), n * sizeof(x[0]));
    auto cs = detect(x, 40000000);
    if (argc > 2)
    {
        cs.push_back({std::stoi(argv[2]), std::stoi(argv[3])});
    }
    std::cerr << simd_backend() << " candidates=" << cs.size() << "\n";
    for (auto c : cs)
    {
        if (c.signal_valid)
            std::cerr << "signal " << c.signal_rate_mbps << " "
                      << c.signal_length_bytes << "\n";
        auto d = decode(x, c, 40000000);
        if (d)
        {
            std::cout << d->rate_mbps << " " << d->mpdu.size() << " "
                      << c.start_sample << " " << c.long_start_sample << "\n";
        }
    }
}
