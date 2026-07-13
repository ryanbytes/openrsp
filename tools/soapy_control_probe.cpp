#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Types.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr double kSampleRate = 2000000.0;
constexpr double kFrequency = 101100000.0;
constexpr double kBandwidth = 1536000.0;
constexpr double kMaximumRateError = 0.05;

struct Measurement {
    size_t samples = 0;
    double rms = 0.0;
    double peak = 0.0;
};

Measurement measure(SoapySDR::Device *device, SoapySDR::Stream *stream,
                    size_t discard_samples, size_t measure_samples)
{
    std::vector<std::complex<short>> samples(device->getStreamMTU(stream));
    if (samples.empty()) samples.resize(4096);

    size_t discarded = 0;
    size_t measured = 0;
    long double sum_i = 0.0;
    long double sum_q = 0.0;
    long double sum_power = 0.0;
    double peak = 0.0;
    unsigned int timeouts = 0;

    while (measured < measure_samples) {
        void *buffers[] = {samples.data()};
        int flags = 0;
        long long time_ns = 0;
        const int result = device->readStream(stream, buffers, samples.size(),
                                              flags, time_ns, 1000000);
        if (result == SOAPY_SDR_TIMEOUT) {
            if (++timeouts > 5) throw std::runtime_error("repeated stream timeout");
            continue;
        }
        if (result < 0)
            throw std::runtime_error(std::string("readStream: ") +
                                     SoapySDR::errToStr(result));
        timeouts = 0;

        size_t offset = 0;
        if (discarded < discard_samples) {
            const size_t count = std::min(static_cast<size_t>(result),
                                          discard_samples - discarded);
            discarded += count;
            offset += count;
        }
        const size_t count = std::min(static_cast<size_t>(result) - offset,
                                      measure_samples - measured);
        for (size_t index = offset; index < offset + count; ++index) {
            const double i = samples[index].real();
            const double q = samples[index].imag();
            sum_i += i;
            sum_q += q;
            sum_power += i * i + q * q;
            peak = std::max(peak, std::hypot(i, q));
        }
        measured += count;
    }

    const long double mean_i = sum_i / measured;
    const long double mean_q = sum_q / measured;
    long double variance = sum_power / measured - mean_i * mean_i - mean_q * mean_q;
    if (variance < 0.0) variance = 0.0;
    Measurement measurement;
    measurement.samples = measured;
    measurement.rms = std::sqrt(static_cast<double>(variance));
    measurement.peak = peak;
    return measurement;
}

void require_gain(SoapySDR::Device *device, const std::string &name,
                  double requested)
{
    device->setGain(SOAPY_SDR_RX, 0, name, requested);
    const double reported = device->getGain(SOAPY_SDR_RX, 0, name);
    if (reported != requested)
        throw std::runtime_error(name + " readback mismatch: requested " +
                                 std::to_string(requested) + ", got " +
                                 std::to_string(reported));
}

void print_measurement(const char *label, const Measurement &measurement)
{
    std::cout << label << ": samples=" << measurement.samples
              << " ac_rms=" << measurement.rms
              << " peak=" << measurement.peak << '\n';
}

double measure_rate(SoapySDR::Device *device, SoapySDR::Stream *stream,
                    double expected_rate)
{
    using clock = std::chrono::steady_clock;
    std::vector<std::complex<short>> samples(device->getStreamMTU(stream));
    if (samples.empty()) samples.resize(4096);
    const size_t request_samples = std::min(
        samples.size(), std::max(static_cast<size_t>(1024),
                                 static_cast<size_t>(expected_rate / 10.0)));
    const auto settle_until = clock::now() + std::chrono::milliseconds(750);
    const auto measure_for = std::chrono::milliseconds(1500);
    unsigned int timeouts = 0;

    while (clock::now() < settle_until) {
        void *buffers[] = {samples.data()};
        int flags = 0;
        long long time_ns = 0;
        const int result = device->readStream(stream, buffers, request_samples,
                                              flags, time_ns, 1000000);
        if (result == SOAPY_SDR_TIMEOUT) {
            if (++timeouts > 5) throw std::runtime_error("repeated stream timeout while settling");
            continue;
        }
        if (result < 0)
            throw std::runtime_error(std::string("readStream while settling: ") +
                                     SoapySDR::errToStr(result));
        timeouts = 0;
    }

    size_t count = 0;
    const auto started = clock::now();
    const auto measure_until = started + measure_for;
    while (clock::now() < measure_until) {
        void *buffers[] = {samples.data()};
        int flags = 0;
        long long time_ns = 0;
        const int result = device->readStream(stream, buffers, request_samples,
                                              flags, time_ns, 1000000);
        if (result == SOAPY_SDR_TIMEOUT) {
            if (++timeouts > 5) throw std::runtime_error("repeated stream timeout while measuring rate");
            continue;
        }
        if (result < 0)
            throw std::runtime_error(std::string("readStream while measuring rate: ") +
                                     SoapySDR::errToStr(result));
        timeouts = 0;
        count += static_cast<size_t>(result);
    }
    const double elapsed = std::chrono::duration<double>(clock::now() - started).count();
    return static_cast<double>(count) / elapsed;
}

