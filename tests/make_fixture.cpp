#include <filesystem>
#include <iostream>
#include <vector>

#include <metavision/sdk/base/events/event_cd.h>
#include <metavision/sdk/stream/raw_evt2_event_file_writer.h>

// A one-second recording for SDK replay tests. Each complete 20 ms window has
// three events: a left-edge event, a linearly split ON event, and an OFF event
// at the final bin center. The last event closes window 49 and starts a tail
// window, which must not be published as a complete tensor at EOF.
int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: make_fixture OUTPUT.raw\n";
        return 2;
    }

    std::vector<Metavision::EventCD> events;
    for (int window = 0; window < 50; ++window) {
        const Metavision::timestamp start = window * 20000;
        events.emplace_back(window, 0, window % 2, start);
        events.emplace_back(100, 50, 1, start + 8000);
        events.emplace_back(1279, 719, 0, start + 18000);
    }
    events.emplace_back(50, 0, 0, 1000000);

    Metavision::RAWEvt2EventFileWriter writer(1280, 720, std::filesystem::path(argv[1]));
    writer.add_events(events.data(), events.data() + events.size());
    writer.close();
    std::cout << "Wrote " << events.size() << " events, 50 complete 20 ms windows, and one tail event to "
              << argv[1] << '\n';
}
