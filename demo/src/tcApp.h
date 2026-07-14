#pragma once

#include <TrussC.h>
using namespace std;
using namespace tc;

// Demo installation for anchorbolt development: publishes custom ops status
// (string status, numeric graphs, a fake camera image) via the mcp::status /
// mcp::graph / mcp::statusImage convention, and draws an animated scene so
// thumbnails visibly change.
class tcApp : public App {
public:
    void setup() override;
    void update() override;
    void draw() override;

private:
    Pixels makeCamFrame();

    string sceneName = "intro";
    double visitors = 20.0;   // random-walk fake visitor count
    double load = 0.0;        // smooth fake load metric
};
