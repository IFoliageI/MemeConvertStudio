#include "../src/api_probe.hpp"
#include <thread>
#include <chrono>

namespace studio {
int parameterProbeTest(const fs::path& directory, const std::wstring& base) {
    std::string log;
    int failures = 0;
    auto test = [&](const char* name, auto action) {
        try { action(); log += std::string("PASS ") + name + "\n"; }
        catch (const std::exception& error) { log += std::string("FAIL ") + name + ": " + error.what() + "\n"; ++failures; }
    };
    auto expect = [](bool value, const char* message) { if (!value) throw Error(message); };
    auto options = [&](const wchar_t* route, bool generation = false) {
        ProbeOptions value; value.provider.baseUrl = base + route;
        value.provider.key = L"probe-test-secret"; value.provider.model = "gpt-image-probe";
        value.provider.quality.clear(); value.provider.size.clear(); value.provider.timeoutSeconds = 5;
        value.generation = generation;
        if (!generation) value.images = probeImages();
        value.cases = {parameterProbeCases(value.provider, generation).front()};
        return value;
    };
    auto add = [&](ProbeOptions& value, const char* name, const Json& fields) {
        value.cases.push_back(customProbeCase(value.provider, {{"name", name}, {"parameters", fields}}));
    };
    test("Edit parameter isolation, combinations, ignored flags, SSE and secret redaction", [&] {
        auto value = options(L"/matrix/v1");
        add(value, "quality", {{"quality", "high"}});
        add(value, "complexity", {{"complexity", "high"}});
        add(value, "sse", {{"stream", true}, {"partial_images", 1}});
        add(value, "format", {{"output_format", "jpeg"}});
        add(value, "combination", {{"quality", "low"}, {"output_format", "png"}});
        add(value, "fallback", {{"stream", true}, {"mock_mode", "json"}});
        Cancel cancel;
        auto report = runParameterProbe(value, cancel);
        writeJson(directory / L"matrix.json", report);
        auto& rows = report["results"];
        expect(report["sent_requests"] == 7, "Unexpected request count or automatic retry");
        expect(rows[0]["outcome"] == "accepted" && rows[1]["outcome"] == "accepted", "Valid image rejected");
        expect(rows[2]["outcome"] == "rejected" && rows[2]["status"] == 400, "Explicit parameter rejection lost");
        expect(rows[3]["outcome"] == "accepted" && rows[3]["streamed"] == true &&
            rows[3]["partial_images_received"] == 1, "SSE evidence missing");
        expect(rows[4]["outcome"] == "ignored", "Output format mismatch accepted");
        expect(rows[5]["outcome"] == "rejected", "Combination conflict not captured");
        expect(rows[6]["outcome"] == "ignored", "Ignored streaming flag accepted");
        expect(report.dump().find("probe-test-secret") == std::string::npos &&
            report.dump().find("[REDACTED]") != std::string::npos, "Report leaked API key");
    });
    test("Generation JSON has no image upload", [&] {
        auto value = options(L"/generation/v1", true); add(value, "quality", {{"quality", "low"}});
        Cancel cancel; auto report = runParameterProbe(value, cancel);
        expect(report["sent_requests"] == 2 && report["results"][1]["outcome"] == "accepted", "Generation failed");
    });
    test("Failed baseline and rate limiting stop subsequent requests", [&] {
        for (auto route : {L"/denied/v1", L"/malformed/v1", L"/corrupt/v1", L"/limited/v1"}) {
            auto value = options(route);
            add(value, "quality", {{"quality", "low"}}); add(value, "size", {{"size", "auto"}});
            Cancel cancel; auto report = runParameterProbe(value, cancel);
            int expected = std::wstring(route) == L"/limited/v1" ? 2 : 1;
            expect(report["sent_requests"] == expected && report["results"][2]["outcome"] == "skipped",
                "Failed baseline or rate limit continued sending");
        }
    });
    test("Invalid custom combinations rejected before network", [&] {
        auto value = options(L"/unused/v1");
        bool rejected = false;
        try { add(value, "invalid", {{"output_format", "png"}, {"output_compression", 80}}); }
        catch (const std::exception&) { rejected = true; }
        expect(rejected, "Invalid combination accepted");
    });
    test("Cancellation stops active request and retains report", [&] {
        auto value = options(L"/slow/v1"); add(value, "quality", {{"quality", "low"}});
        Cancel cancel;
        std::thread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(250)); cancel.stop(); });
        Json report;
        try { report = runParameterProbe(value, cancel); } catch (...) { stopper.join(); throw; }
        stopper.join();
        expect(report["cancelled"] == true && report["sent_requests"] == 1 &&
            report["results"][1]["outcome"] == "skipped", "Cancellation report lost or subsequent request sent");
    });
    fs::create_directories(directory);
    writeFile(directory / L"parameter-probe-results.txt", Bytes(log.begin(), log.end()));
    return failures ? 1 : 0;
}
}
