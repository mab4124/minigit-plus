#include <iostream>
#include <string>
#include <vector>

#include "minigit/cli.hpp"

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  return minigit::run_cli(args, std::cout, std::cerr);
}
