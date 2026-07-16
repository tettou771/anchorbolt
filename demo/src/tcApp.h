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

    // Mouse handlers so remote control (live-view input injection) is visible:
    // the cursor leaves a fading trail and clicks drop a ripple.
    void mouseMoved(Vec2 pos) override;
    void mouseDragged(Vec2 pos, int button) override;
    void mousePressed(Vec2 pos, int button) override;

private:
    Pixels makeCamFrame();

    string sceneName = "intro";
    double visitors = 20.0;   // random-walk fake visitor count
    double load = 0.0;        // smooth fake load metric
    bool frozen = false;      // set by the 'freeze' MCP tool (hang-test)

    Vec2 cursor;                       // last pointer position
    vector<Vec2> trail;                // recent cursor path (fades out)
    struct Ripple { Vec2 p; float t; };
    vector<Ripple> ripples;            // click markers
};