void verify_rate(SoapySDR::Device *device, SoapySDR::Stream *stream,
                 double requested)
{
    device->setSampleRate(SOAPY_SDR_RX, 0, requested);
    const double reported = device->getSampleRate(SOAPY_SDR_RX, 0);
    if (reported != requested)
        throw std::runtime_error("sample-rate readback mismatch: requested " +
                                 std::to_string(requested) + ", got " +
                                 std::to_string(reported));
    const double actual = measure_rate(device, stream, requested);
    const double error = std::fabs(actual - requested) / requested;
    std::cout << "rate requested=" << requested << " measured=" << actual
              << " error=" << error * 100.0 << "%\n";
    if (error > kMaximumRateError)
        throw std::runtime_error("measured sample rate differs by more than 5% at " +
                                 std::to_string(requested));
}

void prepare_rate_measurement(SoapySDR::Device *device)
{
    device->setGainMode(SOAPY_SDR_RX, 0, false);
    require_gain(device, "IFGR", 40.0);
    require_gain(device, "RFGR", 5.0);
}

void finish_rate_measurement(SoapySDR::Device *device)
{
    device->setSampleRate(SOAPY_SDR_RX, 0, kSampleRate);
    device->setGainMode(SOAPY_SDR_RX, 0, true);
}

void run_rate_matrix(SoapySDR::Device *device, SoapySDR::Stream *stream)
{
    const std::vector<double> rates = device->listSampleRates(SOAPY_SDR_RX, 0);
    if (rates.empty()) throw std::runtime_error("Soapy module advertised no sample rates");
    prepare_rate_measurement(device);
    for (const double requested : rates) verify_rate(device, stream, requested);
    finish_rate_measurement(device);
    std::cout << "PASS: every Soapy-advertised single-tuner sample rate remained "
                 "within 5% of wall-clock output; AGC restored\n";
}

} // namespace

