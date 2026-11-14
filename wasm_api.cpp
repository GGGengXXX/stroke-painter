// WebAssembly entry point: the same painter as the server, callable from JavaScript.
//   paint_json(rgb, w, h, rounds, detail) -> malloc'd UTF-8 JSON string (caller frees with _free)
#include "painter.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <emscripten/emscripten.h>

static void appendNum(std::string& o, double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    o += buf;
}

static std::string toJson(const std::vector<Stroke>& strokes, int w, int h) {
    std::string o;
    o.reserve(strokes.size() * 120);
    o += "{\"width\":" + std::to_string(w) + ",\"height\":" + std::to_string(h) + ",\"strokes\":[";
    for (size_t i = 0; i < strokes.size(); ++i) {
        const Stroke& s = strokes[i];
        if (i) o += ',';
        o += "{\"round\":" + std::to_string(s.round) + ",\"radius\":";
        appendNum(o, s.radius);
        o += ",\"color\":[" + std::to_string((int)s.color.r) + "," + std::to_string((int)s.color.g) + ","
           + std::to_string((int)s.color.b) + "],\"pts\":[";
        for (size_t j = 0; j < s.pts.size(); ++j) {
            if (j) o += ',';
            o += '[';
            appendNum(o, s.pts[j].first);
            o += ',';
            appendNum(o, s.pts[j].second);
            o += ']';
        }
        o += "]}";
    }
    o += "]}";
    return o;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
char* paint_json(const uint8_t* rgb, int w, int h, int rounds, int detail) {
    Image img;
    img.w = w;
    img.h = h;
    img.px.resize((size_t)w * h);
    for (size_t i = 0; i < (size_t)w * h; ++i)
        img.px[i] = {(float)rgb[3 * i], (float)rgb[3 * i + 1], (float)rgb[3 * i + 2]};

    PainterParams P = makeParams(w, h, detail, rounds);
    std::string json = toJson(paint(img, P), w, h);

    char* out = (char*)std::malloc(json.size() + 1);
    std::memcpy(out, json.c_str(), json.size() + 1);
    return out;
}

}  // extern "C"
