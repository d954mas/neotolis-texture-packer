/* Masked-second-include fixture: a view TU that names TWO forbidden platform
 * headers. windows.h is a real pre-SR-BASE debt symbol (gui_view_chrome.c is
 * allowed to name it), so under a single whole-file MATCH it was the only hit
 * reported and tinyfiledialogs.h rode in behind an allowance that never
 * covered it. The rule must report BOTH -- the registering ctest asserts two
 * hits, which is exactly one more than the pre-fix scan could produce. */
#include <windows.h>
#include "tinyfiledialogs.h"
