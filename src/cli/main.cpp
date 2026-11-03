#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <iomanip>
#include <windows.h>
#include <shellapi.h>
#include "../core/engine/ime_engine.h"
#include "../core/types.h"

using namespace ime;

static void PrintHelp() {
    std::cout << "=================================================================\n";
    std::cout << "  PersonalIME CLI Debugger & Interactive REPL (Stage 2)\n";
    std::cout << "=================================================================\n";
    std::cout << "Usage:\n";
    std::cout << "  query | q <scheme> <code>               Query candidates (e.g. q pinyin zhongwen)\n";
    std::cout << "  explain <scheme> <code>                 Explain ranking scores for candidates\n";
    std::cout << "  add <text> <scheme:code>...             Add user entry (e.g. add 我的地址 custom:;dz)\n";
    std::cout << "  edit <id> <new_text> [scheme:code...]   Update text and optionally replace codes of entry\n";
    std::cout << "  del <id>                                Delete an existing user entry\n";
    std::cout << "  list                                    List all user custom entries\n";
    std::cout << "  pin <scheme> <code> <id> [rank]         Pin candidate to top (e.g. pin pinyin zhongwen u_1 1)\n";
    std::cout << "  unpin <scheme> <code> <id>              Unpin candidate\n";
    std::cout << "  commit <scheme> <code> <id> [event_id]  Simulate successful commit and update stats\n";
    std::cout << "  reset-stats [id]                        Reset learning stats for an entry or all entries\n";
    std::cout << "  export <file.json>                      Export user data to UTF-8 JSON\n";
    std::cout << "  import <file.json> [merge|replace]      Import user data from JSON with rollback\n";
    std::cout << "  bench                                   Run performance benchmark (latency & load)\n";
    std::cout << "  help                                    Print this help message\n";
    std::cout << "  exit | quit                             Exit CLI\n";
    std::cout << "=================================================================\n";
}

static std::vector<std::string> SplitArgs(const std::string& line) {
    std::vector<std::string> args;
    std::istringstream iss(line);
    std::string token;
    while (iss >> std::quoted(token)) {
        args.push_back(token);
    }
    return args;
}

struct CommandResult {
    bool shouldQuit = false;
    bool success = true;
};

