#pragma once
#include "check.h"
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace test_support {
struct Scenario {
  const char* name;
  void (*body)();
};
template <std::size_t N>
int scenarios(const Scenario (&items)[N]) {
  for (const auto& item : items) {
    std::cout.flush();
    const auto child = fork();
    CHECK(child >= 0, "fork scenario");
    if (child == 0) {
      item.body();
      return 0;
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child, "wait scenario");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      std::cerr << "scenario failed: " << item.name << '\n';
      return 1;
    }
  }
  std::cout << N << " platform integration scenarios: PASS\n";
  return 0;
}
}  // namespace test_support
