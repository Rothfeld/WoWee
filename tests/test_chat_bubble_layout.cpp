// Speech bubble color, font pixel height, wrap width, and 9-slice/tail layout
// parity against TBC 2.4.3 (build 8606).
#include "catch_amalgamated.hpp"
#include "frizqt_metrics_fixture.hpp"
#include "ui/chat/chat_bubble_layout.hpp"
#include "ui/text_wrap.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::shared_ptr<wowee::ui::NativeFace> makeFrizqtFace() {
    std::unordered_map<uint32_t, wowee::ui::NativeGlyph> glyphs;
    for (const auto& g : frizqt_fixture::kGlyphs) {
        wowee::ui::NativeGlyph ng;
        ng.present = true;
        ng.outline = g.outline;
        ng.advance = g.advance;
        ng.lsb = g.lsb;
        ng.xMax = g.xMax;
        glyphs.emplace(g.cp, ng);
    }
    std::unordered_map<uint64_t, int> kerning;
    for (const auto& k : frizqt_fixture::kKerning) {
        kerning.emplace((static_cast<uint64_t>(k.left) << 32) | k.right, k.value);
    }
    return std::make_shared<wowee::ui::NativeFace>(
        frizqt_fixture::kUnitsPerEm, std::move(glyphs), std::move(kerning));
}

std::string joinTrimmed(const wowee::ui::NativeWrappedLine& line) {
    std::string out;
    for (const auto& r : line.runs) out += r.text;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

} // namespace

TEST_CASE("Speech bubble text colors match TBC 2.4.3 ChatTypeInfo", "[chat_bubble][color]") {
    using wowee::game::ChatType;
    using wowee::ui::chatBubbleColor;

    const auto monsterSay = chatBubbleColor(ChatType::MONSTER_SAY);
    CHECK(monsterSay.r == Catch::Approx(1.0f));
    CHECK(monsterSay.g == Catch::Approx(1.0f));
    CHECK(monsterSay.b == Catch::Approx(159.0f / 255.0f).margin(0.002f));

    const auto monsterYell = chatBubbleColor(ChatType::MONSTER_YELL);
    CHECK(monsterYell.r == Catch::Approx(1.0f));
    CHECK(monsterYell.g == Catch::Approx(64.0f / 255.0f).margin(0.002f));
    CHECK(monsterYell.b == Catch::Approx(64.0f / 255.0f).margin(0.002f));

    const auto playerSay = chatBubbleColor(ChatType::SAY);
    CHECK(playerSay.r == Catch::Approx(1.0f));
    CHECK(playerSay.g == Catch::Approx(1.0f));
    CHECK(playerSay.b == Catch::Approx(1.0f));

    const auto playerYell = chatBubbleColor(ChatType::YELL);
    CHECK(playerYell.r == Catch::Approx(1.0f));
    CHECK(playerYell.g == Catch::Approx(64.0f / 255.0f).margin(0.002f));
    CHECK(playerYell.b == Catch::Approx(64.0f / 255.0f).margin(0.002f));

    const auto party = chatBubbleColor(ChatType::PARTY);
    CHECK(party.r == Catch::Approx(170.0f / 255.0f).margin(0.002f));
    CHECK(party.g == Catch::Approx(170.0f / 255.0f).margin(0.002f));
    CHECK(party.b == Catch::Approx(1.0f));

    const auto monsterParty = chatBubbleColor(ChatType::MONSTER_PARTY);
    CHECK(monsterParty.r == Catch::Approx(170.0f / 255.0f).margin(0.002f));
    CHECK(monsterParty.g == Catch::Approx(170.0f / 255.0f).margin(0.002f));
    CHECK(monsterParty.b == Catch::Approx(1.0f));

    CHECK_FALSE(wowee::ui::isPartyChatBubbleType(ChatType::SAY));
    CHECK_FALSE(wowee::ui::isPartyChatBubbleType(ChatType::MONSTER_SAY));
    CHECK(wowee::ui::isPartyChatBubbleType(ChatType::PARTY));
    CHECK(wowee::ui::isPartyChatBubbleType(ChatType::RAID));
}

