#include <windows.h>
// A deterministic unit-test keyboard state; no native input is injected.
static SHORT WINAPI SnapshotTestKeyState(int) { return 0; }
#define GetKeyState SnapshotTestKeyState
#include "../src/tsf_client/text_input_processor.cpp"