static CommandResult ExecuteCommand(ImeEngine& engine, const std::vector<std::string>& args) {
    if (args.empty()) return {false, true};

    const std::string& cmd = args[0];

    if (cmd == "exit" || cmd == "quit") {
        return {true, true};
    }

    if (cmd == "help") {
        PrintHelp();
        return {false, true};
    }

    if (cmd == "query" || cmd == "q") {
        if (args.size() < 3) {
            std::cout << "Usage: query <pinyin|wubi|custom> <code>\n";
            return {false, false};
        }
        InputScheme sch = SchemeFromString(args[1]);
        if (sch == InputScheme::Unknown) {
            std::cout << "[ERROR] Unknown input scheme: " << args[1] << "\n";
            return {false, false};
        }
        std::string code = args[2];
        auto res = engine.Query(sch, code);
        std::cout << "Candidates for [" << SchemeToString(sch) << "] '" << code << "' (" << res.candidates.size() << " found):\n";
        int idx = 1;
        for (const auto& c : res.candidates) {
            std::cout << "  " << idx++ << ". " << WideToUtf8(c.text);
            if (c.is_pinned) std::cout << " [PINNED #" << c.order_rank << "]";
            if (c.selected_count > 0) std::cout << " [Used: " << c.selected_count << " times]";
            std::cout << " (ID: " << c.entry_id << ", " << MatchQualityToString(c.quality) << ", Score: " << std::fixed << std::setprecision(1) << c.score << ")\n";
        }
        return {false, true};
    }

    if (cmd == "explain") {
        if (args.size() < 3) {
            std::cout << "Usage: explain <pinyin|wubi|custom> <code>\n";
            return {false, false};
        }
        InputScheme sch = SchemeFromString(args[1]);
        if (sch == InputScheme::Unknown) {
            std::cout << "[ERROR] Unknown input scheme: " << args[1] << "\n";
            return {false, false};
        }
        std::string code = args[2];
        auto explanations = engine.ExplainCandidates(sch, code);
        std::cout << "Explain ranking for [" << SchemeToString(sch) << "] '" << code << "':\n";
        for (const auto& line : explanations) {
            std::cout << "  " << line << "\n";
        }
        return {false, true};
    }

    if (cmd == "add") {
        if (args.size() < 3) {
            std::cout << "Usage: add <text> <scheme:code> [scheme:code...]\n";
            return {false, false};
        }
        std::wstring text = Utf8ToWide(args[1]);
        std::vector<std::pair<InputScheme, std::string>> codes;
        for (size_t i = 2; i < args.size(); ++i) {
            auto pos = args[i].find(':');
            if (pos != std::string::npos) {
                std::string schStr = args[i].substr(0, pos);
                std::string codeStr = args[i].substr(pos + 1);
                codes.emplace_back(SchemeFromString(schStr), codeStr);
            } else {
                codes.emplace_back(InputScheme::Pinyin, args[i]);
            }
        }
        std::string id = engine.AddUserEntry(text, codes);
        if (!id.empty()) {
            std::cout << "[SUCCESS] Added user entry ID: " << id << "\n";
            return {false, true};
        } else {
            std::cout << "[ERROR] Failed to add user entry.\n";
            return {false, false};
        }
    }

    if (cmd == "edit") {
        if (args.size() < 3) {
            std::cout << "Usage: edit <id> <new_text> [scheme:code ...]\n";
            return {false, false};
        }
        std::string id = args[1];
        std::wstring text = Utf8ToWide(args[2]);
        std::vector<std::pair<InputScheme, std::string>> newCodes;
        for (size_t i = 3; i < args.size(); ++i) {
            auto pos = args[i].find(':');
            if (pos != std::string::npos) {
                std::string schStr = args[i].substr(0, pos);
                std::string codeStr = args[i].substr(pos + 1);
                newCodes.emplace_back(SchemeFromString(schStr), codeStr);
            } else {
                newCodes.emplace_back(InputScheme::Pinyin, args[i]);
            }
        }
        if (engine.EditUserEntry(id, text, newCodes)) {
            std::cout << "[SUCCESS] Entry " << id << " updated successfully.\n";
            return {false, true};
        } else {
            std::cout << "[ERROR] Failed to update entry " << id << ".\n";
            return {false, false};
        }
    }

    if (cmd == "del") {
        if (args.size() < 2) {
            std::cout << "Usage: del <id>\n";
            return {false, false};
        }
        std::string id = args[1];
        if (engine.DeleteUserEntry(id)) {
            std::cout << "[SUCCESS] Entry " << id << " deleted.\n";
            return {false, true};
        } else {
            std::cout << "[ERROR] Failed to delete entry " << id << ".\n";
            return {false, false};
        }
    }

    if (cmd == "list") {
        auto list = engine.GetAllUserEntries();
        std::cout << "Total user entries: " << list.size() << "\n";
        for (const auto& e : list) {
            std::cout << "  - ID: " << e.id << " | Text: " << WideToUtf8(e.text) << " | Codes: ";
            for (const auto& [sch, code] : e.codes) {
                std::cout << SchemeToString(sch) << ":" << code << " ";
            }
            std::cout << "\n";
        }
        return {false, true};
    }

    if (cmd == "pin") {
        if (args.size() < 4) {
            std::cout << "Usage: pin <scheme> <code> <id> [rank]\n";
            return {false, false};
        }
        InputScheme sch = SchemeFromString(args[1]);
        std::string code = args[2];
        std::string id = args[3];
        int rank = (args.size() >= 5) ? std::stoi(args[4]) : 1;
        if (engine.PinCandidate(sch, code, id, rank)) {
            std::cout << "[SUCCESS] Pinned candidate " << id << " to rank " << rank << " under " << code << ".\n";
            return {false, true};
        } else {
            std::cout << "[ERROR] Failed to pin candidate.\n";
            return {false, false};
        }
    }

    if (cmd == "unpin") {
        if (args.size() < 4) {
            std::cout << "Usage: unpin <scheme> <code> <id>\n";
            return {false, false};
        }
        InputScheme sch = SchemeFromString(args[1]);
        std::string code = args[2];
        std::string id = args[3];
        if (engine.UnpinCandidate(sch, code, id)) {
            std::cout << "[SUCCESS] Unpinned candidate " << id << " under " << code << ".\n";
            return {false, true};
        } else {
            std::cout << "[ERROR] Failed to unpin candidate.\n";
            return {false, false};
        }
    }

    if (cmd == "commit") {
        if (args.size() < 4) {
            std::cout << "Usage: commit <scheme> <code> <id> [event_id]\n";
            return {false, false};
        }
        InputScheme sch = SchemeFromString(args[1]);
        std::string code = args[2];
        std::string id = args[3];
        std::string eventId = (args.size() >= 5) ? args[4] : "";
        if (engine.CommitCandidate(eventId, sch, code, id)) {
            std::cout << "[SUCCESS] Commit recorded for " << id << " under " << code << ".\n";
            return {false, true};
        } else {
            std::cout << "[ERROR] Failed to record commit.\n";
            return {false, false};
        }
    }

    if (cmd == "reset-stats") {
        std::string id = (args.size() >= 2) ? args[1] : "";
        if (engine.ResetStats(id)) {
            std::cout << "[SUCCESS] Stats reset " << (id.empty() ? "for all entries." : ("for entry " + id + ".")) << "\n";
            return {false, true};
        } else {
            std::cout << "[ERROR] Failed to reset stats.\n";
            return {false, false};
        }
    }

    if (cmd == "export") {
        if (args.size() < 2) {
            std::cout << "Usage: export <file.json>\n";
            return {false, false};
        }
        std::string path = args[1];
        std::string err;
        if (engine.ExportData(path, &err)) {
            std::cout << "[SUCCESS] Exported user data to " << path << "\n";
            return {false, true};
        } else {
            std::cout << "[ERROR] Export failed: " << err << "\n";
            return {false, false};
        }
    }

    if (cmd == "import") {
        if (args.size() < 2) {
            std::cout << "Usage: import <file.json> [merge|replace]\n";
            return {false, false};
        }
        std::string path = args[1];
        bool replace = (args.size() >= 3 && args[2] == "replace");
        std::string err;
        if (engine.ImportData(path, replace, &err)) {
            std::cout << "[SUCCESS] Imported user data (" << (replace ? "Replace" : "Merge") << " mode) from " << path << "\n";
            return {false, true};
        } else {
            std::cout << "[ERROR] Import failed: " << err << "\n";
            return {false, false};
        }
    }

    if (cmd == "bench") {
        std::cout << "Running benchmark on 1,000 queries...\n";
        LARGE_INTEGER freq, start, end;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);

        for (int i = 0; i < 500; ++i) {
            engine.Query(InputScheme::Pinyin, "zhongwen");
            engine.Query(InputScheme::Wubi86, "aaaa");
        }

        QueryPerformanceCounter(&end);
        double totalMs = (end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
        double avgUs = (totalMs * 1000.0) / 1000.0;
        std::cout << "1,000 queries completed in " << totalMs << " ms (Average: " << avgUs << " us per query!)\n";
        return {false, true};
    }

    std::cout << "Unknown command: " << cmd << ". Type 'help' for usage.\n";
    return {false, false};
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::vector<std::string> allArgs;
    if (wargv) {
        for (int i = 0; i < wargc; ++i) {
            allArgs.push_back(WideToUtf8(wargv[i]));
        }
        LocalFree(wargv);
    }

    std::string dbPath = "personal_ime.db";
    std::string dictDir = "data/dictionaries";

    // Parse options if provided
    std::vector<std::string> cmdArgs;
    for (size_t i = 1; i < allArgs.size(); ++i) {
        std::string arg = allArgs[i];
        if (arg == "--db" && i + 1 < allArgs.size()) {
            dbPath = allArgs[++i];
        } else if (arg == "--dict" && i + 1 < allArgs.size()) {
            dictDir = allArgs[++i];
        } else {
            cmdArgs.push_back(arg);
        }
    }

    ImeEngine engine;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    if (!engine.Initialize(dbPath, dictDir)) {
        std::cerr << "[FATAL] Failed to initialize PersonalIME Engine with DB: " << dbPath << ", Dict: " << dictDir << "\n";
        return 1;
    }

    QueryPerformanceCounter(&t1);
    double initMs = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;

    if (!cmdArgs.empty()) {
        // Direct command mode
        CommandResult res = ExecuteCommand(engine, cmdArgs);
        return res.success ? 0 : 1;
    }

    // Interactive REPL Mode
    std::cout << "PersonalIME Interactive CLI Debugger\n";
    std::cout << "Database: " << dbPath << "\n";
    std::cout << "Builtin Dictionaries: " << engine.GetBuiltinDict().WubiEntryCount() << " wubi, "
              << engine.GetBuiltinDict().PinyinEntryCount() << " pinyin entries loaded in " << initMs << " ms.\n";
    std::cout << "Type 'help' for command list, 'exit' to quit.\n\n";

    std::string line;
    while (true) {
        std::cout << "PersonalIME> ";
        if (!std::getline(std::cin, line)) {
            break;
        }
        auto args = SplitArgs(line);
        CommandResult res = ExecuteCommand(engine, args);
        if (res.shouldQuit) {
            break;
        }
    }

    return 0;
}

