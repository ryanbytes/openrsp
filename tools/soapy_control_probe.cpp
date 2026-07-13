#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
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

struct DualMeasurement {
    size_t samples[2] = {0, 0};
    long double power[2] = {0.0, 0.0};
    double elapsed = 0.0;
};

DualMeasurement measure_dual(SoapySDR::Device *device,
                             SoapySDR::Stream *streams[2])
{
    using clock = std::chrono::steady_clock;
    std::vector<std::complex<short>> buffers[2] = {
        std::vector<std::complex<short>>(device->getStreamMTU(streams[0])),
        std::vector<std::complex<short>>(device->getStreamMTU(streams[1]))
    };
    for (auto &buffer : buffers)
        if (buffer.empty()) buffer.resize(4096);

    DualMeasurement measurement;
    const auto started = clock::now();
    const auto deadline = started + std::chrono::seconds(3);
    unsigned int consecutive_timeouts[2] = {0u, 0u};
    while (clock::now() < deadline) {
        for (size_t channel = 0u; channel < 2u; ++channel) {
            void *output[] = {buffers[channel].data()};
            int flags = 0;
            long long time_ns = 0;
            const int result = device->readStream(
                streams[channel], output, buffers[channel].size(), flags,
                time_ns, 500000);
            if (result == SOAPY_SDR_TIMEOUT) {
                if (++consecutive_timeouts[channel] > 5u)
                    throw std::runtime_error("repeated dual-stream timeout on channel " +
                                             std::to_string(channel));
                continue;
            }
            if (result < 0)
                throw std::runtime_error("dual readStream channel " +
                                         std::to_string(channel) + ": " +
                                         SoapySDR::errToStr(result));
            consecutive_timeouts[channel] = 0u;
            measurement.samples[channel] += static_cast<size_t>(result);
            for (int index = 0; index < result; ++index) {
                const long double i = buffers[channel][index].real();
                const long double q = buffers[channel][index].imag();
                measurement.power[channel] += i * i + q * q;
            }
        }
    }
    measurement.elapsed = std::chrono::duration<double>(clock::now() - started).count();
    return measurement;
}

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
    SoapySDR::setLogLevel(SOAPY_SDR_WARNING);
    SoapySDR::Kwargs args;
    args["driver"] = "sdrplay";
    bool rate_matrix = false;
    bool dual_mode = false;
    double single_rate = 0.0;
    double frequency = kFrequency;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--rates") rate_matrix = true;
        else if (argument == "--dual") dual_mode = true;
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
        else if (argument == "--frequency" && index + 1 < argc) {
            try {
                frequency = std::stod(argv[++index]);
            } catch (const std::exception &) {
                std::cerr << "--frequency requires a numeric hertz value\n";
                return EXIT_FAILURE;
            }
            if (!std::isfinite(frequency) || frequency < 1000.0 ||
                frequency > 2000000000.0) {
                std::cerr << "--frequency must be between 1 kHz and 2 GHz\n";
                return EXIT_FAILURE;
            }
        }
        else if (argument == "--serial" && index + 1 < argc) args["serial"] = argv[++index];
        else {
            std::cerr << "usage: " << argv[0]
                      << " [--serial SERIAL] [--frequency HZ]"
                         " [--rates | --rate SPS] [--dual]\n";
            return EXIT_FAILURE;
        }
    }
    if (rate_matrix && single_rate != 0.0) {
        std::cerr << "--rates and --rate are mutually exclusive\n";
        return EXIT_FAILURE;
    }
    if (dual_mode && (rate_matrix || single_rate != 0.0)) {
        std::cerr << "--dual cannot be combined with --rates or --rate\n";
        return EXIT_FAILURE;
    }
    if (dual_mode) args["mode"] = "DT";

    SoapySDR::Device *device = nullptr;
    SoapySDR::Stream *stream = nullptr;
    SoapySDR::Stream *stream_b = nullptr;
    bool active = false;
    bool active_b = false;
    bool controls_changed = false;
    try {
        const SoapySDR::KwargsList devices = SoapySDR::Device::enumerate(args);
        if (devices.empty()) throw std::runtime_error("no Soapy SDRplay device found");
        device = SoapySDR::Device::make(devices.front());
        if (device == nullptr) throw std::runtime_error("Soapy device creation failed");

        if (dual_mode) {
            if (device->getNumChannels(SOAPY_SDR_RX) != 2u)
                throw std::runtime_error("dual RSPduo did not expose two RX channels");
            if (device->getSampleRate(SOAPY_SDR_RX, 0) != kSampleRate ||
                device->getSampleRate(SOAPY_SDR_RX, 1) != kSampleRate)
                throw std::runtime_error("dual RSPduo did not default to 2 MS/s per channel");
            stream = device->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, {0u});
            stream_b = device->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, {1u});
            if (stream == nullptr || stream_b == nullptr)
                throw std::runtime_error("dual setupStream returned null");
            if (device->activateStream(stream) != 0)
                throw std::runtime_error("dual activateStream failed");
            active = true;
            if (device->activateStream(stream_b) != 0)
                throw std::runtime_error("dual activateStream failed");
            active_b = true;
            const double frequencies[] = {frequency, frequency + 150000.0};
            const double ifgr[] = {35.0, 55.0};
            const double rfgr[] = {1.0, 5.0};
            for (size_t channel = 0u; channel < 2u; ++channel) {
                device->setGainMode(SOAPY_SDR_RX, channel, false);
                device->setFrequency(SOAPY_SDR_RX, channel,
                                     frequencies[channel]);
                device->setGain(SOAPY_SDR_RX, channel, "IFGR", ifgr[channel]);
                device->setGain(SOAPY_SDR_RX, channel, "RFGR", rfgr[channel]);
            }
            for (size_t channel = 0u; channel < 2u; ++channel) {
                if (device->getGainMode(SOAPY_SDR_RX, channel) ||
                    device->getFrequency(SOAPY_SDR_RX, channel) !=
                        frequencies[channel] ||
                    device->getGain(SOAPY_SDR_RX, channel, "IFGR") !=
                        ifgr[channel] ||
                    device->getGain(SOAPY_SDR_RX, channel, "RFGR") !=
                        rfgr[channel])
                    throw std::runtime_error(
                        "dual channel did not retain independent controls");
                std::cout << "dual controls channel=" << channel
                          << " frequency=" << frequencies[channel]
                          << " ifgr=" << ifgr[channel]
                          << " rfgr=" << rfgr[channel] << '\n';
            }
            SoapySDR::Stream *streams[] = {stream, stream_b};
            const DualMeasurement dual = measure_dual(device, streams);
            for (size_t channel = 0u; channel < 2u; ++channel) {
                if (dual.samples[channel] == 0u)
                    throw std::runtime_error("dual channel delivered no IQ");
                const double rate = static_cast<double>(dual.samples[channel]) /
                                    dual.elapsed;
                const double rms = std::sqrt(static_cast<double>(
                    dual.power[channel] / dual.samples[channel]));
                std::cout << "dual channel=" << channel
                          << " samples=" << dual.samples[channel]
                          << " rate=" << rate << " rms=" << rms << '\n';
                if (dual.samples[channel] < 1000000u || rms == 0.0)
                    throw std::runtime_error("dual channel did not deliver live IQ");
            }
            const double balance = static_cast<double>(dual.samples[0]) /
                                   static_cast<double>(dual.samples[1]);
            if (balance < 0.95 || balance > 1.05)
                throw std::runtime_error("dual channel delivery differed by more than 5%");
            device->deactivateStream(stream);
            active = false;
            device->deactivateStream(stream_b);
            active_b = false;
            device->closeStream(stream);
            stream = nullptr;
            device->closeStream(stream_b);
            stream_b = nullptr;
            SoapySDR::Device::unmake(device);
            std::cout << "PASS: Soapy dual mode delivered concurrent A/B IQ\n";
            return EXIT_SUCCESS;
        }

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
        device->setFrequency(SOAPY_SDR_RX, 0, frequency);
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
        if (device != nullptr && stream_b != nullptr) {
            if (active_b) (void)device->deactivateStream(stream_b);
            device->closeStream(stream_b);
        }
        if (device != nullptr) SoapySDR::Device::unmake(device);
        return EXIT_FAILURE;
    }
}