int main(int argc, char **argv)
{
    SoapySDR::Kwargs args;
    args["driver"] = "sdrplay";
    bool rate_matrix = false;
    double single_rate = 0.0;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--rates") rate_matrix = true;
        else if (argument == "--rate" && index + 1 < argc) {
            try {
                single_rate = std::stod(argv[++index]);
            } catch (const std::exception &) {
                std::cerr << "--rate requires a numeric samples-per-second value\n";
                return EXIT_FAILURE;
            }
            if (!std::isfinite(single_rate) || single_rate <= 0.0) {
                std::cerr << "--rate must be finite and greater than zero\n";
                return EXIT_FAILURE;
            }
        }
        else if (argument == "--serial" && index + 1 < argc) args["serial"] = argv[++index];
        else {
            std::cerr << "usage: " << argv[0]
                      << " [--serial SERIAL] [--rates | --rate SPS]\n";
            return EXIT_FAILURE;
        }
    }
    if (rate_matrix && single_rate != 0.0) {
        std::cerr << "--rates and --rate are mutually exclusive\n";
        return EXIT_FAILURE;
    }

    SoapySDR::Device *device = nullptr;
    SoapySDR::Stream *stream = nullptr;
    bool active = false;
    bool controls_changed = false;
    try {
        const SoapySDR::KwargsList devices = SoapySDR::Device::enumerate(args);
        if (devices.empty()) throw std::runtime_error("no Soapy SDRplay device found");
        device = SoapySDR::Device::make(devices.front());
        if (device == nullptr) throw std::runtime_error("Soapy device creation failed");

        const std::vector<std::string> gains = device->listGains(SOAPY_SDR_RX, 0);
        if (std::find(gains.begin(), gains.end(), "IFGR") == gains.end() ||
            std::find(gains.begin(), gains.end(), "RFGR") == gains.end())
            throw std::runtime_error("Soapy module did not advertise IFGR and RFGR");

        controls_changed = true;
        if (rate_matrix || single_rate != 0.0) {
            device->setGainMode(SOAPY_SDR_RX, 0, false);
            device->setGain(SOAPY_SDR_RX, 0, "IFGR", 40.0);
            device->setGain(SOAPY_SDR_RX, 0, "RFGR", 5.0);
        }
        device->setSampleRate(SOAPY_SDR_RX, 0, kSampleRate);
        device->setFrequency(SOAPY_SDR_RX, 0, kFrequency);
        device->setBandwidth(SOAPY_SDR_RX, 0, kBandwidth);
        stream = device->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16);
        if (stream == nullptr) throw std::runtime_error("setupStream returned null");
        const int activate_result = device->activateStream(stream);
        if (activate_result != 0)
            throw std::runtime_error(std::string("activateStream: ") +
                                     SoapySDR::errToStr(activate_result));
        active = true;

        if (rate_matrix) {
            run_rate_matrix(device, stream);
            device->deactivateStream(stream);
            active = false;
            device->closeStream(stream);
            stream = nullptr;
            SoapySDR::Device::unmake(device);
            return EXIT_SUCCESS;
        }
        if (single_rate != 0.0) {
            prepare_rate_measurement(device);
            verify_rate(device, stream, single_rate);
            finish_rate_measurement(device);
            std::cout << "PASS: requested Soapy sample rate verified; AGC restored\n";
            device->deactivateStream(stream);
            active = false;
            device->closeStream(stream);
            stream = nullptr;
            SoapySDR::Device::unmake(device);
            return EXIT_SUCCESS;
        }

        device->setGainMode(SOAPY_SDR_RX, 0, false);
        if (device->getGainMode(SOAPY_SDR_RX, 0))
            throw std::runtime_error("AGC remained enabled after manual-mode request");

        require_gain(device, "IFGR", 59.0);
        require_gain(device, "RFGR", 9.0);
        const Measurement low = measure(device, stream, 500000, 1000000);
        print_measurement("IFGR=59 RFGR=9", low);

        require_gain(device, "IFGR", 40.0);
        require_gain(device, "RFGR", 5.0);
        const Measurement middle = measure(device, stream, 500000, 1000000);
        print_measurement("IFGR=40 RFGR=5", middle);

        require_gain(device, "IFGR", 20.0);
        require_gain(device, "RFGR", 0.0);
        const Measurement high = measure(device, stream, 500000, 1000000);
        print_measurement("IFGR=20 RFGR=0", high);

        if (!(low.rms < middle.rms && middle.rms < high.rms))
            throw std::runtime_error("measured signal level was not monotonic across gain states");
        if (high.rms < low.rms * 2.0)
            throw std::runtime_error("gain endpoints changed measured RMS by less than 6 dB");

        device->setGainMode(SOAPY_SDR_RX, 0, true);
        if (!device->getGainMode(SOAPY_SDR_RX, 0))
            throw std::runtime_error("AGC remained disabled after automatic-mode request");
        const Measurement agc = measure(device, stream, 250000, 500000);
        print_measurement("AGC restored", agc);
        std::cout << "PASS: Soapy manual IFGR/RFGR changed physical sample levels; "
                     "streaming continued and AGC was restored\n";

        device->deactivateStream(stream);
        active = false;
        device->closeStream(stream);
        stream = nullptr;
        SoapySDR::Device::unmake(device);
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        if (device != nullptr && controls_changed) {
            try {
                device->setSampleRate(SOAPY_SDR_RX, 0, kSampleRate);
                device->setGainMode(SOAPY_SDR_RX, 0, true);
            } catch (const std::exception &cleanup_error) {
                std::cerr << "cleanup warning: could not restore 2 MS/s and AGC: "
                          << cleanup_error.what() << '\n';
            }
        }
        if (device != nullptr && stream != nullptr) {
            if (active) (void)device->deactivateStream(stream);
            device->closeStream(stream);
        }
        if (device != nullptr) SoapySDR::Device::unmake(device);
        return EXIT_FAILURE;
    }
}
