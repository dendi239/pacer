#include <iostream>

struct IamBool {
  bool value;

  operator bool() const {
    std::cerr << "Got converted to bool\n";
    return value;
  }

  ~IamBool() noexcept { std::cerr << "Doing something destructor like\n"; }
};

int main() {
  std::cerr << "Begin something\n";
  if (IamBool{true}) {
    std::cerr << "doing stuff with my bool\n";
  }
  std::cerr << "Finished stuff, let's see what happens when we skip\n";
  if (IamBool{false}) {
    std::cerr << "not doing stuff\n";
  }
  std::cerr << "exiting\n";
}
