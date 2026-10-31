// A font face's first use, paid early and in slices.
//
// A face's first layout loads it (the fontconfig match, the FreeType face,
// the fallback fontset): ~10 ms per weight and size, more for a first
// emoji, all inside one loop turn if a real warm paid it. Each face loads on
// an event-loop turn of its own, a beat after startup, into the per-thread
// pango font map every text raster on this thread shares (the cards'
// layouts and the renderer's renderText alike).
#pragma once

#include "config.hpp"
#include "text.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <pango/pangocairo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace NAwesome {

    class CFontWarm {
      public:
        static CFontWarm& inst() {
            static CFontWarm W;
            return W;
        }

        void start() {
            stop();
            m_timer = makeShared<CEventLoopTimer>(START, [this](SP<CEventLoopTimer>, void*) { step(); }, nullptr);
            g_pEventLoopManager->addTimer(m_timer);
        }

        void stop() {
            if (m_timer && g_pEventLoopManager)
                g_pEventLoopManager->removeTimer(m_timer);
            m_timer.reset();
            m_faces.clear();
            m_next = 0;
        }

      private:
        struct SFace {
            std::string family;
            int         weight = 400;
            int         px     = 13;
            const char* sample = "Ag";
        };

        static constexpr auto START = std::chrono::milliseconds(1500); // past the startup warms
        static constexpr auto STEP  = std::chrono::milliseconds(40);

        // the faces the boards draw: both modules' families at the weights
        // and type roles they use, at every monitor's scale, and the emoji
        // fallback a chat's first message reaches for
        void collect() {
            std::set<std::tuple<std::string, int, int, std::string_view>> seen; // the sample too: an emoji's face is its fallback font
            std::vector<double>                         scales;
            for (const auto& M : State::monitorState()->monitors())
                scales.push_back(M->m_scale);
            if (scales.empty())
                scales.push_back(1.0);
            const auto ADD = [&](const std::string& family, int weight, int logical, const char* sample) {
                for (const double S : scales) {
                    const int PX = std::max(1, (int)std::lround(logical * S));
                    if (seen.emplace(family, weight, PX, sample).second)
                        m_faces.push_back({family, weight, PX, sample});
                }
            };
            const std::pair<std::string, int> MODULES[] = {{cfg().getS("plugin:awesome:shell:font"), cfg().getI("plugin:awesome:shell:font_size")},
                                                           {cfg().getS("plugin:awesome:notify:font"), cfg().getI("plugin:awesome:notify:font_size")}};
            for (const auto& [FAMILY, SIZE] : MODULES) {
                for (const int W : {400, 500, 600, 700})
                    for (const int D : {-2, -1, 0, 2})
                        ADD(FAMILY, W, SIZE + D, "Ag");
                ADD(FAMILY, 400, SIZE, "\xF0\x9F\x98\x80"); // U+1F600
            }
            // the kanji tags: the shell's CJK face, at both tag weights
            for (const int W : {400, 600})
                ADD(MODULES[0].first, W, MODULES[0].second, "\xE4\xB8\x80"); // 一
        }

        void step() {
            if (!m_timer)
                return;
            if (m_next == 0 && m_faces.empty())
                collect();
            else if (m_faces.empty())
                return;
            if (m_next >= m_faces.size()) {
                m_faces.clear(); // done: the timer stays disarmed until stop()
                m_faces.shrink_to_fit();
                return;
            }
            load(m_faces[m_next++]);
            m_timer->updateTimeout(STEP);
        }

        static void load(const SFace& f) {
            auto* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
            auto* cr   = cairo_create(surf);
            auto* lay  = pango_cairo_create_layout(cr);
            applyTextOptions(lay); // the scaled fonts the rasters will ask for
            auto* fd   = pango_font_description_new();
            pango_font_description_set_family(fd, f.family.c_str());
            pango_font_description_set_weight(fd, (PangoWeight)f.weight);
            pango_font_description_set_absolute_size(fd, f.px * PANGO_SCALE);
            pango_layout_set_font_description(lay, fd);
            pango_layout_set_text(lay, f.sample, -1);
            pango_cairo_show_layout(cr, lay); // the glyphs too, not just the match
            pango_font_description_free(fd);
            g_object_unref(lay);
            cairo_destroy(cr);
            cairo_surface_destroy(surf);
        }

        SP<CEventLoopTimer> m_timer;
        std::vector<SFace>  m_faces;
        size_t              m_next = 0;
    };

} // namespace NAwesome
