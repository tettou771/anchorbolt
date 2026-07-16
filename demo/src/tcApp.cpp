#include "tcApp.h"

void tcApp::setup() {
    // The whole anchorbolt integration is these four lines — the supervisor
    // discovers tc_get_status via tools/list, no config anywhere else.
    mcp::status("scene",           [this] { return sceneName; });
    mcp::statusGraph("visitors",   [this] { return visitors; });
    mcp::statusGraph("load",       [this] { return load; });
    mcp::statusImage("entranceCam", [this] { return makeCamFrame(); });

    // Opt in to input injection (tc_mouse_click/press/move/release, tc_key_press)
    // so the fleet dashboard's live-view remote control has something to drive.
    // A real venue app enables this only when it wants to be remotely operated.
    mcp::registerDebuggerTools();

    // App-specific tool (unprefixed by convention — tc_/tcx_ belong to the
    // framework). Hangs the main loop so supervisor hang-detection can be
    // tested against a real freeze.
    mcp::tool("freeze", "Hang the main loop forever (supervisor hang-detection test)")
        .bind(std::function<Json()>([this]() -> Json {
            frozen = true;
            return Json{{"status", "ok"}, {"message", "freezing after this reply"}};
        }));

    // Exercises mcp::alert -> tc_get_alerts -> supervisor notification sinks.
    mcp::tool("raise_alert", "Raise a test operator alert (mcp::alert demo)")
        .bind(std::function<Json()>([]() -> Json {
            mcp::alert("test alert raised from the demo app");
            return Json{{"status", "ok"}};
        }));
}

void tcApp::update() {
    if (frozen) {
        while (true) this_thread::sleep_for(chrono::hours(1));  // simulate a hang
    }
    double t = getElapsedTime();
    // Deterministic fake metrics that still look alive on a graph.
    visitors = 50.0 + 30.0 * sin(t * 0.11) + 10.0 * sin(t * 0.47);
    load     = 0.5 + 0.35 * sin(t * 0.23) + 0.15 * sin(t * 1.31);
    const char* scenes[] = {"intro", "main", "finale"};
    sceneName = scenes[int(t / 20.0) % 3];
}

void tcApp::draw() {
    double t = getElapsedTime();
    clear(Color::fromHSB(fmod(t * 0.01, 1.0), 0.5, 0.18));

    // Orbiting circles; radius follows the fake load metric.
    translate(getWidth() / 2.0f, getHeight() / 2.0f);
    int n = 10;
    for (int i = 0; i < n; ++i) {
        double a = t * 0.4 + TAU * i / n;
        setColor(Color::fromHSB(fmod(t * 0.01 + (float)i / n, 1.0f), 0.7f, 0.9f));
        drawCircle(cos(a) * 180.0f, sin(a) * 180.0f,
                   14.0f + 24.0f * (float)load * (0.5f + 0.5f * (float)sin(t * 2.0 + i)));
    }

    setColor(1.0f);
    drawBitmapString("anchorbolt demo / scene: " + sceneName,
                     -getWidth() / 2.0f + 20, -getHeight() / 2.0f + 30);
}

// Fake "entrance camera": a slow plasma so the pushed image visibly changes.
// Stands in for real Pixels from a VideoGrabber / webcam.
Pixels tcApp::makeCamFrame() {
    const int w = 320, h = 240;
    Pixels px;
    px.allocate(w, h, 3);
    unsigned char* d = px.getData();
    double t = getElapsedTime();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double v = sin(x * 0.045 + t) + sin(y * 0.07 - t * 1.3)
                     + sin((x + y) * 0.025 + t * 0.7);
            unsigned char* p = d + (size_t(y) * w + x) * 3;
            p[0] = (unsigned char)(128 + 100 * sin(v));
            p[1] = (unsigned char)(128 + 100 * sin(v + TAU / 3));
            p[2] = (unsigned char)(128 + 100 * sin(v + 2 * TAU / 3));
        }
    }
    return px;
}
