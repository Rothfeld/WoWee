#include <catch_amalgamated.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "addons/stored_cvars.hpp"

using wowee::addons::StoredCVars;

// storedCVarValue used to open cvars.cfg for every key that had no row, at any
// time. The nameplate pass asks for about eight such keys per unit per frame,
// which with 93 units around was 650 opens of the file and 4.2 ms a frame.
// The rule now: before the rows are loaded the file answers, afterwards the
// rows do and the file is left alone. These pin both halves.

namespace {

struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& content) {
        path = std::filesystem::temp_directory_path() /
               ("wowee_test_stored_cvars_" + std::to_string(std::hash<std::string>{}(content)) + ".cfg");
        write(content);
    }
    void write(const std::string& content) const {
        std::ofstream out(path);
        REQUIRE(out.is_open());
        out << content;
    }
    ~TempFile() { std::filesystem::remove(path); }
};

} // namespace

TEST_CASE("Before the rows are loaded the file answers", "[stored_cvars]") {
    const TempFile file("unitnamenpc=0\nuiScale=1.28\nbroken line\n=novalue\n");
    const StoredCVars stored;
    REQUIRE_FALSE(stored.loaded());
    CHECK(stored.value("unitNameNPC", "1", file.path.string()) == "0");
    // Keys compare without case, in the file and in the question.
    CHECK(stored.value("UISCALE", "1", file.path.string()) == "1.28");
    CHECK(stored.value("nameplateShowEnemyTotems", "1", file.path.string()) == "1");
    CHECK(stored.value("broken line", "fallback", file.path.string()) == "fallback");
}

TEST_CASE("Before the rows are loaded a missing file gives the fallback", "[stored_cvars]") {
    const StoredCVars stored;
    CHECK(stored.value("unitNameNPC", "1", "/nonexistent/wowee/cvars.cfg") == "1");
}

TEST_CASE("Loading reads every row and lower-cases its key", "[stored_cvars]") {
    const TempFile file("unitnamenpc=0\nuiScale=1.28\nbroken line\n=novalue\nempty=\nwith=two=signs\n");
    StoredCVars stored;
    CHECK(stored.load(file.path.string()) == 4);
    CHECK(stored.loaded());
    CHECK(stored.rows().size() == 4);
    CHECK(stored.rows().at("unitnamenpc") == "0");
    CHECK(stored.rows().at("uiscale") == "1.28");
    CHECK(stored.rows().at("empty").empty());
    CHECK(stored.rows().at("with") == "two=signs");
}

TEST_CASE("After loading, the rows answer and the file is not read", "[stored_cvars]") {
    const TempFile file("unitnamenpc=0\n");
    StoredCVars stored;
    stored.load(file.path.string());

    CHECK(stored.value("unitNameNPC", "1", file.path.string()) == "0");
    CHECK(stored.value("nameplateShowEnemyTotems", "1", file.path.string()) == "1");

    // A row set in memory answers; the file still says otherwise.
    stored.rows()["unitnamenpc"] = "1";
    CHECK(stored.value("unitNameNPC", "0", file.path.string()) == "1");

    // A key that appears in the file afterwards is not seen: the file is
    // written from the rows, never the other way round, once they are loaded.
    file.write("unitnamenpc=0\nnameplateshowenemytotems=0\n");
    CHECK(stored.value("nameplateShowEnemyTotems", "1", file.path.string()) == "1");

    // Nor is the path needed at all.
    CHECK(stored.value("nameplateShowEnemyTotems", "1", std::string()) == "1");
    CHECK(stored.value("unitNameNPC", "0", std::string()) == "1");
}

TEST_CASE("Loading a missing file leaves no rows and still ends the file's turn", "[stored_cvars]") {
    StoredCVars stored;
    CHECK(stored.load("/nonexistent/wowee/cvars.cfg") == 0);
    CHECK(stored.loaded());
    CHECK(stored.rows().empty());
    CHECK(stored.value("unitNameNPC", "1", "/nonexistent/wowee/cvars.cfg") == "1");
}

TEST_CASE("Every unstored key keeps the fallback its reader passes", "[stored_cvars]") {
    // The keys the nameplate pass asks for per unit, with the defaults it passes.
    const TempFile file("uiscale=1\n");
    StoredCVars loaded;
    loaded.load(file.path.string());
    const StoredCVars early;
    const std::pair<const char*, const char*> asked[] = {
        {"nameplateShowEnemyTotems", "1"}, {"nameplateShowFriendlyTotems", "0"},
        {"nameplateShowEnemyPets", "1"}, {"nameplateShowFriendlyPets", "1"},
        {"nameplateShowEnemyGuardians", "1"}, {"nameplateShowFriendlyGuardians", "1"},
        {"unitNameNPC", "1"}, {"unitNameOwn", "0"}, {"nameplateAllowOverlap", "0"},
        {"unitNamePlayerGuild", "1"}, {"unitNamePlayerPvPTitle", "1"},
    };
    for (const auto& [key, fallback] : asked) {
        INFO(key);
        // The same answer the file gave before the rows were loaded.
        CHECK(loaded.value(key, fallback, file.path.string()) == early.value(key, fallback, file.path.string()));
        CHECK(loaded.value(key, fallback, file.path.string()) == fallback);
    }
}
