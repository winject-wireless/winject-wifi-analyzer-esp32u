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
        {
            std::cerr << "signal " << c.signal_rate_mbps << " "
                      << c.signal_length_bytes << "\n";
            std::cerr << "cand_ltf_snr " << ltf_snr_db(c) << "\n";
        }
        auto d = decode(x, c, 40000000);
        if (d)
        {
            if (!d->raw_data_iq.empty() && !d->constellation.empty())
                std::cerr << "iq " << d->raw_data_iq[0].real() << " " << d->raw_data_iq[0].imag()
                          << " " << d->constellation[0].real() << " " << d->constellation[0].imag()
                          << " " << d->raw_training_iq.size() << "\n";
            std::cerr << "ltf_snr " << d->ltf_snr_db << "\n";
            std::cout << d->rate_mbps << " " << d->mpdu.size() << " "
                      << c.start_sample << " " << c.long_start_sample << "\n";
        }
    }
}
