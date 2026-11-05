#include <io.h>
static bool failSync = true;
static int syncCalls = 0;
static int InjectedCommit(int fd) {
    ++syncCalls;
    return failSync ? -1 : ::_commit(fd);
}
// Only this test translation unit replaces the real OS durability call.
#define _commit InjectedCommit
#include "tsf_client/receipt_journal.h"
#undef _commit
#include <iostream>
#define REQUIRE(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " << #x << '\n'; return 1; } } while (false)
int main() {
    auto dir = std::filesystem::absolute("receipt-durability-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    SetEnvironmentVariableW(L"TEMP", dir.c_str()); SetEnvironmentVariableW(L"TMP", dir.c_str());
    receipt_journal::Record r{1, 2, 3, 0, 0};
    REQUIRE(!receipt_journal::Append({r}));
    // The full frame is readable despite the rejected physical sync.
    std::vector<receipt_journal::Record> rows;
    REQUIRE(receipt_journal::Read(rows) && rows.size() == 1);
    REQUIRE(!receipt_journal::Append({r}));
    REQUIRE(syncCalls == 2);
    failSync = false;
    REQUIRE(receipt_journal::Append({r}));
    REQUIRE(syncCalls == 3);
    REQUIRE(receipt_journal::Read(rows) && rows.size() == 1 && receipt_journal::Same(rows[0], r));
    REQUIRE(receipt_journal::Ack(r.eid));
    REQUIRE(receipt_journal::Read(rows) && rows.empty());
    std::cout << "PASS: readable outcome after failed sync remains failed until a retry actually syncs\n";
}