TEST_CASE("Speech bubble metrics scale with viewport height", "[chat_bubble][metrics]") {
    const auto m720 = wowee::ui::chatBubbleMetrics(720.0f);
    CHECK(m720.scale == Catch::Approx(0.9375f));
    CHECK(m720.pixelHeight == 15);
    CHECK(m720.wrapWidthPx == Catch::Approx(296.25f));
    CHECK(m720.padX == Catch::Approx(15.0f));
    CHECK(m720.padY == Catch::Approx(15.0f));
    CHECK(m720.edge == Catch::Approx(15.0f));
    CHECK(m720.tailSize == Catch::Approx(15.0f));

    const auto m1080 = wowee::ui::chatBubbleMetrics(1080.0f);
    CHECK(m1080.scale == Catch::Approx(1080.0f / 768.0f));
    CHECK(m1080.pixelHeight == 23);
    CHECK(m1080.wrapWidthPx == Catch::Approx(316.0f * (1080.0f / 768.0f)));
}

TEST_CASE("Prophet Velen Sunwell outro line wraps into native 6 lines at 720p", "[chat_bubble][wrap]") {
    const auto face = makeFrizqtFace();
    const auto m = wowee::ui::chatBubbleMetrics(720.0f);
    wowee::ui::NativeTextMeasure measure;
    measure.face = face.get();
    measure.pixelHeight = m.pixelHeight;

    wowee::ui::WrapRun run;
    run.text = "The creature Entropius, whom you were forced to destroy, was once the noble "
               "naaru, M'uru. In life, M'uru channeled vast energies of LIGHT and HOPE. "
               "For a time, a misguided few sought to steal those energies...";
    const auto lines = wowee::ui::wrapTextNative(
        {run}, m.wrapWidthPx, false, measure, [](const wowee::ui::WrapRun&) { return 0.0f; });

    REQUIRE(lines.size() == 6);
    CHECK(joinTrimmed(lines[0]) == "The creature Entropius, whom you were");
    CHECK(joinTrimmed(lines[1]) == "forced to destroy, was once the noble");
    CHECK(joinTrimmed(lines[2]) == "naaru, M'uru. In life, M'uru channeled vast");
    CHECK(joinTrimmed(lines[3]) == "energies of LIGHT and HOPE. For a time,");
    CHECK(joinTrimmed(lines[4]) == "a misguided few sought to steal those");
    CHECK(joinTrimmed(lines[5]) == "energies...");

    float maxW = 0.0f;
    for (const auto& l : lines) {
        const std::string t = joinTrimmed(l);
        maxW = std::max(maxW, static_cast<float>(measure.width(t)));
    }
    const auto box = wowee::ui::computeChatBubbleBox(640.0f, 180.0f, maxW, static_cast<int>(lines.size()), m);
    // Native 1280x720 Velen bubble: ~320px visible width, ~117-120px height.
    CHECK(box.width == Catch::Approx(maxW + 30.0f));
    CHECK(box.height == Catch::Approx(120.0f));
    // Background fill stays strictly inside the 9-slice edge band.
    CHECK(box.bgX0 == Catch::Approx(box.x0 + m.edge));
    CHECK(box.bgY0 == Catch::Approx(box.y0 + m.edge));
    CHECK(box.bgX1 == Catch::Approx(box.x1 - m.edge));
    CHECK(box.bgY1 == Catch::Approx(box.y1 - m.edge));
    // Tail is edge x edge (15x15 at 720p) and starts slightly above bottom edge.
    CHECK((box.tailX1 - box.tailX0) == Catch::Approx(15.0f));
    CHECK((box.tailY1 - box.tailY0) == Catch::Approx(15.0f));
    CHECK(box.tailY0 < box.y1);
    CHECK(box.tailY1 > box.y1);
}

TEST_CASE("Short single-line bubble respects 9-slice minimum dimensions", "[chat_bubble][box]") {
    const auto m = wowee::ui::chatBubbleMetrics(720.0f);
    const auto box = wowee::ui::computeChatBubbleBox(640.0f, 300.0f, 12.0f, 1, m);
    CHECK(box.width >= m.edge * 2.0f);
    CHECK(box.height >= m.edge * 2.0f);
    CHECK(box.bgX1 >= box.bgX0);
    CHECK(box.bgY1 >= box.bgY0);
}
