// Auto Input by pulidoxgd
//
// Reactive bot: reads level hitboxes + the player's hitbox every frame and
// decides whether to click, hold or spam. It is a heuristic (not a full
// physics simulation), so tune "Reaction distance" if it jumps early/late.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace ai {

    struct Obstacle {
        float x0, y0, x1, y1;
        bool hazard; // true = spike/saw, false = solid block
    };

    struct Cfg {
        bool enabled;
        bool full;
        bool everything;
        float range;
        bool autoTrigger;
        float trigger;
        double delay; // seconds
        float margin;
    };

    enum class GM { Cube, Ball, Ship, Ufo, Wave };

    // ---------------------------------------------------------------- state
    static std::vector<Obstacle> s_cache;   // Full mode: every hitbox, sorted by x0
    static float s_maxW = 0.f;
    static bool s_cacheReady = false;
    static bool s_cacheEverything = false;

    static std::vector<Obstacle> s_near;    // Performance mode: rolling window
    static bool s_nearEverything = false;
    static bool s_nearValid = false;

    static std::vector<Obstacle> s_window;  // obstacles used this frame

    static std::deque<std::pair<double, bool>> s_queue; // delayed inputs
    static double s_clock = 0.0;
    static int s_frame = 0;
    static bool s_down = false;      // what we are really sending
    static bool s_want = false;      // what we currently want (before delay)
    static bool s_used = false;      // did we click during this attempt?
    static float s_speed = 311.58f;  // measured units/second
    static float s_lastX = -1.f;
    static float s_gLo = 0.f, s_gHi = 30.f; // player's y-band the last time it was grounded
    static bool s_haveBand = false;
    static double s_pulseEnd = 0.0;
    static double s_lastPulse = -100.0;

    // ------------------------------------------------------------- settings
    static Cfg readCfg() {
        auto m = Mod::get();
        Cfg c;
        c.enabled = m->getSettingValue<bool>("enabled");
        c.full = m->getSettingValue<std::string>("mode") == "Full level";
        c.everything = m->getSettingValue<std::string>("detect") == "Everything";
        c.range = static_cast<float>(m->getSettingValue<double>("detect-range"));
        c.autoTrigger = m->getSettingValue<bool>("auto-trigger");
        c.trigger = static_cast<float>(m->getSettingValue<double>("trigger-distance"));
        c.delay = static_cast<double>(m->getSettingValue<int64_t>("reaction-ms")) / 1000.0;
        c.margin = static_cast<float>(m->getSettingValue<double>("margin"));
        return c;
    }

    // ------------------------------------------------------------ hitboxes
    static bool classify(GameObject* o, bool everything, Obstacle& out) {
        if (!o) return false;
        auto t = o->m_objectType;
        bool haz = t == GameObjectType::Hazard;
        bool sol = t == GameObjectType::Solid;
        if (everything) {
            sol = sol || t == GameObjectType::Slope || t == GameObjectType::Breakable;
        }
        if (!haz && !sol) return false;
        CCRect r = o->getObjectRect();
        out = Obstacle{r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY(), haz};
        return true;
    }

    static void buildCache(GJBaseGameLayer* gl, bool everything) {
        s_cache.clear();
        s_maxW = 0.f;
        if (gl->m_objects) {
            for (auto* o : CCArrayExt<GameObject*>(gl->m_objects)) {
                Obstacle ob;
                if (classify(o, everything, ob)) {
                    s_maxW = std::max(s_maxW, ob.x1 - ob.x0);
                    s_cache.push_back(ob);
                }
            }
        }
        std::sort(s_cache.begin(), s_cache.end(), [](const Obstacle& a, const Obstacle& b) {
            return a.x0 < b.x0;
        });
        s_cacheEverything = everything;
        s_cacheReady = true;
    }

    static void rescanNear(GJBaseGameLayer* gl, bool everything, float minX, float maxX) {
        s_near.clear();
        if (gl->m_objects) {
            float lo = minX - 80.f;
            float hi = maxX + s_speed * 0.25f + 120.f;
            for (auto* o : CCArrayExt<GameObject*>(gl->m_objects)) {
                Obstacle ob;
                if (classify(o, everything, ob) && ob.x1 >= lo && ob.x0 <= hi) {
                    s_near.push_back(ob);
                }
            }
        }
        s_nearEverything = everything;
        s_nearValid = true;
    }

    static void collect(GJBaseGameLayer* gl, const Cfg& c, float minX, float maxX) {
        s_window.clear();
        if (c.full) {
            if (!s_cacheReady || s_cacheEverything != c.everything) buildCache(gl, c.everything);
            float from = minX - s_maxW;
            auto it = std::lower_bound(
                s_cache.begin(), s_cache.end(), from,
                [](const Obstacle& o, float v) { return o.x0 < v; }
            );
            for (; it != s_cache.end() && it->x0 <= maxX; ++it) {
                if (it->x1 >= minX) s_window.push_back(*it);
            }
        } else {
            if (!s_nearValid || s_nearEverything != c.everything || s_frame % 8 == 0) {
                rescanNear(gl, c.everything, minX, maxX);
            }
            for (auto& o : s_near) {
                if (o.x1 >= minX && o.x0 <= maxX) s_window.push_back(o);
            }
        }
    }

    // --------------------------------------------------------------- input
    static void sendButton(GJBaseGameLayer* gl, bool down) {
        // If this line fails to compile on your SDK, replace it with:
        //   down ? gl->pushButton(PlayerButton::Jump, true) : gl->releaseButton(PlayerButton::Jump, true);
        gl->handleButton(down, 1, true);
    }

    static void apply(GJBaseGameLayer* gl, bool down) {
        if (down == s_down) return;
        s_down = down;
        if (down) s_used = true;
        sendButton(gl, down);
    }

    static void releaseAll(GJBaseGameLayer* gl) {
        s_queue.clear();
        s_want = false;
        if (s_down) apply(gl, false);
    }

    // short click used for ball / ufo / spider ("spam")
    static bool pulse(float cooldown) {
        if (s_clock - s_lastPulse >= cooldown) {
            s_lastPulse = s_clock;
            s_pulseEnd = s_clock + 0.04;
        }
        return s_clock < s_pulseEnd;
    }

    // -------------------------------------------------------------- planner
    static GM gamemodeOf(PlayerObject* p) {
        if (p->m_isShip) return GM::Ship;
        if (p->m_isBird) return GM::Ufo;
        if (p->m_isDart) return GM::Wave;
        if (p->m_isBall || p->m_isSpider) return GM::Ball;
        return GM::Cube;
    }

    static float autoTrigger(GM gm, float speed) {
        switch (gm) {
            case GM::Ship:
            case GM::Ufo:
            case GM::Wave: return std::clamp(speed * 0.30f, 40.f, 260.f);
            case GM::Ball: return std::clamp(speed * 0.12f, 18.f, 150.f);
            default:       return std::clamp(speed * 0.11f, 18.f, 140.f);
        }
    }

    // Cube / robot / ball / spider: is there something we must jump over?
    static bool threatAhead(PlayerObject* p, const CCRect& pr, float trig, float margin) {
        float front = pr.getMaxX();
        float h = pr.size.height;
        float jumpMax = h * 2.2f;
        bool flip = p->m_isUpsideDown;

        for (auto& o : s_window) {
            if (o.x1 + margin < pr.getMinX()) continue;       // already behind us
            if (o.x0 - margin > front + trig) continue;       // still too far

            if (o.hazard) {
                if (o.y1 + margin > s_gLo && o.y0 - margin < s_gHi) return true;
            } else {
                float rise;
                bool overlapsBand;
                if (!flip) {
                    rise = o.y1 - s_gLo;
                    overlapsBand = o.y0 < s_gHi && o.y1 > s_gLo + 4.f;
                } else {
                    rise = s_gHi - o.y0;
                    overlapsBand = o.y1 > s_gLo && o.y0 < s_gHi - 4.f;
                }
                if (overlapsBand && rise > 4.f && rise <= jumpMax) return true;
            }
        }
        return false;
    }

    // Ship / ufo / wave: pick a safe y to fly towards.
    static float planTarget(const CCRect& pr, float trig, float margin) {
        float py = pr.getMidY();
        float h = pr.size.height;
        float front = pr.getMaxX();
        const float band = 150.f;

        std::vector<std::pair<float, float>> blocked;
        for (auto& o : s_window) {
            if (o.x1 + margin < pr.getMinX()) continue;
            if (o.x0 - margin > front + trig) continue;
            if (o.y1 + margin < py - band || o.y0 - margin > py + band) continue;
            blocked.emplace_back(o.y0 - margin, o.y1 + margin);
        }
        if (blocked.empty()) return py; // nothing around: hold the line

        std::sort(blocked.begin(), blocked.end());

        struct Gap { float a, b; bool openLo, openHi; };
        std::vector<Gap> gaps;
        float cursor = py - band;
        bool firstOpen = true;
        for (auto& iv : blocked) {
            if (iv.first > cursor) {
                gaps.push_back({cursor, iv.first, firstOpen, false});
            }
            cursor = std::max(cursor, iv.second);
            firstOpen = false;
        }
        if (cursor < py + band) gaps.push_back({cursor, py + band, false, true});
        if (gaps.empty()) return py;

        const float pad = 6.f;
        auto centerOf = [&](const Gap& g) {
            if (g.openLo && g.openHi) return py;
            if (g.openLo) return g.b - (h * 0.5f + pad);
            if (g.openHi) return g.a + (h * 0.5f + pad);
            return (g.a + g.b) * 0.5f;
        };
        auto fits = [&](const Gap& g) {
            return g.openLo || g.openHi || (g.b - g.a) >= h + 2.f;
        };

        float best = py;
        float bestDist = 1e9f;
        bool found = false;
        for (auto& g : gaps) {
            if (!fits(g)) continue;
            float c = centerOf(g);
            float d = std::fabs(c - py);
            if (d < bestDist) { bestDist = d; best = c; found = true; }
        }
        if (!found) { // nothing fits: use the widest gap
            float widest = -1.f;
            for (auto& g : gaps) {
                float w = g.b - g.a;
                if (w > widest) { widest = w; best = (g.a + g.b) * 0.5f; }
            }
        }
        return best;
    }

    static bool decide(PlayerObject* p, const CCRect& pr, GM gm, float trig, float margin) {
        bool onGround = p->m_isOnGround;

        if (gm == GM::Cube) {
            // click to jump, keep holding while hazards are still under/ahead
            bool threat = threatAhead(p, pr, trig, margin);
            return threat && (onGround || s_want);
        }

        if (gm == GM::Ball) {
            bool threat = threatAhead(p, pr, trig, margin);
            if (threat && onGround) return pulse(0.25f);
            return s_clock < s_pulseEnd;
        }

        // Ship / ufo / wave: steer towards the safe corridor
        float target = planTarget(pr, trig * 1.5f, margin);
        float py = pr.getMidY();
        float dir = p->m_isUpsideDown ? -1.f : 1.f; // direction the button pushes
        float err = (target - py) * dir;
        float dead = pr.size.height * 0.2f;

        if (gm == GM::Ufo) {
            if (err > dead) return pulse(0.10f);
            return s_clock < s_pulseEnd;
        }

        // ship & wave: hold / release (this naturally becomes spam in tight spots)
        if (err > dead) return true;
        if (err < -dead) return false;
        return s_want;
    }

    // ----------------------------------------------------------------- tick
    static void tick(GJBaseGameLayer* gl, float dt) {
        auto* pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != gl) return;

        Cfg c = readCfg();
        auto* p = gl->m_player1;
        if (!c.enabled || !p || p->m_isDead) {
            releaseAll(gl);
            return;
        }

        s_frame++;
        s_clock += dt;

        // player hitbox (follows size, vehicle, mini, etc.)
        CCRect pr = p->getObjectRect();
        float px = p->getPositionX();

        // measured speed
        if (dt > 0.f && s_lastX >= 0.f && px > s_lastX) {
            float v = (px - s_lastX) / dt;
            if (v > 100.f && v < 900.f) s_speed = s_speed * 0.8f + v * 0.2f;
        }
        s_lastX = px;

        // remember the y-band where the player stands
        if (p->m_isOnGround || !s_haveBand) {
            s_gLo = pr.getMinY();
            s_gHi = pr.getMaxY();
            s_haveBand = true;
        }

        GM gm = gamemodeOf(p);
        float trig = c.autoTrigger ? autoTrigger(gm, s_speed) : c.trigger;
        float range = std::max(c.range, trig);

        collect(gl, c, pr.getMinX() - 40.f, pr.getMaxX() + range);

        bool want = decide(p, pr, gm, std::min(trig, range), c.margin);

        if (want != s_want) {
            s_want = want;
            s_queue.emplace_back(s_clock + c.delay, want);
        }
        while (!s_queue.empty() && s_queue.front().first <= s_clock) {
            apply(gl, s_queue.front().second);
            s_queue.pop_front();
        }
    }

    static void onLevelStart() {
        s_cache.clear();
        s_cacheReady = false;
        s_near.clear();
        s_nearValid = false;
        s_maxW = 0.f;
        s_speed = 311.58f;
        s_lastX = -1.f;
        s_haveBand = false;
    }

    static void onReset() {
        s_queue.clear();
        s_clock = 0.0;
        s_frame = 0;
        s_down = false;
        s_want = false;
        s_used = false;
        s_lastX = -1.f;
        s_haveBand = false;
        s_pulseEnd = 0.0;
        s_lastPulse = -100.0;
    }
}

// ------------------------------------------------------------------- hooks
class $modify(AIGameLayer, GJBaseGameLayer) {
    void processCommands(float dt) {
        ai::tick(this, dt);
        GJBaseGameLayer::processCommands(dt);
    }
};

class $modify(AIPlayLayer, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        ai::onLevelStart();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        ai::onReset();
    }

    void levelComplete() {
        // Completions made with Auto Input are not saved (acts like a test run).
        if (ai::s_used && Mod::get()->getSettingValue<bool>("no-record")) {
            m_isTestMode = true;
        }
        PlayLayer::levelComplete();
    }
};
