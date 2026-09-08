#include "hft/check.hpp"
// Intentionally fails — used manually to prove Release builds still enforce checks.
int main() {
    CHECK(1 == 0, "deliberate failure must not pass under Release");
    TEST_EXIT();
}
